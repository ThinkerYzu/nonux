/*
 * framework/recompose.c — runtime recomposition orchestrator (slice 8.2).
 *
 * Five-phase protocol (DESIGN.md §Recomposition Protocol):
 *
 *   1. VALIDATE   — reject FROZEN slots, wrong new_comp states
 *   2. PAUSE      — topological order (dependents first); cutoff→drain→done
 *   3. REWIRE     — remove edges, swap comps, add edges
 *   4. RESUME     — enable new / resume paused; flush hold queues (reverse order)
 *   5. NOTIFY     — fire on_dep_swapped on upstream dependents
 *
 * timer_pause()/timer_resume() bracket steps 2–4 on the kernel build.
 */

#include "framework/recompose.h"

#include "framework/component.h"
#include "framework/ipc.h"
#include "framework/registry.h"

#if !__STDC_HOSTED__
#include "core/timer/timer.h"
#endif

#include <stdbool.h>
#include <stddef.h>

/* ---- Topo-sort constants ----------------------------------------------- */

#define RECOMP_MAX_SLOTS  32
#define RECOMP_MAX_EDGES  (RECOMP_MAX_SLOTS * RECOMP_MAX_SLOTS)

/* ---- Topo sort: Kahn's algorithm on the affected-slot subgraph --------- *
 *
 * Edge direction mirrors the registry: A → B means A depends on B (A calls B).
 * Kahn's starts with nodes of in-degree 0 — those that nothing in the plan
 * calls.  That gives dependents before dependencies, which is the pause order.
 */

struct scan_dep_ctx {
    struct nx_slot **set;
    int              n_set;
    int              src_idx;
    int            (*edges)[2];   /* [n][0]=from_idx, [n][1]=to_idx */
    int             *n_edges;
    int             *deg;         /* in-degree within set */
    bool             overflow;
};

static void scan_dep_cb(struct nx_connection *conn, void *vctx)
{
    struct scan_dep_ctx *ctx = vctx;
    for (int j = 0; j < ctx->n_set; j++) {
        if (ctx->set[j] != conn->to_slot) continue;
        if (*ctx->n_edges >= RECOMP_MAX_EDGES) { ctx->overflow = true; return; }
        ctx->edges[*ctx->n_edges][0] = ctx->src_idx;
        ctx->edges[*ctx->n_edges][1] = j;
        (*ctx->n_edges)++;
        ctx->deg[j]++;
        return;
    }
}

static int build_pause_order(const struct recomp_plan *plan,
                              struct nx_slot **order, int *n_order)
{
    struct nx_slot *set[RECOMP_MAX_SLOTS];
    int             deg[RECOMP_MAX_SLOTS];
    bool            done[RECOMP_MAX_SLOTS];
    int             edges[RECOMP_MAX_EDGES][2];
    int             n = 0, n_edges = 0;

    for (int i = 0; i < plan->num_changes; i++) {
        if (n >= RECOMP_MAX_SLOTS) return NX_ENOMEM;
        set[n++] = plan->changes[i].slot;
    }
    for (int i = 0; i < n; i++) { deg[i] = 0; done[i] = false; }

    struct scan_dep_ctx sc = {
        .set      = set,
        .n_set    = n,
        .edges    = edges,
        .n_edges  = &n_edges,
        .deg      = deg,
        .overflow = false,
    };
    for (int i = 0; i < n; i++) {
        sc.src_idx = i;
        nx_slot_foreach_dependency(set[i], scan_dep_cb, &sc);
        if (sc.overflow) return NX_ENOMEM;
    }

    *n_order = 0;
    while (*n_order < n) {
        bool progress = false;
        for (int i = 0; i < n; i++) {
            if (done[i] || deg[i] != 0) continue;
            order[(*n_order)++] = set[i];
            done[i] = true;
            progress = true;
            for (int e = 0; e < n_edges; e++)
                if (edges[e][0] == i) deg[edges[e][1]]--;
        }
        if (!progress) return NX_EINVAL; /* cycle in affected set */
    }
    return NX_OK;
}

/* ---- Connection-find helper ------------------------------------------- */

struct conn_find_ctx {
    struct nx_slot       *from;
    struct nx_slot       *to;
    struct nx_connection *found;
};

static void conn_find_cb(struct nx_connection *conn, void *vctx)
{
    struct conn_find_ctx *ctx = vctx;
    if (!ctx->found &&
        conn->from_slot == ctx->from && conn->to_slot == ctx->to)
        ctx->found = conn;
}

/* ---- Dep-swap notification -------------------------------------------- */

struct dep_notify_ctx {
    struct nx_slot      *changed_slot;
    struct nx_component *old_comp;
    struct nx_component *new_comp;
    uint32_t             flags;
};

static void dep_notify_cb(struct nx_connection *conn, void *vctx)
{
    struct dep_notify_ctx *ctx = vctx;
    struct nx_slot *from = conn->from_slot;
    if (!from || !from->active) return;
    struct nx_component *c = from->active;
    if (!c->descriptor || !c->descriptor->ops) return;
    if (!c->descriptor->ops->on_dep_swapped) return;
    c->descriptor->ops->on_dep_swapped(c->impl,
                                       ctx->changed_slot,
                                       ctx->old_comp,
                                       ctx->new_comp,
                                       ctx->flags);
}

/* ---- Post-replace slot cleanup ---------------------------------------- *
 *
 * After swapping in a new component and enabling it, the slot's pause_state
 * is still DONE (from the old component's pause).  Clear it to NONE and flush
 * the hold queue so messages buffered during the pause reach the new component.
 *
 * QUEUE-policy blocking callers parked on resume_waitq are woken here once
 * blocking callers are exercised through the recompose path (deferred to
 * the slice that tests nx_slot_call_blocking across a live swap).
 */
