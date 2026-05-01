/*
 * Slice 8.0a.8 — observe-only hook that records every firing.
 *
 * Registers as a NX_HOOK_CONTINUE hook so it never aborts the chain.
 * Records the hook point, IPC src/dst slot pointers, and message pointer
 * for each firing into a fixed-size ring (overflow increments fire_count
 * but stops writing).
 *
 * Usage (all functions are static):
 *
 *   struct hook_inspector hi;
 *   hook_inspector_init(&hi, NX_HOOK_IPC_SEND, 100);   // priority 100
 *   // ... drive operations ...
 *   ASSERT_EQ_U(hi.fire_count, expected);
 *   ASSERT(hi.events[0].ipc_src == &expected_src);
 *   hook_inspector_teardown(&hi);
 */

#ifndef TEST_HOOK_INSPECTOR_H
#define TEST_HOOK_INSPECTOR_H

#include "framework/hook.h"

#include <stddef.h>
#include <string.h>

#define HOOK_INSPECTOR_MAX_EVENTS 32

/* One recorded firing. */
struct hook_event {
    enum nx_hook_point  point;
    struct nx_slot     *ipc_src;   /* u.ipc.src (IPC_SEND / IPC_RECV only) */
    struct nx_slot     *ipc_dst;   /* u.ipc.dst */
    struct nx_ipc_message *ipc_msg;
};

struct hook_inspector {
    struct nx_hook      hook;
    struct hook_event   events[HOOK_INSPECTOR_MAX_EVENTS];
    size_t              fire_count;  /* total firings (may exceed MAX_EVENTS) */
};

static enum nx_hook_action hook_inspector_fn(struct nx_hook_context *ctx,
                                              void *user)
{
    struct hook_inspector *hi = user;
    if (hi->fire_count < HOOK_INSPECTOR_MAX_EVENTS) {
        struct hook_event *e = &hi->events[hi->fire_count];
        e->point   = ctx->point;
        if (ctx->point == NX_HOOK_IPC_SEND ||
            ctx->point == NX_HOOK_IPC_RECV) {
            e->ipc_src = ctx->u.ipc.src;
            e->ipc_dst = ctx->u.ipc.dst;
            e->ipc_msg = ctx->u.ipc.msg;
        } else {
            e->ipc_src = NULL;
            e->ipc_dst = NULL;
            e->ipc_msg = NULL;
        }
    }
    hi->fire_count++;
    return NX_HOOK_CONTINUE;   /* observe-only; never aborts the chain */
}

static void hook_inspector_init(struct hook_inspector *hi,
                                 enum nx_hook_point point,
                                 int priority)
{
    memset(hi, 0, sizeof *hi);
    hi->hook.point    = point;
    hi->hook.priority = priority;
    hi->hook.fn       = hook_inspector_fn;
    hi->hook.user     = hi;
    hi->hook.name     = "hook_inspector";
    nx_hook_register(&hi->hook);
}

static void hook_inspector_teardown(struct hook_inspector *hi)
{
    nx_hook_unregister(&hi->hook);
}

static void hook_inspector_reset(struct hook_inspector *hi)
{
    hi->fire_count = 0;
    memset(hi->events, 0, sizeof hi->events);
}

#endif /* TEST_HOOK_INSPECTOR_H */
