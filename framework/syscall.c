#include "framework/syscall.h"
#include "framework/channel.h"
#include "framework/handle.h"
#include "framework/process.h"
#include "framework/registry.h"
#include "framework/component.h"
#include "interfaces/fs.h"
#include "interfaces/vfs.h"

#include <stddef.h>
#include <stdint.h>

#if !__STDC_HOSTED__
#include "core/lib/lib.h"              /* uart_putc, memcpy */
#include "core/cpu/exception.h"        /* struct trap_frame */
#include "core/mmu/mmu.h"              /* mmu_user_window_{base,size} */
#else
#include <string.h>                    /* memcpy */
/* Host build: core/cpu/exception.h contains ARM64 inline asm, which
 * won't compile on x86.  Mirror the trap_frame layout locally so host
 * dispatch tests can construct synthetic frames without depending on
 * the CPU header. */
struct trap_frame {
    uint64_t x[31];
    uint64_t sp_el0;
    uint64_t pc;
    uint64_t pstate;
};
#endif

/*
 * Syscall dispatch — slice 5.4.
 *
 * One static dispatch table keyed by syscall number.  Each entry takes
 * the six AArch64-ABI argument registers (x0..x5) as raw uint64_t and
 * returns an nx_status_t; each handler is responsible for type-
 * converting its own args.  A NULL slot is reserved (NX_SYS_RESERVED_0
 * at number 0) so stray `svc #0` with x8 unset gets a crisp NX_EINVAL
 * rather than silently invoking op 0.
 *
 * The table is const, so dispatch is lock-free by construction.  The
 * handlers themselves may touch shared state (the global handle table,
 * the UART) — those touch points manage their own invariants.
 */

typedef nx_status_t (*syscall_fn)(uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t, uint64_t);

/* ---------- Current handle table -------------------------------------- *
 *
 * Slice 7.1 moves from a process-agnostic global to the handle table
 * owned by the current task's process.  The old `g_kernel_handles`
 * role is now played by `g_kernel_process.handles` — the always-
 * present kernel process that covers bootstrap and any kthread not
 * yet reassigned to a user process.  EL0 tests that want an isolated
 * handle namespace call `nx_process_create` + assign `task->process`
 * before issuing syscalls.
 */

/* Slice 5.5: test-observable counter of successful debug_write calls.
 * Bumped inside the syscall body on the happy path; tests read via
 * nx_syscall_debug_write_calls() to confirm EL0 code reached the SVC
 * without relying on UART output capture. */
static _Atomic uint64_t g_debug_write_calls;

struct nx_handle_table *nx_syscall_current_table(void)
{
    struct nx_process *p = nx_process_current();
    return &p->handles;
}

void nx_syscall_reset_for_test(void)
{
    /* Reset the CURRENT process's handle table.  In host tests that's
     * typically the kernel process (no scheduled user tasks); in
     * kernel tests a freshly-spawned kthread inherits the caller's
     * process, so by default this still clears the kernel process's
     * table.  Tests that drive multiple processes can call
     * `nx_process_reset_for_test` to wipe the whole table universe. */
    struct nx_process *p = nx_process_current();
    nx_handle_table_init(&p->handles);
    __atomic_store_n(&g_debug_write_calls, 0, __ATOMIC_RELAXED);
}

uint64_t nx_syscall_debug_write_calls(void)
{
    return __atomic_load_n(&g_debug_write_calls, __ATOMIC_RELAXED);
}

/* ---------- User-pointer copy helpers (slice 5.6) -------------------- *
 *
 * On kernel, bounds-check a candidate user pointer against the MMU's
 * user window — the one EL0-accessible VA range.  If it falls outside,
 * NX_EINVAL; otherwise a plain memcpy.  On host there's no user
 * window, so we trust the pointer and memcpy directly (host tests
 * hand-construct kernel pointers).
 *
 * Proper page-fault-tolerant copy (with an exception-handler fixup)
 * is a later slice; today a bad user pointer that still falls within
 * the window hits memcpy and may scribble on user pages.  That's an
 * acceptable v1 gap: EL0 is trusted research code, and the bounds
 * check catches the "wild kernel-pointer" misuse.
 */
