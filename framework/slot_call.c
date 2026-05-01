/*
 * Blocking-call infrastructure — slice 8.0a (sub-sliced 8.0a.3 → 8.0a.8).
 *
 * Body landing reference: SLOT-CALL-API.md §"Body sequence" + DESIGN.md
 * §"Sync-mode caller must be on a dispatcher".  This file implements the
 * caller-side half of the round-trip (validate + pause-state read +
 * stash reply buf + IPC_SEND hook + cap-scan + dispatcher enqueue +
 * block on `task->reply_waitq`).  The dispatcher-side half — handler
 * invocation under `slot->in_flight_calls`, reply synthesis on
 * `NX_MSG_FLAG_REPLY_REQUESTED`, ABORT-path EABORT reply — lives in
 * `framework/dispatcher.c`.  The reply payload's transit into the
 * caller's wrapper-allocated reply buffer is handled by
 * `posix_shim_handle_msg` (`components/posix_shim/posix_shim.c`).
 */

#include "framework/slot_call.h"

#include "framework/dispatcher.h"
#include "framework/hook.h"
#include "framework/registry.h"
#include "core/sched/task.h"
#include "core/sched/waitq.h"

#include <stddef.h>

/* --- Edge lookup helper --------------------------------------------- *
 *
 * The pause-protocol policy (QUEUE / REJECT / REDIRECT) lives on the
 * `(src, dst)` connection edge.  `framework/ipc.c`'s `find_edge` is
 * static; rather than widen its visibility we walk the slot's outgoing
 * deps inline — the per-task caller_slot's edges were registered by
 * `wire_caller_slot` (slice 8.0a.5) so the search is O(deps), and the
 * cost only matters when the destination slot is paused (rare).
 */

struct edge_search {
    struct nx_slot       *target;
    struct nx_connection *hit;
};

static void edge_match_to(struct nx_connection *c, void *ctx)
{
    struct edge_search *s = ctx;
    if (s->hit) return;
    if (c->to_slot == s->target) s->hit = c;
}

static struct nx_connection *find_outgoing_edge(struct nx_slot *src,
                                                struct nx_slot *dst)
{
    if (!src || !dst) return NULL;
    struct edge_search s = { .target = dst, .hit = NULL };
    nx_slot_foreach_dependency(src, edge_match_to, &s);
    return s.hit;
}

/* --- Body ------------------------------------------------------------ */

int nx_slot_call_blocking(struct nx_slot        *slot,
                          struct nx_ipc_message *msg,
                          void                  *reply_buf,
                          size_t                 reply_buf_len)
{
    if (!slot || !msg) return NX_EINVAL;
    if (msg->dst_slot != slot) return NX_EINVAL;
    if (!msg->src_slot)        return NX_EINVAL;

    /* The caller must be a task with a registered `caller_slot` (slice
     * 8.0a.5).  ISRs / kthreads / boot code don't have caller_slots and
     * must use the existing `nx_ipc_send` async path. */
    struct nx_task *task = nx_task_current();
    if (!task)                              return NX_EINVAL;
    if (!task->caller_slot_active)          return NX_EINVAL;
    if (msg->src_slot != &task->caller_slot) return NX_EINVAL;

    /* v1: single in-flight blocking call per task.  Recursive calls
     * (a syscall handler issuing another blocking call) hit this guard
     * and fail with NX_EINVAL — the recursion depth would blow up the
     * single in_flight_reply_buf slot otherwise. */
    if (task->in_flight_reply_buf != NULL) return NX_EINVAL;

    /* The dispatcher needs to know to post a reply leg back; the wrapper
     * may have set this already, but we set it unconditionally so a
     * misbehaving caller can't silently turn a blocking call into a
     * fire-and-forget that hangs the reply waitq. */
    msg->flags |= NX_MSG_FLAG_REPLY_REQUESTED;

    /* Pause-state policy (slot-side; per SLOT-CALL-API.md §"Pause
     * Protocol Interaction").  Re-check on every QUEUE wake — by the
     * time we run, the slot may have transitioned again. */
    while (1) {
        enum nx_slot_pause_state ps = nx_slot_pause_state(slot);
        if (ps == NX_SLOT_PAUSE_NONE) break;

        struct nx_connection *edge = find_outgoing_edge(msg->src_slot, slot);
        if (!edge) return NX_ENOENT;

        if (edge->policy == NX_PAUSE_REJECT) return NX_EBUSY;
        if (edge->policy == NX_PAUSE_REDIRECT) {
            /* Mirror the existing IPC router contract: redirect chains
             * cap at NX_IPC_REDIRECT_DEPTH_MAX.  Slice 8.0a.6 keeps it
             * simple — fail closed if the slot's fallback isn't wired,
             * since the blocking-call path can't easily recurse without
             * trampling the in_flight reply buf state.  Once a real
             * REDIRECT consumer lands we'll lift the redirect chain
             * walker into a shared helper. */
            return NX_ENOENT;
        }

        /* QUEUE: park on the slot's resume_waitq.  Indefinite — the
         * slot's resume path's `nx_waitq_wake_all` will release us. */
        (void)nx_waitq_wait_with_deadline(&slot->resume_waitq, 0);
        /* loop and re-check pause_state */
    }

    /* Stash the reply target on the task BEFORE enqueuing.  The
     * dispatcher's reply leg (delivered to our caller_slot, handled by
     * posix_shim) will memcpy the reply payload into this buffer. */
    task->in_flight_reply_buf     = reply_buf;
    task->in_flight_reply_buf_len = reply_buf_len;
    task->in_flight_reply_rc      = 0;

    /* IPC_SEND hook.  ABORT here means "this caller never reaches the
     * dispatcher" — no enqueue, no reply needed (we're still on our own
     * stack), so we can return EABORT directly without a synthetic
     * reply leg.  Clear the in-flight buf state on the way out. */
    struct nx_hook_context hctx = {
        .point = NX_HOOK_IPC_SEND,
        .u.ipc = { .src = msg->src_slot, .dst = slot,
                   .msg = msg, .edge = NULL },
    };
    if (nx_hook_dispatch(&hctx) == NX_HOOK_ABORT) {
        task->in_flight_reply_buf     = NULL;
        task->in_flight_reply_buf_len = 0;
        return NX_EABORT;
    }

    /* Cap-scan: every NX_CAP_SLOT_REF in `msg->caps` must correspond to
     * a registered outgoing edge from the sender.  See DESIGN.md R3. */
    int rc = nx_ipc_scan_send_caps(msg->src_slot, msg);
    if (rc != NX_OK) {
        task->in_flight_reply_buf     = NULL;
        task->in_flight_reply_buf_len = 0;
        return rc;
    }

    rc = nx_dispatcher_enqueue(msg);
    if (rc != NX_OK) {
        task->in_flight_reply_buf     = NULL;
        task->in_flight_reply_buf_len = 0;
        return rc;
    }

    /* Block until the reply leg lands.  budget_ns == 0 → indefinite;
     * the reply path's `nx_waitq_wake_one` is the only legitimate
     * wakeup source for the per-task `reply_waitq` in v1. */
    (void)nx_waitq_wait_with_deadline(&task->reply_waitq, 0);

    rc = task->in_flight_reply_rc;
    task->in_flight_reply_buf     = NULL;
    task->in_flight_reply_buf_len = 0;
    task->in_flight_reply_rc      = 0;
    return rc;
}
