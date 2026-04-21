#include "framework/component.h"
#include "framework/hook.h"
#include "framework/ipc.h"

#include <stddef.h>

/*
 * Lifecycle state machine — see framework/component.h for the edge diagram.
 *
 * Slice 3.8 adds three things on top of the 3.3 matrix:
 *
 *   1. Hook dispatch around enable / disable / pause / resume.  Hooks run
 *      BEFORE the state transition, carrying the proposed (from, to) pair.
 *      An ABORT return causes the verb to fail with NX_EABORT without
 *      mutating state.
 *
 *   2. Pause protocol — nx_component_pause drives its bound slot(s)
 *      through CUTTING → DRAINING → DONE (cutoff, drain inbox, call
 *      ops->pause_hook, call ops->pause).  nx_component_resume runs the
 *      symmetric path: ops->resume, set NX_SLOT_PAUSE_NONE, flush the
 *      hold queue for every incoming edge.
 *
 *   3. Destroy guard — nx_component_destroy refuses (NX_EBUSY) if the
 *      component is still bound as any slot's `active`.  The guard
 *      complements nx_component_unregister's existing check and makes
 *      destroy-without-unbind impossible.
 */

bool nx_lifecycle_transition_legal(enum nx_lifecycle_state from,
                                   enum nx_lifecycle_state to)
{
    switch (from) {
    case NX_LC_UNINIT:
        return to == NX_LC_INIT || to == NX_LC_READY;
    case NX_LC_INIT:
        return to == NX_LC_READY;
    case NX_LC_READY:
        return to == NX_LC_ACTIVE || to == NX_LC_DESTROYED;
    case NX_LC_ACTIVE:
        return to == NX_LC_PAUSED || to == NX_LC_READY;
    case NX_LC_PAUSED:
        return to == NX_LC_ACTIVE || to == NX_LC_READY;
    case NX_LC_DESTROYED:
        return false;
    }
    return false;
}

const char *nx_lifecycle_state_name(enum nx_lifecycle_state s)
{
    switch (s) {
    case NX_LC_UNINIT:    return "uninit";
    case NX_LC_INIT:      return "init";
    case NX_LC_READY:     return "ready";
    case NX_LC_ACTIVE:    return "active";
    case NX_LC_PAUSED:    return "paused";
    case NX_LC_DESTROYED: return "destroyed";
    }
    return "unknown";
}

/* ---------- Hook dispatch helper --------------------------------------- */

static int dispatch_lifecycle_hook(enum nx_hook_point point,
                                   struct nx_component *c,
                                   enum nx_lifecycle_state to)
{
    struct nx_hook_context ctx = {
        .point = point,
        .u.comp = { .comp = c, .from = c->state, .to = to },
    };
    return nx_hook_dispatch(&ctx) == NX_HOOK_ABORT ? NX_EABORT : NX_OK;
}

/* Single-step transition helper.  `expected_from` is the only legal current
 * state; any other value (including an unknown component caught by
 * state_set's NX_ENOENT path) returns NX_ESTATE / NX_ENOENT / NX_EINVAL
 * as appropriate.  We consult the matrix as a belt-and-suspenders check so
 * a typo in a caller's expected_from can't silently bypass it. */
static int transition_from(struct nx_component *c,
                           enum nx_lifecycle_state expected_from,
                           enum nx_lifecycle_state to)
{
    if (!c) return NX_EINVAL;
    if (c->state != expected_from) return NX_ESTATE;
    if (!nx_lifecycle_transition_legal(expected_from, to)) return NX_ESTATE;
    return nx_component_state_set(c, to);
}

int nx_component_init(struct nx_component *c)
{
    /* v1 synchronous init: UNINIT → READY in a single step.  The INIT
     * state is legal-but-unused until async bring-up is added.
     * ops->init runs before the state transition so a failing init
     * leaves the component in UNINIT — free to retry. */
    if (!c) return NX_EINVAL;
    if (c->state != NX_LC_UNINIT) return NX_ESTATE;
    if (c->descriptor && c->descriptor->ops && c->descriptor->ops->init) {
        int orc = c->descriptor->ops->init(c->impl);
        if (orc != NX_OK) return orc;
    }
    return transition_from(c, NX_LC_UNINIT, NX_LC_READY);
}

int nx_component_enable(struct nx_component *c)
{
    if (!c) return NX_EINVAL;
    if (c->state != NX_LC_READY) return NX_ESTATE;
    int rc = dispatch_lifecycle_hook(NX_HOOK_COMPONENT_ENABLE, c, NX_LC_ACTIVE);
    if (rc != NX_OK) return rc;
    if (c->descriptor && c->descriptor->ops && c->descriptor->ops->enable) {
        int orc = c->descriptor->ops->enable(c->impl);
        if (orc != NX_OK) return orc;
    }
    return transition_from(c, NX_LC_READY, NX_LC_ACTIVE);
}

/* ---------- Pause / resume --------------------------------------------- */

static void slot_cutoff_cb(struct nx_slot *s, void *ctx)
{
    (void)ctx;
    nx_slot_set_pause_state(s, NX_SLOT_PAUSE_CUTTING);
}

static void slot_drain_cb(struct nx_slot *s, void *ctx)
{
    (void)ctx;
    nx_slot_set_pause_state(s, NX_SLOT_PAUSE_DRAINING);
    /* Drain every queued message synchronously.  Host-side v1: dispatch
     * runs on the caller's thread.  Slice 3.9 replaces this with a wait
     * on the dispatcher-thread's drain-complete signal. */
    nx_ipc_dispatch(s, (size_t)-1);
}