static int copy_from_user(void *kdst, const void *user_src, size_t len)
{
    if (!kdst || !user_src) return NX_EINVAL;
    if (len == 0) return NX_OK;
#if !__STDC_HOSTED__
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t p    = (uint64_t)(uintptr_t)user_src;
    if (p < base || p > base + size)       return NX_EINVAL;
    if (p + len < p || p + len > base + size) return NX_EINVAL;
#endif
    memcpy(kdst, user_src, len);
    return NX_OK;
}

static int copy_to_user(void *user_dst, const void *ksrc, size_t len)
{
    if (!user_dst || !ksrc) return NX_EINVAL;
    if (len == 0) return NX_OK;
#if !__STDC_HOSTED__
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t p    = (uint64_t)(uintptr_t)user_dst;
    if (p < base || p > base + size)       return NX_EINVAL;
    if (p + len < p || p + len > base + size) return NX_EINVAL;
#endif
    memcpy(user_dst, ksrc, len);
    return NX_OK;
}

/* ---------- Syscall bodies ------------------------------------------- */

/*
 * NX_SYS_DEBUG_WRITE — (const char *buf, size_t len).
 *
 * Writes `len` bytes from `buf` to the kernel UART.  Returns the byte
 * count on success (always == len in v1; partial writes arrive when
 * flow control is added).  NX_EINVAL on NULL buf, NX_OK-but-zero on
 * zero len.
 *
 * On host there is no UART — the call fails cleanly with NX_EINVAL so
 * the slice 5.4 plumbing can still link under the host test harness
 * (a host test could mock it if needed; kernel tests exercise the real
 * path).
 */
static nx_status_t sys_debug_write(uint64_t a0, uint64_t a1,
                                   uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    const char *buf = (const char *)(uintptr_t)a0;
    size_t      len = (size_t)a1;
    if (!buf && len > 0) return NX_EINVAL;

#if !__STDC_HOSTED__
    for (size_t i = 0; i < len; i++)
        uart_putc(buf[i]);
#endif
    /* Slice 5.5: bump the test-observable counter on the happy path
     * only.  Atomic because EL0 and kernel-test tasks can both call
     * debug_write under preemption; no correctness rides on this
     * beyond "counter monotonically increases once per successful
     * call", which __ATOMIC_RELAXED delivers. */
    __atomic_fetch_add(&g_debug_write_calls, 1, __ATOMIC_RELAXED);
    return (nx_status_t)len;
}

/* ---------- VFS slot resolution (slice 6.3) -------------------------- *
 *
 * File syscalls look up the `vfs` slot fresh on every call — matches
 * vfs_simple's own internal discipline (DESIGN §Slot-Based Indirection)
 * and keeps future hot-swap transparent to the syscall layer.
 * Returns NX_OK on success (with `*out_ops` / `*out_self` populated),
 * NX_ENOENT if the slot is unregistered or has no active binding,
 * NX_EINVAL if the bound component doesn't export iface_ops.
 */
static int resolve_vfs(const struct nx_vfs_ops **out_ops, void **out_self)
{
    struct nx_slot *slot = nx_slot_lookup("vfs");
    if (!slot || !slot->active) return NX_ENOENT;
    if (!slot->active->descriptor || !slot->active->descriptor->iface_ops)
        return NX_EINVAL;
    *out_ops  = (const struct nx_vfs_ops *)slot->active->descriptor->iface_ops;
    *out_self = slot->active->impl;
    return NX_OK;
}

/* ---------- Path copy (slice 6.3) ------------------------------------ *
 *
 * Copy a NUL-terminated user-space path into a kernel buffer, bounded
 * by `cap`.  Reads a byte at a time through `copy_from_user` so each
 * byte goes through the bounds check independently — that lets a path
 * that genuinely ends before the user-window boundary succeed even if
 * copying `cap` bytes in one go would overflow.
 *
 * Returns:
 *   NX_OK      — `kdst` holds a NUL-terminated string.
 *   NX_EINVAL  — NULL args, zero cap, byte outside the user window,
 *                or no NUL within `cap`.
 */
