#ifndef NX_FRAMEWORK_SLOT_CALL_H
#define NX_FRAMEWORK_SLOT_CALL_H

#include <stddef.h>

#include "framework/ipc.h"

/*
 * Blocking-call infrastructure — slice 8.0a (sub-sliced 8.0a.3 → 8.0a.8).
 *
 * The kernel-side `posix_shim` component (slice 8.0a.4) and every
 * generated per-op wrapper (slice 8.0b activates these) issue cross-
 * component calls through `nx_slot_call_blocking`.  The dispatcher
 * resolves `slot->active`, runs the receiver's `handle_msg`, and posts
 * a reply leg back to the caller's per-task `caller_slot`; the caller
 * blocks on `task->reply_waitq` for the duration.
 *
 * v1 (slice 8.0a) constraints:
 *   - Single in-flight blocking call per task (asserted via
 *     `task->in_flight_reply_buf` being NULL on entry).
 *   - Caller must be a task with a registered `caller_slot` (slice
 *     8.0a.5 lands this).  ISRs and kthreads cannot call this.
 *   - Bounded handlers still apply — a handler that takes 10 ms blocks
 *     the syscall caller for 10 ms.  See DESIGN.md §"Bounded Handlers
 *     and Async Split".
 *
 * Slice 8.0a.3 lands the declaration + a stub body; the dispatcher
 * round-trip body lands in slice 8.0a.6.  See [SLOT-CALL-API.md] for
 * the full protocol and the post-Session-85 reconciliations
 * (`(reply_buf, reply_buf_len)` arg pair, `NX_MSG_FLAG_REPLY_REQUESTED`
 * request flag, `NX_EABORT = -8` reused).
 */

/*
 * Issue a blocking call to a slot.  Caller's task blocks on its own
 * reply waitq until the dispatcher posts a reply.
 *
 * `slot` is the destination slot.  `msg` carries the request:
 *   msg->src_slot   = current task's caller_slot (set by wrapper)
 *   msg->dst_slot   = slot
 *   msg->msg_type   = op_id from generated `enum nx_<iface>_op_id`
 *   msg->flags     |= NX_MSG_FLAG_REPLY_REQUESTED
 *   msg->payload    = pointer to wrapper-built request struct (kstack)
 *   msg->n_caps    + msg->caps  -- optional; per-iface
 *
 * `reply_buf` / `reply_buf_len` describe the wrapper-allocated kstack
 * struct the reply leg's payload is copied into.  Stashed on the
 * caller task before enqueue; cleared after wake.  Threading the
 * pointer through a separate arg (vs. growing `nx_ipc_message`) keeps
 * the IPC carrier generic — the reply-buffer concept is specific to
 * the blocking-call wrapper protocol.  Pass NULL/0 only if the op has
 * no return payload (rare; see the per-op generated wrapper).
 *
 * Returns the handler's int rc (NX_OK or negative NX_E*) on successful
 * round-trip, or one of:
 *   NX_EBUSY    — slot paused with REJECT policy on the edge.
 *   NX_ELOOP    — REDIRECT depth cap exceeded (loop in fallback chain).
 *   NX_ENOENT   — slot has no active impl with handle_msg.
 *   NX_EABORT   — NX_HOOK_IPC_SEND chain returned ABORT.
 *   NX_EINVAL   — NULL/mismatched args.
 *
 * Slice 8.0a.3 stub: returns `NX_ENOSYS` for any well-formed call;
 * the dispatcher round-trip body lands in slice 8.0a.6.
 */
int nx_slot_call_blocking(struct nx_slot       *slot,
                          struct nx_ipc_message *msg,
                          void                  *reply_buf,
                          size_t                 reply_buf_len);

#endif /* NX_FRAMEWORK_SLOT_CALL_H */
