/*
 * Slice 8.0d — FS sync-dispatcher wrappers.
 *
 * Caller-side wrappers for the `fs` interface.  These are used by
 * vfs_simple to forward operations to ramfs/procfs (component-to-
 * component, dispatcher-to-dispatcher).
 *
 * Host build: direct ops call through slot->active->descriptor->
 * iface_ops (fast path — no scheduler required for host unit tests).
 *
 * Kernel build: vfs_simple's handle_msg runs on the dispatcher
 * thread, where nx_slot_call_blocking cannot be used (requires a
 * per-task caller_slot).  Instead, each wrapper builds a synthetic
 * nx_ipc_message on the kstack, resolves handle_msg from
 * slot->active->descriptor->ops, and calls it directly — a
 * synchronous dispatcher-to-dispatcher call.  Reading slot->active
 * here is legal: DESIGN §Slot-Resolve Locality (R8) permits it on
 * dispatcher threads.  iface_ops is NOT accessed in the kernel path,
 * satisfying the forthcoming 8.0e iface_ops rule.
 *
 * nx_fs_dispatch (framework/fs_dispatch.h) writes the reply struct
 * in-place at msg->payload, overwriting the request.  Every
 * nx_fs_msg_* struct is sized >= its corresponding nx_fs_reply_*
 * struct, so the in-place write always fits.
 */

#include "framework/fs_call.h"
#include "framework/component.h"
#include "framework/ipc.h"
#include "interfaces/fs_msg.h"

#if !__STDC_HOSTED__
#include "core/lib/lib.h"   /* memset */
#else
#include <string.h>
#endif

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ *
 * Host-build helpers                                                   *
 * ------------------------------------------------------------------ */

#if __STDC_HOSTED__

#define HOST_RESOLVE_FS(slot, ops_var, err_val)                        \
    if (!(slot) || !(slot)->active || !(slot)->active->descriptor ||   \
        !(slot)->active->descriptor->iface_ops)                        \
        return (err_val);                                              \
    const struct nx_fs_ops *(ops_var) =                                \
        (const struct nx_fs_ops *)(slot)->active->descriptor->iface_ops

#define HOST_RESOLVE_FS_VOID(slot, ops_var)                            \
    if (!(slot) || !(slot)->active || !(slot)->active->descriptor ||   \
        !(slot)->active->descriptor->iface_ops)                        \
        return;                                                        \
    const struct nx_fs_ops *(ops_var) =                                \
        (const struct nx_fs_ops *)(slot)->active->descriptor->iface_ops

#endif /* __STDC_HOSTED__ */

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

/*
 * Resolve the target slot to (handle_msg, impl) for a sync
 * dispatcher-to-dispatcher call.  Returns NX_ENOENT if the slot is
 * absent, inactive, or has no handle_msg.
 *
 * Reads slot->active: legal because fs_call.c is only called from
 * vfs_simple's handle_msg, which runs on the dispatcher thread (R8).
 */
static int disp_resolve(struct nx_slot *slot,
                        int (**out_hmsg)(void *, struct nx_ipc_message *),
                        void **out_impl)
{
    if (!slot || !slot->active || !slot->active->descriptor)
        return NX_ENOENT;
    const struct nx_component_ops *ops = slot->active->descriptor->ops;
    if (!ops || !ops->handle_msg)
        return NX_ENOENT;
    *out_hmsg = ops->handle_msg;
    *out_impl = slot->active->impl;
    return NX_OK;
}

/*
 * Initialise a kstack nx_ipc_message pointing at req_var as payload.
 * nx_fs_dispatch writes the reply in-place at msg->payload, so
 * req_var doubles as the reply buffer after the call.
 *
 * Every caller that reads back from msg.payload after hmsg() MUST
 * follow the call with `__asm__ volatile("" ::: "memory")` before
 * casting msg.payload to the reply type.  The in-place write goes
 * through a type-punned pointer (different struct type); the barrier
 * prevents GCC from caching req fields across the call under its
 * strict-aliasing rules.
 */
#define DISP_INIT_MSG(msg_var, slot_var, op_id, req_var)               \
    struct nx_ipc_message msg_var;                                     \
    memset(&(msg_var), 0, sizeof(msg_var));                            \
    (msg_var).dst_slot    = (slot_var);                                \
    (msg_var).msg_type    = (op_id);                                   \
    (msg_var).payload     = &(req_var);                                \
    (msg_var).payload_len = (uint32_t)sizeof(req_var)

#endif /* !__STDC_HOSTED__ */

/* ================================================================== *
 * Wrappers                                                             *
 * ================================================================== */

/* --- nx_fs_open ------------------------------------------------------- */