static int copy_path_from_user(char *kdst, size_t cap, const char *user_src)
{
    if (!kdst || !user_src || cap == 0) return NX_EINVAL;
    for (size_t i = 0; i < cap; i++) {
        char ch = 0;
        int rc = copy_from_user(&ch, user_src + i, 1);
        if (rc != NX_OK) return rc;
        kdst[i] = ch;
        if (ch == '\0') return NX_OK;
    }
    return NX_EINVAL;  /* path didn't fit in cap */
}

/*
 * NX_SYS_HANDLE_CLOSE — (nx_handle_t h).
 *
 * Closes a handle in the caller's table, running the object-side
 * destructor first if the type has one.  Slice 5.6 adds the
 * HANDLE_CHANNEL destructor (`nx_channel_endpoint_close`); slice 6.3
 * adds HANDLE_FILE (dispatch through the `vfs` slot).  Future handle
 * types (VMO, PROCESS, ...) hook in here too.
 */
static nx_status_t sys_handle_close(uint64_t a0, uint64_t a1,
                                    uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    nx_handle_t h = (nx_handle_t)a0;
    struct nx_handle_table *t = nx_syscall_current_table();

    /* Type-aware close: look up first so we can drop the object's
     * own refcount before the slot itself is freed.  If lookup fails
     * (stale handle), pass straight to nx_handle_close which returns
     * the canonical NX_ENOENT. */
    enum nx_handle_type type;
    void               *object = 0;
    int rc = nx_handle_lookup(t, h, &type, 0, &object);
    if (rc == NX_OK && object) {
        if (type == NX_HANDLE_CHANNEL) {
            nx_channel_endpoint_close(object);
        } else if (type == NX_HANDLE_FILE) {
            /* Dispatch through the vfs slot.  If the slot has been
             * unmounted mid-flight the object leaks — unavoidable, but
             * the handle slot still gets freed.  Not a normal path. */
            const struct nx_vfs_ops *vops; void *vself;
            if (resolve_vfs(&vops, &vself) == NX_OK)
                vops->close(vself, object);
        }
    }
    return nx_handle_close(t, h);
}

/* ---------- Channel syscalls (slice 5.6) ----------------------------- *
 *
 * All three look identifiers up in the caller's table and dispatch
 * through `framework/channel.c`.  The handles carry the full
 * READ|WRITE|TRANSFER rights at create time — rights attenuation via
 * `nx_handle_duplicate` is how a caller gives out a narrowed handle
 * (e.g. read-only side of a channel).
 */

static nx_status_t sys_channel_create(uint64_t a0, uint64_t a1,
                                      uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    nx_handle_t *user_end0 = (nx_handle_t *)(uintptr_t)a0;
    nx_handle_t *user_end1 = (nx_handle_t *)(uintptr_t)a1;
    if (!user_end0 || !user_end1) return NX_EINVAL;

    struct nx_channel_endpoint *e0 = 0, *e1 = 0;
    int rc = nx_channel_create(&e0, &e1);
    if (rc != NX_OK) return rc;

    struct nx_handle_table *t = nx_syscall_current_table();
    nx_handle_t h0 = NX_HANDLE_INVALID, h1 = NX_HANDLE_INVALID;

    uint32_t rights = NX_RIGHT_READ | NX_RIGHT_WRITE | NX_RIGHT_TRANSFER;
    rc = nx_handle_alloc(t, NX_HANDLE_CHANNEL, rights, e0, &h0);
    if (rc != NX_OK) { nx_channel_endpoint_close(e0);
                        nx_channel_endpoint_close(e1); return rc; }
    rc = nx_handle_alloc(t, NX_HANDLE_CHANNEL, rights, e1, &h1);
    if (rc != NX_OK) {
        /* First alloc succeeded — unwind. */
        nx_handle_close(t, h0);           /* drops the handle slot */
        nx_channel_endpoint_close(e0);
        nx_channel_endpoint_close(e1);
        return rc;
    }

    rc = copy_to_user(user_end0, &h0, sizeof h0);
    if (rc == NX_OK) rc = copy_to_user(user_end1, &h1, sizeof h1);
    if (rc != NX_OK) {
        /* Bad user pointer — close the fresh handles (which also
         * closes the endpoints via sys_handle_close semantics).  We
         * use nx_channel_endpoint_close directly rather than going
         * through the syscall so this is pure framework code. */
        nx_handle_close(t, h0);
        nx_handle_close(t, h1);
        nx_channel_endpoint_close(e0);
        nx_channel_endpoint_close(e1);
        return rc;
    }
    return NX_OK;
}

