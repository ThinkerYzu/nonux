#include "framework/slot_call.h"

/*
 * Slice 8.0a.3 — skeleton.
 *
 * Lands the call surface so dependent slices can compile with
 * `extern` references:
 *   - 8.0a.4 (kernel `posix_shim` component) issues blocking calls
 *     into vfs / scheduler / mm / char_device.serial via this entry.
 *   - 8.0a.5 (per-task `caller_slot`) wires `msg->src_slot` to the
 *     task's embedded slot.
 *
 * The full body — pause-state handling, hook-chain firing, cap-scan,
 * dispatcher enqueue, reply-waitq block, ABORT-path reply synthesis —
 * lands in slice 8.0a.6 alongside the per-CPU reply pool and
 * dispatcher integration (see [SLOT-CALL-API.md] §"Body sequence").
 *
 * Until then this stub returns `NX_ENOSYS` so any accidental caller
 * (test fixture, partial wiring) fails safely rather than crashing.
 */
int nx_slot_call_blocking(struct nx_slot       *slot,
                          struct nx_ipc_message *msg,
                          void                  *reply_buf,
                          size_t                 reply_buf_len)
{
    if (!slot || !msg) return NX_EINVAL;
    (void)reply_buf;
    (void)reply_buf_len;
    return NX_ENOSYS;
}
