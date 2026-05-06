/*
 * Slice 9b.1 — VFS blocking-call wrappers (ID-based API).
 *
 * Each wrapper packages its inputs into a kstack request struct,
 * issues `nx_slot_call_blocking` (kernel build), and unpacks the
 * reply.  Host-build fast path calls the slot's bound ops directly,
 * bypassing the IPC round-trip so host unit tests keep working
 * without a real scheduler.
 *
 * Slice 9b.1 changes: `open` returns uint32_t id (0 = failure);
 * close/retain/read/write/seek take `uint32_t id` instead of `void *file`.
 *
 * Generated wrapper declarations live in `framework/vfs_call.h`;
 * message / reply structs in `interfaces/vfs_msg.h`;
 * receiver dispatch shim in `framework/vfs_dispatch.h`.
 */

#include "framework/vfs_call.h"
#include "framework/slot_call.h"
#include "framework/component.h"
#include "framework/ipc.h"
#include "interfaces/vfs_msg.h"

#if !__STDC_HOSTED__
#include "core/sched/task.h"
#include "core/lib/lib.h"
#else
#include <string.h>
#endif

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ *
 * Host-build helpers                                                   *
 * ------------------------------------------------------------------ */

#if __STDC_HOSTED__

#define HOST_RESOLVE(slot, ops_var, err_val)                           \
    if (!(slot) || !(slot)->active || !(slot)->active->descriptor ||   \
        !(slot)->active->descriptor->iface_ops)                        \
        return (err_val);                                              \
    const struct nx_vfs_ops *(ops_var) =                               \
        (const struct nx_vfs_ops *)(slot)->active->descriptor->iface_ops

#define HOST_RESOLVE_VOID(slot, ops_var)                               \
    if (!(slot) || !(slot)->active || !(slot)->active->descriptor ||   \
        !(slot)->active->descriptor->iface_ops)                        \
        return;                                                        \
    const struct nx_vfs_ops *(ops_var) =                               \
        (const struct nx_vfs_ops *)(slot)->active->descriptor->iface_ops

#endif  /* __STDC_HOSTED__ */

/* ------------------------------------------------------------------ *
 * Kernel-path helpers                                                  *
 * ------------------------------------------------------------------ */

#if !__STDC_HOSTED__