static int lookup_channel_endpoint(nx_handle_t h, uint32_t need_rights,
                                   struct nx_channel_endpoint **out)
{
    struct nx_handle_table *t = nx_syscall_current_table();
    enum nx_handle_type type;
    uint32_t             rights;
    void                *obj;
    int rc = nx_handle_lookup(t, h, &type, &rights, &obj);
    if (rc != NX_OK) return rc;
    if (type != NX_HANDLE_CHANNEL) return NX_EINVAL;
    if ((rights & need_rights) != need_rights) return NX_EPERM;
    *out = obj;
    return NX_OK;
}

static nx_status_t sys_channel_send(uint64_t a0, uint64_t a1, uint64_t a2,
                                    uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    nx_handle_t  h   = (nx_handle_t)a0;
    const void  *buf = (const void *)(uintptr_t)a1;
    size_t       len = (size_t)a2;
    if (len > NX_CHANNEL_MSG_MAX) return NX_EINVAL;

    struct nx_channel_endpoint *e = 0;
    int rc = lookup_channel_endpoint(h, NX_RIGHT_WRITE, &e);
    if (rc != NX_OK) return rc;

    /* Copy the user payload into a kernel staging buffer before
     * handing it to the channel — prevents a racing user from
     * mutating the bytes after we've checked but before we've
     * enqueued, and decouples the channel layer from user pointers. */
    uint8_t staging[NX_CHANNEL_MSG_MAX];
    rc = copy_from_user(staging, buf, len);
    if (rc != NX_OK) return rc;

    return (nx_status_t)nx_channel_send(e, staging, len);
}

static nx_status_t sys_channel_recv(uint64_t a0, uint64_t a1, uint64_t a2,
                                    uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    nx_handle_t  h   = (nx_handle_t)a0;
    void        *buf = (void *)(uintptr_t)a1;
    size_t       cap = (size_t)a2;
    if (cap == 0) return NX_EINVAL;
    if (cap > NX_CHANNEL_MSG_MAX) cap = NX_CHANNEL_MSG_MAX;

    struct nx_channel_endpoint *e = 0;
    int rc = lookup_channel_endpoint(h, NX_RIGHT_READ, &e);
    if (rc != NX_OK) return rc;

    uint8_t staging[NX_CHANNEL_MSG_MAX];
    int got = nx_channel_recv(e, staging, cap);
    if (got < 0) return got;

    rc = copy_to_user(buf, staging, (size_t)got);
    if (rc != NX_OK) return rc;
    return (nx_status_t)got;
}

/* ---------- File syscalls (slice 6.3) -------------------------------- *
 *
 * Dispatch through the `vfs` slot's iface_ops (vfs_simple in the live
 * composition).  The syscall layer owns path copy-in, rights assignment,
 * and handle-table bookkeeping; vfs_simple + the mounted driver own
 * path resolution and byte movement.
 *
 * Handle rights:
 *   `NX_FS_OPEN_READ`   → `NX_RIGHT_READ`
 *   `NX_FS_OPEN_WRITE`  → `NX_RIGHT_WRITE`
 *   `NX_FS_OPEN_CREATE` is an open-time flag, not a handle right; it's
 *                         forwarded to the driver and dropped.
 * Slice 6.4 adds `NX_RIGHT_SEEK` alongside the seek op.
 */