static void slot_clear_pause(struct nx_slot *s)
{
    nx_slot_set_pause_state(s, NX_SLOT_PAUSE_NONE);
    nx_ipc_flush_hold_queue(s);
}

/* ---- Main orchestrator ------------------------------------------------ */

int nx_recompose(const struct recomp_plan *plan)
{
    if (!plan) return NX_EINVAL;
    if (plan->num_changes < 0 || plan->num_connections < 0) return NX_EINVAL;

    /* ---- 1. Validate ---- */
    for (int i = 0; i < plan->num_changes; i++) {
        const struct nx_slot_change *sc = &plan->changes[i];
        if (!sc->slot) return NX_EINVAL;
        if (sc->slot->mutability == NX_MUT_FROZEN) return NX_EPERM;
        if (sc->action == NX_SLOT_REPLACE) {
            if (!sc->new_comp) return NX_EINVAL;
            if (sc->new_comp->state != NX_LC_READY) return NX_ESTATE;
        }
    }

    /* ---- 2. Build topological pause order ---- */
    struct nx_slot *pause_order[RECOMP_MAX_SLOTS];
    int n_pause = 0;
    int rc = build_pause_order(plan, pause_order, &n_pause);
    if (rc != NX_OK) return rc;

    /* Save each slot's old active component before any mutation. */
    struct nx_component *old_comps[RECOMP_MAX_SLOTS];
    for (int i = 0; i < plan->num_changes; i++)
        old_comps[i] = plan->changes[i].slot->active;

    /* ---- Timer quiescence (kernel build only) ---- */
#if !__STDC_HOSTED__
    timer_pause();
#endif

    /* ---- 3. Pause: top-down (dependents first) ---- */
    for (int i = 0; i < n_pause; i++) {
        struct nx_slot *s = pause_order[i];
        if (!s->active || s->active->state != NX_LC_ACTIVE) continue;
        rc = nx_component_pause(s->active);
        if (rc != NX_OK) {
            /* Rollback: resume previously paused components in reverse. */
            for (int j = i - 1; j >= 0; j--) {
                struct nx_slot *rs = pause_order[j];
                if (rs->active && rs->active->state == NX_LC_PAUSED)
                    (void)nx_component_resume(rs->active);
            }
#if !__STDC_HOSTED__
            timer_resume();
#endif
            return rc;
        }
    }

    /* ---- 4a. REWIRE: remove connections ---- */
    for (int i = 0; i < plan->num_connections; i++) {
        const struct nx_conn_change *cc = &plan->connections[i];
        if (cc->action != NX_CONN_REMOVE) continue;
        struct conn_find_ctx fc = { cc->from_slot, cc->to_slot, NULL };
        nx_graph_foreach_connection(conn_find_cb, &fc);
        if (fc.found) (void)nx_connection_unregister(fc.found);
    }

    /* ---- 4b. REWIRE: swap components (disable old, bind new) ---- */
    for (int i = 0; i < plan->num_changes; i++) {
        const struct nx_slot_change *sc = &plan->changes[i];
        if (sc->action != NX_SLOT_REPLACE) continue;
        struct nx_component *old = old_comps[i];

        /* Disable old: PAUSED|ACTIVE → READY. */
        if (old && (old->state == NX_LC_PAUSED || old->state == NX_LC_ACTIVE))
            (void)nx_component_disable(old);

        /* Bind new component to the slot (not yet enabled). */
        (void)nx_slot_swap(sc->slot, sc->new_comp);

        /* Destroy old now that it is unbound (READY → DESTROYED). */
        if (old && old->state == NX_LC_READY && !nx_component_is_bound(old))
            (void)nx_component_destroy(old);
    }

    /* ---- 4c. REWIRE: add / retune connections ---- */
    for (int i = 0; i < plan->num_connections; i++) {
        const struct nx_conn_change *cc = &plan->connections[i];
        if (cc->action == NX_CONN_ADD) {
            (void)nx_connection_register(cc->from_slot, cc->to_slot,
                                         cc->mode, cc->stateful,
                                         cc->policy, NULL);
        } else if (cc->action == NX_CONN_REWIRE) {
            struct conn_find_ctx fc = { cc->from_slot, cc->to_slot, NULL };
            nx_graph_foreach_connection(conn_find_cb, &fc);
            if (fc.found)
                (void)nx_connection_retune(fc.found, cc->mode,
                                           fc.found->stateful);
        }
    }

    /* ---- 5. Resume: bottom-up (dependencies before dependents) ---- */
    for (int i = n_pause - 1; i >= 0; i--) {
        struct nx_slot *s = pause_order[i];
        if (!s->active) continue;
        if (s->active->state == NX_LC_READY) {
            /* Replaced component: enable it, then clear slot pause state. */
            (void)nx_component_enable(s->active);
            slot_clear_pause(s);
        } else if (s->active->state == NX_LC_PAUSED) {
            /* Existing paused component: resume handles state + hold flush. */
            (void)nx_component_resume(s->active);
        }
    }

    /* ---- Timer resume (kernel build only) ---- */
#if !__STDC_HOSTED__
    timer_resume();
#endif

    /* ---- 6. Notify dependents of each swapped slot ---- */
    for (int i = 0; i < plan->num_changes; i++) {
        const struct nx_slot_change *sc = &plan->changes[i];
        if (sc->action != NX_SLOT_REPLACE) continue;
        struct dep_notify_ctx ctx = {
            .changed_slot = sc->slot,
            .old_comp     = old_comps[i],
            .new_comp     = sc->new_comp,
            .flags        = sc->state_lost ? NX_SWAP_STATE_LOST : 0u,
        };
        nx_slot_foreach_dependent(sc->slot, dep_notify_cb, &ctx);
    }

    return NX_OK;
}