static void copy_path(char *dst, size_t dst_cap, const char *src)
{
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    while (i + 1 < dst_cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

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
 * Wrappers                                                             *
 * ================================================================== */

/* --- nx_vfs_open ------------------------------------------------------ */

uint32_t nx_vfs_open(struct nx_slot *slot, const char *path, uint32_t flags)
{
#if __STDC_HOSTED__
    HOST_RESOLVE(slot, ops, 0);
    if (!ops->open) return 0;
    return ops->open(slot->active->impl, path, flags);
#else
    struct nx_task *task = nx_task_current();
    if (!task) return 0;

    struct nx_vfs_msg_open req;
    memset(&req, 0, sizeof req);
    copy_path(req.path, sizeof req.path, path);
    req.flags = flags;

    struct nx_vfs_reply_open reply;
    memset(&reply, 0, sizeof reply);

    KERNEL_INIT_MSG(msg, task, slot, NX_VFS_OP_OPEN, req);
    int rc = nx_slot_call_blocking(slot, &msg, &reply, sizeof reply);
    /* nx_slot_call_blocking returns in_flight_reply_rc = (int32_t)vfs_id.
     * A positive vfs_id indicates success; only negative rc means a
     * framework-level error.  Do NOT treat non-zero as failure here. */
    if (rc < 0) return 0;
    return reply.rc;
#endif
}

/* --- nx_vfs_close ----------------------------------------------------- */

void nx_vfs_close(struct nx_slot *slot, uint32_t id)
{
#if __STDC_HOSTED__
    HOST_RESOLVE_VOID(slot, ops);
    if (!ops->close) return;
    ops->close(slot->active->impl, id);
#else
    struct nx_task *task = nx_task_current();
    if (!task) return;

    struct nx_vfs_msg_close req;
    memset(&req, 0, sizeof req);
    req.id = id;

    struct nx_vfs_reply_close reply;
    memset(&reply, 0, sizeof reply);

    KERNEL_INIT_MSG(msg, task, slot, NX_VFS_OP_CLOSE, req);
    (void)nx_slot_call_blocking(slot, &msg, &reply, sizeof reply);
#endif
}

/* --- nx_vfs_retain ---------------------------------------------------- */

void nx_vfs_retain(struct nx_slot *slot, uint32_t id)
{
#if __STDC_HOSTED__
    HOST_RESOLVE_VOID(slot, ops);
    if (!ops->retain) return;
    ops->retain(slot->active->impl, id);
#else
    struct nx_task *task = nx_task_current();
    if (!task) return;

    struct nx_vfs_msg_retain req;
    memset(&req, 0, sizeof req);
    req.id = id;

    struct nx_vfs_reply_retain reply;
    memset(&reply, 0, sizeof reply);

    KERNEL_INIT_MSG(msg, task, slot, NX_VFS_OP_RETAIN, req);
    (void)nx_slot_call_blocking(slot, &msg, &reply, sizeof reply);
#endif
}

/* --- nx_vfs_read ------------------------------------------------------ */

int64_t nx_vfs_read(struct nx_slot *slot, uint32_t id, void *buf, size_t cap)
{
#if __STDC_HOSTED__
    HOST_RESOLVE(slot, ops, (int64_t)NX_ENOENT);
    if (!ops->read) return NX_ENOENT;
    return ops->read(slot->active->impl, id, buf, cap);
#else
    struct nx_task *task = nx_task_current();
    if (!task) return NX_EINVAL;

    struct nx_vfs_msg_read req;
    memset(&req, 0, sizeof req);
    req.id  = id;
    req.buf = (uint64_t)(uintptr_t)buf;
    req.cap = cap;

    struct nx_vfs_reply_read reply;
    memset(&reply, 0, sizeof reply);

    KERNEL_INIT_MSG(msg, task, slot, NX_VFS_OP_READ, req);
    int rc = nx_slot_call_blocking(slot, &msg, &reply, sizeof reply);
    if (rc != NX_OK) return rc;
    return reply.rc;
#endif
}

/* --- nx_vfs_write ----------------------------------------------------- */

int64_t nx_vfs_write(struct nx_slot *slot, uint32_t id, const void *buf,
                     size_t len)
{
#if __STDC_HOSTED__
    HOST_RESOLVE(slot, ops, (int64_t)NX_ENOENT);
    if (!ops->write) return NX_ENOENT;
    return ops->write(slot->active->impl, id, buf, len);
#else
    struct nx_task *task = nx_task_current();
    if (!task) return NX_EINVAL;

    struct nx_vfs_msg_write req;
    memset(&req, 0, sizeof req);
    req.id  = id;
    req.buf = (uint64_t)(uintptr_t)buf;
    req.len = len;

    struct nx_vfs_reply_write reply;
    memset(&reply, 0, sizeof reply);

    KERNEL_INIT_MSG(msg, task, slot, NX_VFS_OP_WRITE, req);
    int rc = nx_slot_call_blocking(slot, &msg, &reply, sizeof reply);
    if (rc != NX_OK) return rc;
    return reply.rc;
#endif
}

/* --- nx_vfs_seek ------------------------------------------------------ */

int64_t nx_vfs_seek(struct nx_slot *slot, uint32_t id, int64_t offset,
                    int whence)
{
#if __STDC_HOSTED__
    HOST_RESOLVE(slot, ops, (int64_t)NX_ENOENT);
    if (!ops->seek) return NX_ENOENT;
    return ops->seek(slot->active->impl, id, offset, whence);
#else
    struct nx_task *task = nx_task_current();
    if (!task) return NX_EINVAL;

    struct nx_vfs_msg_seek req;
    memset(&req, 0, sizeof req);
    req.id     = id;
    req.offset = offset;
    req.whence = whence;

    struct nx_vfs_reply_seek reply;
    memset(&reply, 0, sizeof reply);

    KERNEL_INIT_MSG(msg, task, slot, NX_VFS_OP_SEEK, req);
    int rc = nx_slot_call_blocking(slot, &msg, &reply, sizeof reply);
    if (rc != NX_OK) return rc;
    return reply.rc;
#endif
}

/* --- nx_vfs_readdir --------------------------------------------------- */

int nx_vfs_readdir(struct nx_slot *slot, const char *dir_path,
                   uint32_t *cookie, struct nx_fs_dirent *out)
{
#if __STDC_HOSTED__
    HOST_RESOLVE(slot, ops, NX_ENOENT);
    if (!ops->readdir) return NX_ENOENT;
    return ops->readdir(slot->active->impl, dir_path, cookie, out);
#else
    struct nx_task *task = nx_task_current();
    if (!task) return NX_EINVAL;

    struct nx_vfs_msg_readdir req;
    memset(&req, 0, sizeof req);
    copy_path(req.dir_path, sizeof req.dir_path, dir_path);
    req.cookie = cookie ? *cookie : 0;

    struct nx_vfs_reply_readdir reply;
    memset(&reply, 0, sizeof reply);

    KERNEL_INIT_MSG(msg, task, slot, NX_VFS_OP_READDIR, req);
    int rc = nx_slot_call_blocking(slot, &msg, &reply, sizeof reply);
    if (rc != NX_OK) return rc;
    if (cookie) *cookie = reply.cookie;
    if (out)    *out    = reply.out;
    return reply.rc;
#endif
}

/* --- nx_vfs_mkdir ----------------------------------------------------- */

int nx_vfs_mkdir(struct nx_slot *slot, const char *path)
{
#if __STDC_HOSTED__
    HOST_RESOLVE(slot, ops, NX_ENOENT);
    if (!ops->mkdir) return NX_ENOENT;
    return ops->mkdir(slot->active->impl, path);
#else
    struct nx_task *task = nx_task_current();
    if (!task) return NX_EINVAL;

    struct nx_vfs_msg_mkdir req;
    memset(&req, 0, sizeof req);
    copy_path(req.path, sizeof req.path, path);

    struct nx_vfs_reply_mkdir reply;
    memset(&reply, 0, sizeof reply);

    KERNEL_INIT_MSG(msg, task, slot, NX_VFS_OP_MKDIR, req);
    int rc = nx_slot_call_blocking(slot, &msg, &reply, sizeof reply);
    if (rc != NX_OK) return rc;
    return reply.rc;
#endif
}

/* --- nx_vfs_stat ------------------------------------------------------ */

int nx_vfs_stat(struct nx_slot *slot, const char *path,
                struct nx_fs_stat *out)
{
#if __STDC_HOSTED__
    if (!slot || !slot->active || !slot->active->descriptor ||
        !slot->active->descriptor->iface_ops)
        return NX_ENOENT;
    const struct nx_vfs_ops *ops =
        (const struct nx_vfs_ops *)slot->active->descriptor->iface_ops;
    if (!ops->stat) return NX_ENOENT;
    return ops->stat(slot->active->impl, path, out);
#else
    struct nx_task *task = nx_task_current();
    if (!task) return NX_EINVAL;

    struct nx_vfs_msg_stat req;
    memset(&req, 0, sizeof req);
    copy_path(req.path, sizeof req.path, path);

    struct nx_vfs_reply_stat reply;
    memset(&reply, 0, sizeof reply);

    KERNEL_INIT_MSG(msg, task, slot, NX_VFS_OP_STAT, req);
    int rc = nx_slot_call_blocking(slot, &msg, &reply, sizeof reply);
    if (rc != NX_OK) return rc;
    if (out) *out = reply.out;
    return reply.rc;
#endif
}