static nx_status_t sys_open(uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    const char *user_path = (const char *)(uintptr_t)a0;
    uint32_t    flags     = (uint32_t)a1;

    char kpath[NX_PATH_MAX];
    int rc = copy_path_from_user(kpath, NX_PATH_MAX, user_path);
    if (rc != NX_OK) return rc;

    const struct nx_vfs_ops *vops; void *vself;
    rc = resolve_vfs(&vops, &vself);
    if (rc != NX_OK) return rc;

    void *file = 0;
    rc = vops->open(vself, kpath, flags, &file);
    if (rc != NX_OK) return rc;

    uint32_t rights = 0;
    if (flags & NX_VFS_OPEN_READ)  rights |= NX_RIGHT_READ;
    if (flags & NX_VFS_OPEN_WRITE) rights |= NX_RIGHT_WRITE;
    /* SEEK is implicitly granted on any file open — seek is meaningful
     * on every backing store that supports reads or writes.  If a
     * future driver exposes a non-seekable stream type, the right can
     * be dropped or attenuated via `nx_handle_duplicate`. */
    if (rights) rights |= NX_RIGHT_SEEK;

    struct nx_handle_table *t = nx_syscall_current_table();
    nx_handle_t h = NX_HANDLE_INVALID;
    rc = nx_handle_alloc(t, NX_HANDLE_FILE, rights, file, &h);
    if (rc != NX_OK) {
        /* Handle-table full or other alloc failure — roll back the
         * driver-side open so the per-open slot isn't leaked. */
        vops->close(vself, file);
        return rc;
    }
    return (nx_status_t)h;
}

/*
 * Validate a handle as a HANDLE_FILE with the requested rights, return
 * the backing per-open object pointer.  Matches the channel syscalls'
 * `lookup_channel_endpoint` helper — shared shape means future handle
 * types that need the same guard can lift this pattern.
 */
static int lookup_file_object(nx_handle_t h, uint32_t need_rights,
                              void **out)
{
    struct nx_handle_table *t = nx_syscall_current_table();
    enum nx_handle_type type;
    uint32_t             rights;
    void                *obj;
    int rc = nx_handle_lookup(t, h, &type, &rights, &obj);
    if (rc != NX_OK) return rc;
    if (type != NX_HANDLE_FILE) return NX_EINVAL;
    if ((rights & need_rights) != need_rights) return NX_EPERM;
    *out = obj;
    return NX_OK;
}

static nx_status_t sys_read(uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    nx_handle_t  h   = (nx_handle_t)a0;
    void        *buf = (void *)(uintptr_t)a1;
    size_t       cap = (size_t)a2;
    if (cap == 0) return 0;  /* no-op read is explicitly OK */
    if (cap > NX_FILE_IO_MAX) cap = NX_FILE_IO_MAX;

    void *object = 0;
    int rc = lookup_file_object(h, NX_RIGHT_READ, &object);
    if (rc != NX_OK) return rc;

    const struct nx_vfs_ops *vops; void *vself;
    rc = resolve_vfs(&vops, &vself);
    if (rc != NX_OK) return rc;

    uint8_t staging[NX_FILE_IO_MAX];
    int64_t got = vops->read(vself, object, staging, cap);
    if (got < 0) return (nx_status_t)got;

    rc = copy_to_user(buf, staging, (size_t)got);
    if (rc != NX_OK) return rc;
    return (nx_status_t)got;
}

static nx_status_t sys_write(uint64_t a0, uint64_t a1, uint64_t a2,
                             uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    nx_handle_t  h   = (nx_handle_t)a0;
    const void  *buf = (const void *)(uintptr_t)a1;
    size_t       len = (size_t)a2;
    if (len == 0) return 0;
    if (len > NX_FILE_IO_MAX) len = NX_FILE_IO_MAX;

    void *object = 0;
    int rc = lookup_file_object(h, NX_RIGHT_WRITE, &object);
    if (rc != NX_OK) return rc;

    uint8_t staging[NX_FILE_IO_MAX];
    rc = copy_from_user(staging, buf, len);
    if (rc != NX_OK) return rc;

    const struct nx_vfs_ops *vops; void *vself;
    rc = resolve_vfs(&vops, &vself);
    if (rc != NX_OK) return rc;

    return (nx_status_t)vops->write(vself, object, staging, len);
}

/*
 * NX_SYS_SEEK — (nx_handle_t h, int64_t offset, int whence).
 *
 * Same look-up-and-dispatch shape as read/write.  Handle must be
 * HANDLE_FILE with RIGHT_SEEK; whence values are `NX_VFS_SEEK_{SET,
 * CUR,END}`.  Returns the new absolute position (≥ 0) or negative
 * `NX_E*` (e.g. NX_EINVAL for out-of-range result, unknown whence).
 */