int nx_fs_open(struct nx_slot *slot, const char *path, uint32_t flags,
               void **out_file)
{
#if __STDC_HOSTED__
    HOST_RESOLVE_FS(slot, ops, NX_ENOENT);
    if (!ops->open) return NX_ENOENT;
    return ops->open(slot->active->impl, path, flags, out_file);
#else
    int (*hmsg)(void *, struct nx_ipc_message *);
    void *impl;
    if (disp_resolve(slot, &hmsg, &impl) != NX_OK) return NX_ENOENT;

    struct nx_fs_msg_open req;
    memset(&req, 0, sizeof req);
    copy_path(req.path, sizeof req.path, path);
    req.flags = flags;

    DISP_INIT_MSG(msg, slot, NX_FS_OP_OPEN, req);
    (void)hmsg(impl, &msg);
    __asm__ volatile("" ::: "memory");
    const struct nx_fs_reply_open *rep =
        (const struct nx_fs_reply_open *)msg.payload;
    if (out_file) *out_file = (void *)(uintptr_t)rep->out_file;
    return rep->rc;
#endif
}

/* --- nx_fs_close ------------------------------------------------------ */

void nx_fs_close(struct nx_slot *slot, void *file)
{
#if __STDC_HOSTED__
    HOST_RESOLVE_FS_VOID(slot, ops);
    if (!ops->close) return;
    ops->close(slot->active->impl, file);
#else
    int (*hmsg)(void *, struct nx_ipc_message *);
    void *impl;
    if (disp_resolve(slot, &hmsg, &impl) != NX_OK) return;

    struct nx_fs_msg_close req;
    memset(&req, 0, sizeof req);
    req.file = (uint64_t)(uintptr_t)file;

    DISP_INIT_MSG(msg, slot, NX_FS_OP_CLOSE, req);
    (void)hmsg(impl, &msg);
#endif
}

/* --- nx_fs_retain ----------------------------------------------------- */

void nx_fs_retain(struct nx_slot *slot, void *file)
{
#if __STDC_HOSTED__
    HOST_RESOLVE_FS_VOID(slot, ops);
    if (!ops->retain) return;
    ops->retain(slot->active->impl, file);
#else
    int (*hmsg)(void *, struct nx_ipc_message *);
    void *impl;
    if (disp_resolve(slot, &hmsg, &impl) != NX_OK) return;

    struct nx_fs_msg_retain req;
    memset(&req, 0, sizeof req);
    req.file = (uint64_t)(uintptr_t)file;

    DISP_INIT_MSG(msg, slot, NX_FS_OP_RETAIN, req);
    (void)hmsg(impl, &msg);
#endif
}

/* --- nx_fs_read ------------------------------------------------------- */

int64_t nx_fs_read(struct nx_slot *slot, void *file, void *buf, size_t cap)
{
#if __STDC_HOSTED__
    HOST_RESOLVE_FS(slot, ops, (int64_t)NX_ENOENT);
    if (!ops->read) return (int64_t)NX_ENOENT;
    return ops->read(slot->active->impl, file, buf, cap);
#else
    int (*hmsg)(void *, struct nx_ipc_message *);
    void *impl;
    if (disp_resolve(slot, &hmsg, &impl) != NX_OK) return (int64_t)NX_EINVAL;

    struct nx_fs_msg_read req;
    memset(&req, 0, sizeof req);
    req.file = (uint64_t)(uintptr_t)file;
    req.buf  = (uint64_t)(uintptr_t)buf;
    req.cap  = cap;

    DISP_INIT_MSG(msg, slot, NX_FS_OP_READ, req);
    (void)hmsg(impl, &msg);
    __asm__ volatile("" ::: "memory");
    const struct nx_fs_reply_read *rep =
        (const struct nx_fs_reply_read *)msg.payload;
    return rep->rc;
#endif
}

/* --- nx_fs_write ------------------------------------------------------ */

int64_t nx_fs_write(struct nx_slot *slot, void *file, const void *buf,
                    size_t len)
{
#if __STDC_HOSTED__
    HOST_RESOLVE_FS(slot, ops, (int64_t)NX_ENOENT);
    if (!ops->write) return (int64_t)NX_ENOENT;
    return ops->write(slot->active->impl, file, buf, len);
#else
    int (*hmsg)(void *, struct nx_ipc_message *);
    void *impl;
    if (disp_resolve(slot, &hmsg, &impl) != NX_OK) return (int64_t)NX_EINVAL;

    struct nx_fs_msg_write req;
    memset(&req, 0, sizeof req);
    req.file = (uint64_t)(uintptr_t)file;
    req.buf  = (uint64_t)(uintptr_t)buf;
    req.len  = len;

    DISP_INIT_MSG(msg, slot, NX_FS_OP_WRITE, req);
    (void)hmsg(impl, &msg);
    __asm__ volatile("" ::: "memory");
    const struct nx_fs_reply_write *rep =
        (const struct nx_fs_reply_write *)msg.payload;
    return rep->rc;
#endif
}

