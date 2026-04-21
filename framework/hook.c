#include "framework/hook.h"

#include <stddef.h>

/*
 * Hook framework implementation — slice 3.8.
 *
 * One singly-linked chain per hook point, heads indexed by enum.  Inserts
 * are O(chain-length) priority-ordered; dispatch walks each chain in order.
 *
 * Unregister-during-dispatch correctness:
 *
 *   A global `g_dispatching` counter is bumped for every call to
 *   `nx_hook_dispatch` and decremented on return.  Unregister checks the
 *   counter:
 *
 *     - counter == 0  →  unlink the node right away.
 *     - counter > 0   →  flag `_dead = true`; a sweep runs when the
 *                         outermost dispatch drops the counter back to 0.
 *
 *   Dispatch itself skips any node with `_dead == true`.  Because hooks
 *   may register new hooks too, the walk snapshots `_next` at each step
 *   rather than re-reading after callback return.
 */

static struct nx_hook *g_chains[NX_HOOK_POINT_COUNT];
static int             g_dispatching;

/* ---------- Registration ------------------------------------------------- */

int nx_hook_register(struct nx_hook *h)
{
    if (!h || !h->fn) return NX_EINVAL;
    if ((unsigned)h->point >= NX_HOOK_POINT_COUNT) return NX_EINVAL;

    /* Duplicate-pointer guard: same `h` can't be in the chain twice. */
    for (struct nx_hook *n = g_chains[h->point]; n; n = n->_next) {
        if (n == h) return NX_EEXIST;
    }

    h->_dead = false;

    /* Insert before the first node with priority strictly greater than
     * ours (i.e. stable: equal-priority entries preserve insertion order). */
    struct nx_hook **pp = &g_chains[h->point];
    while (*pp && (*pp)->priority <= h->priority) pp = &(*pp)->_next;
    h->_next = *pp;
    *pp      = h;
    return NX_OK;
}

static void chain_unlink(struct nx_hook *h)
{
    struct nx_hook **pp = &g_chains[h->point];
    while (*pp && *pp != h) pp = &(*pp)->_next;
    if (*pp) {
        *pp = h->_next;
        h->_next = NULL;
        h->_dead = false;
    }
}

static void sweep_dead(void)
{
    for (int p = 0; p < NX_HOOK_POINT_COUNT; p++) {
        struct nx_hook **pp = &g_chains[p];
        while (*pp) {
            if ((*pp)->_dead) {
                struct nx_hook *dead = *pp;
                *pp = dead->_next;
                dead->_next = NULL;
                dead->_dead = false;
            } else {
                pp = &(*pp)->_next;
            }
        }
    }
}

void nx_hook_unregister(struct nx_hook *h)
{
    if (!h) return;
    if ((unsigned)h->point >= NX_HOOK_POINT_COUNT) return;

    /* Is it currently in a chain? */
    bool found = false;
    for (struct nx_hook *n = g_chains[h->point]; n; n = n->_next) {
        if (n == h) { found = true; break; }
    }
    if (!found) return;

    if (g_dispatching > 0) {
        h->_dead = true;
    } else {
        chain_unlink(h);
    }
}

/* ---------- Dispatch ----------------------------------------------------- */

enum nx_hook_action nx_hook_dispatch(struct nx_hook_context *ctx)
{
    if (!ctx) return NX_HOOK_CONTINUE;
    if ((unsigned)ctx->point >= NX_HOOK_POINT_COUNT) return NX_HOOK_CONTINUE;

    g_dispatching++;
    enum nx_hook_action action = NX_HOOK_CONTINUE;

    struct nx_hook *h = g_chains[ctx->point];
    while (h) {
        /* Snapshot _next before invoking the callback — the callback
         * may register further hooks, and we don't want to re-read
         * h->_next after it potentially shifted. */
        struct nx_hook *nxt = h->_next;
        if (!h->_dead) {
            if (h->fn(ctx, h->user) == NX_HOOK_ABORT) {
                action = NX_HOOK_ABORT;
                break;
            }
        }
        h = nxt;
    }

    g_dispatching--;
    if (g_dispatching == 0) sweep_dead();
    return action;
}

/* ---------- Introspection ------------------------------------------------- */

size_t nx_hook_chain_length(enum nx_hook_point point)
{
    if ((unsigned)point >= NX_HOOK_POINT_COUNT) return 0;
    size_t n = 0;
    for (struct nx_hook *h = g_chains[point]; h; h = h->_next) {
        if (!h->_dead) n++;
    }
    return n;
}

void nx_hook_reset(void)
{
    for (int p = 0; p < NX_HOOK_POINT_COUNT; p++) {
        struct nx_hook *h = g_chains[p];
        while (h) {
            struct nx_hook *nxt = h->_next;
            h->_next = NULL;
            h->_dead = false;
            h = nxt;
        }
        g_chains[p] = NULL;
    }
    g_dispatching = 0;
}