static nx_status_t sys_seek(uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    nx_handle_t  h      = (nx_handle_t)a0;
    int64_t      offset = (int64_t)a1;
    int          whence = (int)a2;

    void *object = 0;
    int rc = lookup_file_object(h, NX_RIGHT_SEEK, &object);
    if (rc != NX_OK) return rc;

    const struct nx_vfs_ops *vops; void *vself;
    rc = resolve_vfs(&vops, &vself);
    if (rc != NX_OK) return rc;

    return (nx_status_t)vops->seek(vself, object, offset, whence);
}

/*
 * NX_SYS_READDIR — (uint32_t *user_cookie, struct nx_fs_dirent *user_out).
 *
 * Filesystem-level enumeration in v1 (no dir handles — see
 * interfaces/fs.h readdir docs).  Caller owns the cookie: copy it
 * into a kernel-side local through `copy_from_user`, pass to the
 * driver, copy the updated cookie + populated dirent back via
 * `copy_to_user`.
 *
 * Returns NX_OK on success (out populated, cookie advanced),
 * NX_ENOENT when iteration is complete, NX_EINVAL on NULL args or
 * a bad user pointer.
 */
static nx_status_t sys_readdir(uint64_t a0, uint64_t a1, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    uint32_t           *user_cookie = (uint32_t *)(uintptr_t)a0;
    struct nx_fs_dirent *user_out   = (struct nx_fs_dirent *)(uintptr_t)a1;
    if (!user_cookie || !user_out) return NX_EINVAL;

    uint32_t kcookie = 0;
    int rc = copy_from_user(&kcookie, user_cookie, sizeof kcookie);
    if (rc != NX_OK) return rc;

    const struct nx_vfs_ops *vops; void *vself;
    rc = resolve_vfs(&vops, &vself);
    if (rc != NX_OK) return rc;

    struct nx_fs_dirent kent;
    rc = vops->readdir(vself, &kcookie, &kent);
    if (rc != NX_OK) return rc;

    rc = copy_to_user(user_out, &kent, sizeof kent);
    if (rc != NX_OK) return rc;
    rc = copy_to_user(user_cookie, &kcookie, sizeof kcookie);
    if (rc != NX_OK) return rc;
    return NX_OK;
}

/*
 * NX_SYS_EXIT — (int code).
 *
 * Marks the current process EXITED with the supplied code and parks
 * the caller in a `wfe` loop (slice 7.1).  Never returns — the
 * dispatcher's `tf->x[0] = rc` write never happens.  The ktest
 * harness still dequeues the stranded task externally via
 * `ops->dequeue`; slice 7.4 will integrate real process wait +
 * scheduler-driven reclamation.
 */
static nx_status_t sys_exit(uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    int code = (int)(int64_t)a0;
    nx_process_exit(code);
    /* Unreachable — nx_process_exit is noreturn. */
    return NX_OK;
}

/* ---------- Dispatch table ------------------------------------------- */

static const syscall_fn g_syscall_table[NX_SYSCALL_COUNT] = {
    [NX_SYS_RESERVED_0]     = NULL,             /* caught below → NX_EINVAL */
    [NX_SYS_DEBUG_WRITE]    = sys_debug_write,
    [NX_SYS_HANDLE_CLOSE]   = sys_handle_close,
    [NX_SYS_CHANNEL_CREATE] = sys_channel_create,
    [NX_SYS_CHANNEL_SEND]   = sys_channel_send,
    [NX_SYS_CHANNEL_RECV]   = sys_channel_recv,
    [NX_SYS_OPEN]           = sys_open,
    [NX_SYS_READ]           = sys_read,
    [NX_SYS_WRITE]          = sys_write,
    [NX_SYS_SEEK]           = sys_seek,
    [NX_SYS_READDIR]        = sys_readdir,
    [NX_SYS_EXIT]           = sys_exit,
};

/* ---------- Entry point ---------------------------------------------- */

void nx_syscall_dispatch(struct trap_frame *tf)
{
    if (!tf) return;

    uint64_t num = tf->x[8];
    nx_status_t rc;

    if (num >= NX_SYSCALL_COUNT || g_syscall_table[num] == NULL) {
        rc = NX_EINVAL;
    } else {
        rc = g_syscall_table[num](tf->x[0], tf->x[1], tf->x[2],
                                  tf->x[3], tf->x[4], tf->x[5]);
    }
    tf->x[0] = (uint64_t)rc;
}
