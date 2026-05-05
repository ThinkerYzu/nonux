/*
 * Slice 9b.3 — char_device blocking-call wrappers.
 *
 * Caller-side wrappers for the `char_device` interface, following the
 * same pattern as vfs_call.c.  `write` and `read` go through
 * `nx_slot_call_blocking` (kernel path) so sys_read / sys_write can
 * route RESOURCE handles uniformly via `entry->target` without calling
 * nx_console_read / nx_console_write directly.
 *
 * `rx_byte` is an IRQ-context op; it is a no-op stub until a real IRQ
 * message pool is wired up (the ISR currently calls nx_console_rx_isr
 * directly, not via the component interface).
 *
 * Host build: direct iface_ops calls through slot->active->descriptor->
 * iface_ops (fast path — no scheduler required for host unit tests).
 */

#include "framework/char_device_call.h"
#include "framework/component.h"
#include "framework/ipc.h"
#include "framework/slot_call.h"
#include "interfaces/char_device_msg.h"

#if !__STDC_HOSTED__
#include "core/sched/task.h"
#include "core/lib/lib.h"
#else
#include <string.h>
#endif

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ *
 * Host-build helpers                                                  *
 * ------------------------------------------------------------------ */

#if __STDC_HOSTED__

#define HOST_RESOLVE(slot, ops_var, err_val)                           \
    if (!(slot) || !(slot)->active || !(slot)->active->descriptor ||   \
        !(slot)->active->descriptor->iface_ops)                        \
        return (err_val);                                              \
    const struct nx_char_device_ops *(ops_var) =                       \
        (const struct nx_char_device_ops *)(slot)->active->descriptor->iface_ops

#define HOST_RESOLVE_VOID(slot, ops_var)                               \
    if (!(slot) || !(slot)->active || !(slot)->active->descriptor ||   \
        !(slot)->active->descriptor->iface_ops)                        \
        return;                                                        \
    const struct nx_char_device_ops *(ops_var) =                       \
        (const struct nx_char_device_ops *)(slot)->active->descriptor->iface_ops

#endif  /* __STDC_HOSTED__ */

/* ------------------------------------------------------------------ *
 * Kernel-path helpers                                                 *
 * ------------------------------------------------------------------ */

#if !__STDC_HOSTED__

#define KERNEL_INIT_MSG(msg_var, task_var, slot_var, op_id, req_var)   \
    struct nx_ipc_message msg_var;                                     \
    memset(&(msg_var), 0, sizeof(msg_var));                            \
    (msg_var).src_slot    = &(task_var)->caller_slot;                  \
    (msg_var).dst_slot    = (slot_var);                                \
    (msg_var).msg_type    = (op_id);                                   \
    (msg_var).flags       = NX_MSG_FLAG_REPLY_REQUESTED;               \
    (msg_var).payload     = &(req_var);                                \
    (msg_var).payload_len = (uint32_t)sizeof(req_var)

#endif  /* !__STDC_HOSTED__ */

/* ================================================================== *
 * Wrappers                                                            *
 * ================================================================== */

/* --- nx_char_device_write ----------------------------------------- */

int64_t nx_char_device_write(struct nx_slot *slot, const void *buf, size_t len)
{
#if __STDC_HOSTED__
    HOST_RESOLVE(slot, ops, (int64_t)NX_ENOENT);
    if (!ops->write) return (int64_t)NX_ENOENT;
    return ops->write(slot->active->impl, buf, len);
#else
    struct nx_task *task = nx_task_current();
    if (!task) return NX_EINVAL;

    struct nx_char_device_msg_write req;
    memset(&req, 0, sizeof req);
    req.buf = (uint64_t)(uintptr_t)buf;
    req.len = len;

    struct nx_char_device_reply_write reply;
    memset(&reply, 0, sizeof reply);

    KERNEL_INIT_MSG(msg, task, slot, NX_CHAR_DEVICE_OP_WRITE, req);
    int rc = nx_slot_call_blocking(slot, &msg, &reply, sizeof reply);
    if (rc != NX_OK) return rc;
    return reply.rc;
#endif
}

/* --- nx_char_device_read ------------------------------------------ */

int64_t nx_char_device_read(struct nx_slot *slot, uint32_t id, void *buf,
                             size_t cap)
{
#if __STDC_HOSTED__
    HOST_RESOLVE(slot, ops, (int64_t)NX_ENOENT);
    if (!ops->read) return (int64_t)NX_ENOENT;
    return ops->read(slot->active->impl, id, buf, cap);
#else
    struct nx_task *task = nx_task_current();
    if (!task) return NX_EINVAL;

    struct nx_char_device_msg_read req;
    memset(&req, 0, sizeof req);
    req.id  = id;
    req.buf = (uint64_t)(uintptr_t)buf;
    req.cap = cap;

    struct nx_char_device_reply_read reply;
    memset(&reply, 0, sizeof reply);

    KERNEL_INIT_MSG(msg, task, slot, NX_CHAR_DEVICE_OP_READ, req);
    int rc = nx_slot_call_blocking(slot, &msg, &reply, sizeof reply);
    if (rc != NX_OK) return rc;
    return reply.rc;
#endif
}

/* --- nx_char_device_rx_byte --------------------------------------- */

void nx_char_device_rx_byte(struct nx_slot *slot, uint8_t byte)
{
#if __STDC_HOSTED__
    HOST_RESOLVE_VOID(slot, ops);
    if (!ops->rx_byte) return;
    ops->rx_byte(slot->active->impl, byte);
#else
    /* IRQ-context op — cannot use nx_slot_call_blocking (no task context).
     * v1 stub: no-op until the IRQ message pool is wired; ISR calls
     * nx_console_rx_isr directly rather than through this wrapper. */
    (void)slot;
    (void)byte;
#endif
}