static void slot_done_cb(struct nx_slot *s, void *ctx)
{
    (void)ctx;
    nx_slot_set_pause_state(s, NX_SLOT_PAUSE_DONE);
}

static void slot_resume_cb(struct nx_slot *s, void *ctx)
{
    (void)ctx;
    nx_slot_set_pause_state(s, NX_SLOT_PAUSE_NONE);
    nx_ipc_flush_hold_queue(s);
}

int nx_component_pause(struct nx_component *c)
{
    if (!c) return NX_EINVAL;
    if (c->state != NX_LC_ACTIVE) return NX_ESTATE;

    int rc = dispatch_lifecycle_hook(NX_HOOK_COMPONENT_PAUSE, c, NX_LC_PAUSED);
    if (rc != NX_OK) return rc;

    /* Pause protocol.  Cutoff → drain → pause_hook → ops->pause → DONE.
     * A component may be bound to zero slots (registered but not yet
     * wired in), in which case the _bound_slot walks are no-ops — the
     * lifecycle transition still happens so recomposition can keep
     * progressing. */
    nx_component_foreach_bound_slot(c, slot_cutoff_cb, NULL);
    nx_component_foreach_bound_slot(c, slot_drain_cb,  NULL);

    if (c->descriptor && c->descriptor->ops) {
        if (c->descriptor->ops->pause_hook) {
            int hrc = c->descriptor->ops->pause_hook(c->impl);
            if (hrc != NX_OK) return hrc;
        }
        if (c->descriptor->ops->pause) {
            int orc = c->descriptor->ops->pause(c->impl);
            if (orc != NX_OK) return orc;
        }
    }

    nx_component_foreach_bound_slot(c, slot_done_cb, NULL);
    return transition_from(c, NX_LC_ACTIVE, NX_LC_PAUSED);
}

int nx_component_resume(struct nx_component *c)
{
    if (!c) return NX_EINVAL;
    if (c->state != NX_LC_PAUSED) return NX_ESTATE;

    int rc = dispatch_lifecycle_hook(NX_HOOK_COMPONENT_RESUME, c, NX_LC_ACTIVE);
    if (rc != NX_OK) return rc;

    if (c->descriptor && c->descriptor->ops && c->descriptor->ops->resume) {
        int orc = c->descriptor->ops->resume(c->impl);
        if (orc != NX_OK) return orc;
    }

    /* Clear pause state and flush each slot's hold queue before the
     * component is officially ACTIVE again.  Messages held during pause
     * thus run before anything that was queued after resume. */
    nx_component_foreach_bound_slot(c, slot_resume_cb, NULL);
    return transition_from(c, NX_LC_PAUSED, NX_LC_ACTIVE);
}

int nx_component_disable(struct nx_component *c)
{
    if (!c) return NX_EINVAL;
    if (c->state != NX_LC_ACTIVE && c->state != NX_LC_PAUSED)
        return NX_ESTATE;
    int rc = dispatch_lifecycle_hook(NX_HOOK_COMPONENT_DISABLE, c, NX_LC_READY);
    if (rc != NX_OK) return rc;
    if (c->descriptor && c->descriptor->ops && c->descriptor->ops->disable) {
        int orc = c->descriptor->ops->disable(c->impl);
        if (orc != NX_OK) return orc;
    }
    return nx_component_state_set(c, NX_LC_READY);
}

int nx_component_destroy(struct nx_component *c)
{
    if (!c) return NX_EINVAL;
    if (c->state != NX_LC_READY) return NX_ESTATE;
    /* Destroy guard: the component must be fully unbound first.
     * Matches nx_component_unregister's check — without this, a caller
     * could run `destroy()` while a slot still points at the component,
     * leaving a dangling `active` pointer after the subsequent
     * unregister path. */
    if (nx_component_is_bound(c)) return NX_EBUSY;
    if (c->descriptor && c->descriptor->ops && c->descriptor->ops->destroy) {
        c->descriptor->ops->destroy(c->impl);
    }
    return transition_from(c, NX_LC_READY, NX_LC_DESTROYED);
}

/* ---------- Dependency resolution (slice 3.4) -------------------------- */

/*
 * Walk the descriptor's dep list once.  For each dep we look up the slot
 * by name, then:
 *
 *   - missing + required       → NX_ENOENT (leaves previously-written
 *                                 pointers alone; caller is expected to
 *                                 reset component state before retrying)
 *   - missing + optional       → leave the slot pointer as-is, skip the
 *                                 connection edge
 *   - found                    → write the pointer, register a connection
 *                                 edge from self_slot to the target
 *
 * `self_slot` may be NULL to signal a boot/external entry edge (the
 * registry accepts from_slot == NULL as the boot-edge marker).  Writing
 * the pointer happens *before* `connection_register` so a later ENOMEM
 * from the registry doesn't leave the graph registered without the
 * component field populated.
 */
int nx_resolve_deps(const struct nx_component_descriptor *d,
                    struct nx_slot *self_slot,
                    void *state)
{
    if (!d || !state) return NX_EINVAL;

    for (size_t i = 0; i < d->n_deps; i++) {
        const struct nx_dep_descriptor *dep = &d->deps[i];
        struct nx_slot *tgt = nx_slot_lookup(dep->name);

        if (!tgt) {
            if (dep->required) return NX_ENOENT;
            continue;
        }

        struct nx_slot **field = (struct nx_slot **)
            ((char *)state + dep->offset);
        *field = tgt;

        int err = NX_OK;
        nx_connection_register(self_slot, tgt,
                               dep->mode, dep->stateful, dep->policy, &err);
        if (err != NX_OK) return err;
    }
    return NX_OK;
}