/* --- nx_fs_seek ------------------------------------------------------- */

int64_t nx_fs_seek(struct nx_slot *slot, void *file, int64_t offset,
                   int whence)
{
#if __STDC_HOSTED__
    HOST_RESOLVE_FS(slot, ops, (int64_t)NX_ENOENT);
    if (!ops->seek) return (int64_t)NX_ENOENT;
    return ops->seek(slot->active->impl, file, offset, whence);
#else
    int (*hmsg)(void *, struct nx_ipc_message *);
    void *impl;
    if (disp_resolve(slot, &hmsg, &impl) != NX_OK) return (int64_t)NX_EINVAL;

    struct nx_fs_msg_seek req;
    memset(&req, 0, sizeof req);
    req.file   = (uint64_t)(uintptr_t)file;
    req.offset = offset;
    req.whence = whence;

    DISP_INIT_MSG(msg, slot, NX_FS_OP_SEEK, req);
    (void)hmsg(impl, &msg);
    __asm__ volatile("" ::: "memory");
    const struct nx_fs_reply_seek *rep =
        (const struct nx_fs_reply_seek *)msg.payload;
    return rep->rc;
#endif
}

/* --- nx_fs_readdir ---------------------------------------------------- */

int nx_fs_readdir(struct nx_slot *slot, const char *dir_path,
                  uint32_t *cookie, struct nx_fs_dirent *out)
{
#if __STDC_HOSTED__
    HOST_RESOLVE_FS(slot, ops, NX_ENOENT);
    if (!ops->readdir) return NX_ENOENT;
    return ops->readdir(slot->active->impl, dir_path, cookie, out);
#else
    int (*hmsg)(void *, struct nx_ipc_message *);
    void *impl;
    if (disp_resolve(slot, &hmsg, &impl) != NX_OK) return NX_EINVAL;

    struct nx_fs_msg_readdir req;
    memset(&req, 0, sizeof req);
    copy_path(req.dir_path, sizeof req.dir_path, dir_path);
    req.cookie = cookie ? *cookie : 0;

    DISP_INIT_MSG(msg, slot, NX_FS_OP_READDIR, req);
    (void)hmsg(impl, &msg);
    /* Compiler barrier: hmsg writes the reply in-place at msg.payload (= &req)
     * through a type-punned pointer; without this barrier GCC may cache
     * pre-call values of req under strict-aliasing assumptions. */
    __asm__ volatile("" ::: "memory");
    const struct nx_fs_reply_readdir *rep =
        (const struct nx_fs_reply_readdir *)msg.payload;
    if (cookie) *cookie = rep->cookie;
    if (out)    *out    = rep->out;
    return rep->rc;
#endif
}

/* --- nx_fs_mkdir ------------------------------------------------------ */

int nx_fs_mkdir(struct nx_slot *slot, const char *path)
{
#if __STDC_HOSTED__
    HOST_RESOLVE_FS(slot, ops, NX_ENOENT);
    if (!ops->mkdir) return NX_ENOENT;
    return ops->mkdir(slot->active->impl, path);
#else
    int (*hmsg)(void *, struct nx_ipc_message *);
    void *impl;
    if (disp_resolve(slot, &hmsg, &impl) != NX_OK) return NX_EINVAL;

    struct nx_fs_msg_mkdir req;
    memset(&req, 0, sizeof req);
    copy_path(req.path, sizeof req.path, path);

    DISP_INIT_MSG(msg, slot, NX_FS_OP_MKDIR, req);
    (void)hmsg(impl, &msg);
    __asm__ volatile("" ::: "memory");
    const struct nx_fs_reply_mkdir *rep =
        (const struct nx_fs_reply_mkdir *)msg.payload;
    return rep->rc;
#endif
}

/* --- nx_fs_stat ------------------------------------------------------- */

int nx_fs_stat(struct nx_slot *slot, const char *path, struct nx_fs_stat *out)
{
#if __STDC_HOSTED__
    HOST_RESOLVE_FS(slot, ops, NX_ENOENT);
    if (!ops->stat) return NX_ENOENT;
    return ops->stat(slot->active->impl, path, out);
#else
    int (*hmsg)(void *, struct nx_ipc_message *);
    void *impl;
    if (disp_resolve(slot, &hmsg, &impl) != NX_OK) return NX_EINVAL;

    struct nx_fs_msg_stat req;
    memset(&req, 0, sizeof req);
    copy_path(req.path, sizeof req.path, path);

    DISP_INIT_MSG(msg, slot, NX_FS_OP_STAT, req);
    (void)hmsg(impl, &msg);
    __asm__ volatile("" ::: "memory");
    const struct nx_fs_reply_stat *rep =
        (const struct nx_fs_reply_stat *)msg.payload;
    if (out) *out = rep->out;
    return rep->rc;
#endif
}
