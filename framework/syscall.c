#include "framework/syscall.h"
#include "framework/channel.h"
#include "framework/console.h"
#include "framework/handle.h"
#include "framework/process.h"
#include "framework/registry.h"
#include "framework/component.h"
#include "interfaces/fs.h"
#include "interfaces/vfs.h"

#if !__STDC_HOSTED__
#include "core/lib/kheap.h"
#include "core/mmu/mmu.h"
#include "core/sched/sched.h"
#include "core/sched/task.h"
#include "framework/elf.h"
#include "interfaces/scheduler.h"
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if !__STDC_HOSTED__
#include "core/lib/lib.h"              /* uart_putc, memcpy */
#include "core/cpu/exception.h"        /* struct trap_frame */
#include "core/cpu/monotonic.h"        /* nx_monotonic_raw — AT_RANDOM seed */
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
 * at number 0) so stray `svc #0` with x8 unset gets a crisp NX_ENOSYS
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
    nx_console_reset_for_test();
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

    /* Slice 7.6d.N.6b — POSIX STDIN_FILENO = 0 routes to slot 2 (encoded
     * value 0 has no natural form; see sys_read for the same routing
     * + rationale).  Reconstruct the encoded handle for slot 2 with the
     * slot's current generation so `nx_handle_close` resolves it. */
    if (h == 0) {
        if (!t || t->entries[2].type == NX_HANDLE_INVALID) return NX_ENOENT;
        h = (t->entries[2].generation << 8) | (uint32_t)(2 + 1);
    }

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
        } else if (type == NX_HANDLE_DIR) {
            /* Slice 7.6d.N.5 — free the directory cursor.  No vfs
             * dispatch: the cursor is a kheap-allocated state struct
             * owned by the syscall layer.  Host build has no kheap;
             * the HANDLE_DIR path is unreachable on host because
             * sys_open's `/` branch is gated under !__STDC_HOSTED__. */
#if !__STDC_HOSTED__
            free(object);
#endif
        }
        /* HANDLE_CONSOLE: nothing to free — the underlying object is the
         * `g_nx_console` sentinel, shared across every slot 0/1/2 in
         * every process.  The handle slot itself is freed by the
         * `nx_handle_close` call below. */
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

/*
 * Slice 7.6d.N.5 — directory cursor.  Bare-bones state struct
 * holding just the readdir cookie; allocated when sys_open is
 * called with path == "/" and freed by sys_handle_close on the
 * HANDLE_DIR.  The flat-namespace ramfs has only one possible
 * directory ("/"), so there's no path field — every cursor
 * iterates the global file table.
 */
struct nx_dir_cursor {
    uint32_t cookie;
};

static nx_status_t sys_open(uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    const char *user_path = (const char *)(uintptr_t)a0;
    uint32_t    flags     = (uint32_t)a1;

    char kpath[NX_PATH_MAX];
    int rc = copy_path_from_user(kpath, NX_PATH_MAX, user_path);
    if (rc != NX_OK) return rc;

#if !__STDC_HOSTED__
    /* Slice 7.6d.N.5: directory open.  Path "/" gets a HANDLE_DIR
     * with a fresh cursor; ls / busybox's opendir → getdents64
     * loop reads through it.  vfs_simple has no directory inode
     * for "/" so we'd otherwise NX_ENOENT, blocking the entire
     * directory-listing flow.  Host build has no kheap so this
     * path falls through to vfs_simple's normal path resolution
     * (which returns NX_ENOENT for "/" on host fixtures too —
     * matching the pre-slice-7.6d.N.5 behaviour). */
    if (kpath[0] == '/' && kpath[1] == '\0') {
        struct nx_dir_cursor *cur = malloc(sizeof *cur);
        if (!cur) return NX_ENOMEM;
        cur->cookie = 0;

        struct nx_handle_table *t = nx_syscall_current_table();
        nx_handle_t h = NX_HANDLE_INVALID;
        rc = nx_handle_alloc(t, NX_HANDLE_DIR, NX_RIGHT_READ, cur, &h);
        if (rc != NX_OK) { free(cur); return rc; }
        return (nx_status_t)h;
    }
#endif

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

/*
 * Handle-type-polymorphic `read` / `write`.  Slice 6.3 introduced
 * these for HANDLE_FILE; slice 7.5 teaches them to dispatch through
 * the channel layer when the handle is HANDLE_CHANNEL (which is
 * what `sys_pipe` allocates for both pipe ends).  POSIX conflates
 * these into one `read` / `write` pair — matching that expectation
 * means a `posix_read(pipe_fd)` wrapper doesn't need to know which
 * kernel primitive is under the fd.
 */
static nx_status_t sys_read(uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    nx_handle_t  h   = (nx_handle_t)a0;
    void        *buf = (void *)(uintptr_t)a1;
    size_t       cap = (size_t)a2;
    if (cap == 0) return 0;  /* no-op read is explicitly OK */

    struct nx_handle_table *t = nx_syscall_current_table();
    enum nx_handle_type type;
    uint32_t rights;
    void *obj;
    int rc;

    /* Slice 7.6d.N.6b — POSIX STDIN_FILENO = 0 routes to slot 2 (the
     * pre-installed CONSOLE read end) since encoded value 0 is
     * reserved for NX_HANDLE_INVALID and `nx_handle_lookup` would
     * reject it as a bad encoding.  Reading slot 2 directly bypasses
     * the encoded-value generation check; a dup3-redirected stdin
     * still resolves here because dup3 installs at slot 2 with gen 0
     * when newfd == 0.  No magic fall-through to EOF — if slot 2 is
     * empty (which only happens before nx_process_create runs, e.g.
     * the bare g_kernel_process used in host fixtures that bypass
     * nx_process_create), a normal-shape NX_ENOENT is returned. */
    if (h == 0) {
        if (!t || t->entries[2].type == NX_HANDLE_INVALID) return NX_ENOENT;
        type   = t->entries[2].type;
        rights = t->entries[2].rights;
        obj    = t->entries[2].object;
        rc     = NX_OK;
    } else {
        rc = nx_handle_lookup(t, h, &type, &rights, &obj);
        if (rc != NX_OK) return rc;
    }
    if ((rights & NX_RIGHT_READ) != NX_RIGHT_READ) return NX_EPERM;

    if (type == NX_HANDLE_CONSOLE) {
        /* v1: UART RX not wired — nx_console_read returns 0 = EOF.
         * Skip the user-buffer copy on the EOF case to avoid faulting
         * on a NULL `buf` when cap > 0. */
        int got = nx_console_read(0, cap);
        return (nx_status_t)got;
    }

    if (type == NX_HANDLE_CHANNEL) {
        /* Pipe read — bounded by the channel's 256-byte message size,
         * same as sys_channel_recv.  Slice 7.6d.N.6b: convert
         * NX_EAGAIN (empty + writers still attached) into a blocking
         * yield-loop so POSIX read semantics hold for shell pipelines.
         * Empty + writers all closed returns 0 = EOF directly from
         * nx_channel_recv (channel.c).  Host build doesn't have a
         * scheduler, so the loop falls through after one attempt —
         * host pipe tests don't exercise the blocking case. */
        if (cap > NX_CHANNEL_MSG_MAX) cap = NX_CHANNEL_MSG_MAX;
        uint8_t staging[NX_CHANNEL_MSG_MAX];
        int got;
        for (;;) {
            got = nx_channel_recv(obj, staging, cap);
#if !__STDC_HOSTED__
            if (got != NX_EAGAIN) break;
            nx_task_yield();
#else
            break;
#endif
        }
        if (got < 0) return got;
        rc = copy_to_user(buf, staging, (size_t)got);
        if (rc != NX_OK) return rc;
        return (nx_status_t)got;
    }

    if (type != NX_HANDLE_FILE) return NX_EINVAL;

    if (cap > NX_FILE_IO_MAX) cap = NX_FILE_IO_MAX;
    const struct nx_vfs_ops *vops; void *vself;
    rc = resolve_vfs(&vops, &vself);
    if (rc != NX_OK) return rc;

    uint8_t staging[NX_FILE_IO_MAX];
    int64_t got = vops->read(vself, obj, staging, cap);
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

    struct nx_handle_table *t = nx_syscall_current_table();
    enum nx_handle_type type;
    uint32_t rights;
    void *obj;
    int rc = nx_handle_lookup(t, h, &type, &rights, &obj);
    if (rc != NX_OK) return rc;
    if ((rights & NX_RIGHT_WRITE) != NX_RIGHT_WRITE) return NX_EPERM;

    if (type == NX_HANDLE_CONSOLE) {
        /* Slice 7.6d.N.6b — STDOUT/STDERR routed through the pre-
         * installed CONSOLE handles at slots 0/1.  Per-byte UART
         * write inside nx_console_write; copy_from_user the payload
         * into a kernel staging buffer first so we don't ask the
         * UART to dereference user pointers across a TTBR0 flip. */
        if (len > NX_FILE_IO_MAX) len = NX_FILE_IO_MAX;
        uint8_t staging[NX_FILE_IO_MAX];
        rc = copy_from_user(staging, buf, len);
        if (rc != NX_OK) return rc;
        return (nx_status_t)nx_console_write(staging, len);
    }

    if (type == NX_HANDLE_CHANNEL) {
        if (len > NX_CHANNEL_MSG_MAX) len = NX_CHANNEL_MSG_MAX;
        uint8_t staging[NX_CHANNEL_MSG_MAX];
        rc = copy_from_user(staging, buf, len);
        if (rc != NX_OK) return rc;
        return (nx_status_t)nx_channel_send(obj, staging, len);
    }

    if (type != NX_HANDLE_FILE) return NX_EINVAL;

    if (len > NX_FILE_IO_MAX) len = NX_FILE_IO_MAX;
    uint8_t staging[NX_FILE_IO_MAX];
    rc = copy_from_user(staging, buf, len);
    if (rc != NX_OK) return rc;

    const struct nx_vfs_ops *vops; void *vself;
    rc = resolve_vfs(&vops, &vself);
    if (rc != NX_OK) return rc;

    return (nx_status_t)vops->write(vself, obj, staging, len);
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

/*
 * NX_SYS_FORK — ().
 *
 * Slice 7.4a.  Duplicate the current process:
 *   - Fresh child process with its own address-space copy and
 *     (in v1) an empty handle table.
 *   - Child task with a kernel stack whose top holds a copy of the
 *     parent's trap frame with `x[0]` rewritten to 0.  When the
 *     scheduler picks the child, `nx_task_fork_child_entry`
 *     RESTORE_TRAPFRAMEs + erets — the child lands at the
 *     instruction after the fork SVC with x0 = 0.
 *   - Parent return: this function returns the child's pid, which
 *     the dispatcher stores into the parent's `tf->x[0]`.
 *
 * On host this is a no-op (no MMU, no trap frames) — returns
 * `NX_EINVAL` so host tests can't accidentally rely on it.
 */
static nx_status_t sys_fork(uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
#if __STDC_HOSTED__
    return NX_EINVAL;
#else
    struct nx_task *caller = nx_task_current();
    if (!caller || !caller->process) return NX_EINVAL;

    /* The parent's trap frame sits at the top of its kernel stack —
     * the one that SAVE_TRAPFRAME pushed on exception entry.  Recover
     * it from `nx_syscall_dispatch`'s argument, which is exactly
     * `sp` at the point the exception handler called `on_sync`.
     *
     * Subtle: this syscall body is NOT given `tf` directly — the
     * dispatch table abstracts it away.  We recover it via a
     * per-CPU stash set by `nx_syscall_dispatch` just before it
     * reads the syscall number.  See below for the stash helper. */
    extern struct trap_frame *nx_syscall_current_tf(void);
    const struct trap_frame *parent_tf = nx_syscall_current_tf();
    if (!parent_tf) return NX_EINVAL;

    struct nx_process *child = nx_process_fork(caller->process);
    if (!child) return NX_ENOMEM;

    /*
     * Slice 7.6 prereq: duplicate the parent's CHANNEL handles into
     * the child's handle table.  Each duplicate bumps the matching
     * endpoint's handle_refs so a single endpoint can survive close
     * calls from both processes — the close-on-fork pipe pattern.
     *
     * HANDLE_FILE is intentionally NOT duplicated (per-cursor state).
     * HANDLE_CONSOLE doesn't need duplication — `nx_process_create`
     * pre-installs CONSOLE at child slots 0/1/2 with the same shape
     * as the parent's, so the child's stdin/stdout/stderr already
     * mirror the parent's at fork time.  (Edge case: parent dup3'd
     * a CHANNEL onto slot 0/1/2 before fork → child still gets the
     * pre-installed CONSOLE there, NOT the parent's redirected
     * CHANNEL.  Workloads in 7.6d.N.6b dup3 AFTER fork in each
     * child, so this isn't currently exercised.  Slot-position-
     * preserving inheritance is a follow-up.)
     */
    struct nx_handle_table *parent_tbl = &caller->process->handles;
    struct nx_handle_table *child_tbl  = &child->handles;
    for (size_t i = 0; i < NX_HANDLE_TABLE_CAPACITY; i++) {
        const struct nx_handle_entry *src = &parent_tbl->entries[i];
        if (src->type != NX_HANDLE_CHANNEL) continue;
        if (!src->object) continue;
        nx_channel_endpoint_retain(src->object);
        nx_handle_t dup_h = NX_HANDLE_INVALID;
        int rc = nx_handle_alloc(child_tbl, src->type,
                                 src->rights, src->object, &dup_h);
        if (rc != NX_OK) {
            nx_channel_endpoint_close(src->object);
            nx_process_destroy(child);
            return NX_ENOMEM;
        }
    }

    struct nx_task *child_task =
        nx_task_create_forked(caller->name, parent_tf);
    if (!child_task) {
        nx_process_destroy(child);
        return NX_ENOMEM;
    }
    child_task->process = child;

    /* Enqueue the child so the scheduler picks it on a future tick.
     * Returning parent-side completes the parent's SVC normally. */
    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *sched_self = sched_self_for_test();
    if (ops) ops->enqueue(sched_self, child_task);

    return (nx_status_t)child->pid;
#endif
}

/*
 * NX_SYS_WAIT — (uint32_t pid, int *user_status).
 *
 * Slice 7.4b.  Block the caller until the target process is
 * EXITED, then deliver its exit_code to the user via
 * `copy_to_user` (if `user_status` is non-NULL) and return the
 * target's pid.
 *
 * Semantics:
 *   - pid == 0 or unknown pid: NX_ENOENT.
 *   - pid == caller's own process: NX_EINVAL (can't wait on self;
 *     matches POSIX's rejection of self-wait).
 *   - target already EXITED: copy_to_user + return pid in one go
 *     (no yielding).
 *   - target ACTIVE: yield cooperatively and recheck.  v1 uses an
 *     unbounded loop — the caller blocks "forever" in the POSIX
 *     sense until the target exits.  The ktest harness has an
 *     outer timeout (15 s) as a safety net.
 *
 * Does NOT reap the zombie.  The target process struct + its
 * handle table + address space stay live after wait returns — a
 * pid-leak in v1.  Proper reap lands when the process-table-
 * capacity becomes a pressure (currently 16 entries; > enough
 * for every test we run today).
 *
 * Host: no scheduler + no user pointer semantics — returns
 * NX_EINVAL.
 */
static nx_status_t sys_wait(uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
#if __STDC_HOSTED__
    (void)a0; (void)a1;
    return NX_EINVAL;
#else
    uint32_t  pid         = (uint32_t)a0;
    int      *user_status = (int *)(uintptr_t)a1;

    struct nx_process *caller = nx_process_current();
    struct nx_process *target = NULL;

    /* Slice 7.6d.N.6b: pid == (uint32_t)-1 (POSIX waitpid for any
     * child).  ash uses this for shell pipelines.  Scan the live
     * process table for any process whose `parent_pid` matches the
     * caller; prefer EXITED children so a ready zombie reaps
     * immediately, otherwise yield and retry until one becomes
     * ready.  Returns NX_ENOENT (≈ Linux ECHILD) if the caller has
     * no children at all. */
    if (pid == (uint32_t)-1) {
        for (;;) {
            struct nx_process *any_child = NULL;
            struct nx_process *exited_child =
                nx_process_find_exited_child(caller, &any_child);
            if (exited_child) { target = exited_child; pid = target->pid; break; }
            if (!any_child) {
                /* Linux ECHILD = 10.  Returning the NX_E* equivalent
                 * (NX_ENOENT = -4) would translate to musl errno=EINTR
                 * which ash treats as "interrupted, retry" → infinite
                 * spin.  Return -10 directly to match the POSIX
                 * "no more children" contract. */
                return -10;
            }
            nx_task_yield();
        }
    } else {
        target = nx_process_lookup_by_pid(pid);
        if (!target || target == &g_kernel_process) return NX_ENOENT;
        if (target == caller) return NX_EINVAL;

        while (target->state != NX_PROCESS_STATE_EXITED)
            nx_task_yield();
    }

    /* Deliver exit_code to user if requested.  Bad user pointer is
     * non-fatal — we still return the pid so the caller learns
     * the target exited even if the status copy failed. */
    if (user_status) {
        int kstatus = target->exit_code;
        (void)copy_to_user(user_status, &kstatus, sizeof kstatus);
    }
    /* Slice 7.6d.N.6b: mark as reaped so a subsequent waitpid(-1)
     * doesn't return the same EXITED child again.  Real reap-on-wait
     * (free the process struct) is still a follow-up — for now we
     * just hide the zombie from `nx_process_find_exited_child`. */
    target->reaped = true;
    return (nx_status_t)pid;
#endif
}

/*
 * NX_SYS_EXEC — (const char *path).
 *
 * Slice 7.4c.  Replace the current process's address space with a
 * fresh one loaded from the ELF at `path` (via the `vfs` slot),
 * transfer control to the ELF's entry point.  Doesn't return to
 * the caller on success — the SVC's `eret` delivers control to
 * the new program.
 *
 * Steps:
 *   1. `copy_path_from_user` the path into a kernel buffer.
 *   2. Resolve `vfs`; open the path for read.
 *   3. Read file bytes into a kernel buffer (capped at
 *      `SYS_EXEC_MAX_FILE`).
 *   4. `nx_elf_parse` to validate.
 *   5. Allocate a fresh address space via `mmu_create_address_
 *      space`.
 *   6. Temporarily point `current->ttbr0_root = new_root` so the
 *      loader's `mmu_address_space_user_backing` resolves the
 *      right backing chunk.
 *   7. `nx_elf_load_into_process` copies all PT_LOAD segments
 *      into the new backing.
 *   8. Modify the current trap frame (via `nx_syscall_current_tf`)
 *      to eret at the ELF's entry with zeroed registers + a
 *      top-of-user-window SP.
 *   9. `mmu_switch_address_space(new_root)` flips TTBR0.
 *   10. Free the old address space.
 *   11. Return NX_OK; dispatcher writes `tf->x[0] = 0` (redundant
 *       but harmless since we already zeroed x[0] in step 8).
 *
 * Error paths unwind cleanly — old address space stays live if any
 * prep step fails.
 *
 * Handle table carries over (v1 doesn't close inherited handles on
 * exec; a later slice can wire close-on-exec flags).
 *
 * Host: no MMU + no file I/O semantics — returns NX_EINVAL.
 */
#define SYS_EXEC_MAX_FILE  (4u * 1024u * 1024u)
                                   /* Bumped 8192 → 4 MiB in slice
                                    * 7.6d.2c so the ~2.29 MiB busybox
                                    * 1.36.1 binary can be slurped from
                                    * vfs.  Per-call kheap allocation,
                                    * so no static cost — but each
                                    * sys_exec briefly holds 4 MiB.
                                    * Falls in line with RAMFS_FILE_CAP
                                    * (also 4 MiB).  Earlier sizes:
                                    * ~150 B (init_prog), 4096
                                    * (libnxlibc demos), 8192
                                    * (musl demos). */
#define SYS_EXEC_ARGV_MAX  16u     /* slice 7.6c.4: max argv entries */
#define SYS_EXEC_ARGV_BYTES 1024u  /* total bytes of argv-string data */

static nx_status_t sys_exec(uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
#if __STDC_HOSTED__
    (void)a0; (void)a1;
    return NX_EINVAL;
#else
    const char *user_path = (const char *)(uintptr_t)a0;
    char *const *user_argv = (char *const *)(uintptr_t)a1;
    /* envp is reserved (a2) — currently always treated as the empty
     * environment.  Per-program env handling lands when a slice
     * actually needs it (likely 7.6d busybox). */

    /* 1. Path copy. */
    char kpath[NX_PATH_MAX];
    int rc = copy_path_from_user(kpath, NX_PATH_MAX, user_path);
    if (rc != NX_OK) return rc;

    /*
     * 1.5. Slice 7.6c.4: copy argv from the user (still in the OLD
     *      address space) into kernel staging.  Two reads per entry:
     *      first the pointer slot from the argv array, then the
     *      string it points to.  Stops at the first NULL pointer or
     *      when we hit the per-call caps (16 entries / 1024 bytes
     *      total — keeps the staging fixed-size on the kernel stack).
     *
     *      `argv_strs` is a packed buffer ("arg0\0arg1\0..."),
     *      `argv_offsets[i]` is the byte offset of argv[i] inside it.
     *      Both get re-laid-out onto the new user stack later.
     */
    char    argv_strs[SYS_EXEC_ARGV_BYTES];
    size_t  argv_offsets[SYS_EXEC_ARGV_MAX];
    size_t  argv_str_len = 0;
    size_t  argc = 0;

    if (user_argv) {
        while (argc < SYS_EXEC_ARGV_MAX) {
            char *user_str_ptr = NULL;
            rc = copy_from_user(&user_str_ptr,
                                (const char *)user_argv + argc * sizeof user_str_ptr,
                                sizeof user_str_ptr);
            if (rc != NX_OK) return rc;
            if (user_str_ptr == NULL) break;

            size_t remain = SYS_EXEC_ARGV_BYTES - argv_str_len;
            if (remain == 0) break;       /* truncate silently — same
                                           * shape as Linux's argv-too-
                                           * long ENAMETOOLONG, but
                                           * v1 silently drops the
                                           * tail. */
            argv_offsets[argc] = argv_str_len;
            rc = copy_path_from_user(argv_strs + argv_str_len,
                                     remain, user_str_ptr);
            if (rc != NX_OK) return rc;

            /* copy_path_from_user uses NX_PATH_MAX bound; argv strings
             * follow the same rules as paths in v1.  Advance past the
             * NUL terminator the helper wrote. */
            size_t slen = 0;
            while (argv_str_len + slen < SYS_EXEC_ARGV_BYTES &&
                   argv_strs[argv_str_len + slen] != '\0') slen++;
            argv_str_len += slen + 1;     /* +1 to include NUL */
            argc++;
        }
    }

    /* If no argv was passed (or it was an empty array), synthesise
     * argv = { path, NULL } per Linux convention. */
    if (argc == 0) {
        size_t plen = 0;
        while (plen < NX_PATH_MAX && kpath[plen] != '\0') plen++;
        if (plen + 1 > SYS_EXEC_ARGV_BYTES) plen = SYS_EXEC_ARGV_BYTES - 1;
        memcpy(argv_strs, kpath, plen);
        argv_strs[plen] = '\0';
        argv_offsets[0] = 0;
        argv_str_len = plen + 1;
        argc = 1;
    }

    /* 2. Resolve vfs + open. */
    const struct nx_vfs_ops *vops; void *vself;
    rc = resolve_vfs(&vops, &vself);
    if (rc != NX_OK) return rc;

    void *file = NULL;
    rc = vops->open(vself, kpath, NX_VFS_OPEN_READ, &file);
    if (rc != NX_OK) return rc;

    /* 3. Slurp file contents. */
    uint8_t *kbuf = malloc(SYS_EXEC_MAX_FILE);
    if (!kbuf) { vops->close(vself, file); return NX_ENOMEM; }

    size_t total = 0;
    while (total < SYS_EXEC_MAX_FILE) {
        int64_t n = vops->read(vself, file, kbuf + total,
                               SYS_EXEC_MAX_FILE - total);
        if (n < 0)  { vops->close(vself, file); free(kbuf); return (nx_status_t)n; }
        if (n == 0) break;
        total += (size_t)n;
    }
    vops->close(vself, file);

    /* 4. Validate ELF. */
    struct nx_elf_info info;
    rc = nx_elf_parse(kbuf, total, &info);
    if (rc != NX_OK) { free(kbuf); return rc; }

    /* 5. Allocate a fresh address space. */
    uint64_t new_root = mmu_create_address_space();
    if (new_root == 0) { free(kbuf); return NX_ENOMEM; }

    /* 6-7. Swap the current process's root so the loader resolves
     * the new backing; load segments. */
    struct nx_process *p = nx_process_current();
    uint64_t old_root = p->ttbr0_root;
    uint64_t old_brk  = p->brk_addr;
    uint64_t old_mmap = p->mmap_bump;
    p->ttbr0_root = new_root;
    /* Slice 7.6c.3c — fresh address space gets a fresh program
     * break.  exec replaces the entire image, so the inherited
     * brk_addr from the pre-exec process is meaningless. */
    p->brk_addr = mmu_user_window_base() + NX_PROCESS_HEAP_OFFSET;
    /* Slice 7.6d.N.1 — same logic for the mmap arena bump pointer.
     * The new image's musl will issue its own mmap calls. */
    p->mmap_bump = mmu_user_window_base() + NX_PROCESS_MMAP_OFFSET;

    uint64_t entry = 0;
    rc = nx_elf_load_into_process(p, kbuf, total, &entry);
    free(kbuf);
    if (rc != NX_OK) {
        /* Roll back the process's root before destroying the
         * half-loaded new one — leave the caller in its original
         * address space with its original PC intact. */
        p->ttbr0_root = old_root;
        p->brk_addr   = old_brk;
        p->mmap_bump  = old_mmap;
        mmu_destroy_address_space(new_root);
        return rc;
    }

    /*
     * 7.5. Slice 7.6c.4: build the System V argv layout in the new
     *      user backing.  We're between `nx_elf_load_into_process`
     *      and the TTBR0 flip, so the new backing is reachable
     *      through the kernel-visible alias from
     *      `mmu_address_space_user_backing` (same trick the ELF
     *      loader uses for its PT_LOAD copies).
     *
     *      Stack layout, low (sp) to high (slice 7.6c.3b extends
     *      with AUXV between envp NULL and the argv strings):
     *        sp+0:                argc
     *        sp+8 .. sp+8+8*argc: argv[0]..argv[argc-1]
     *        sp+8*(argc+1):       NULL  (argv terminator)
     *        sp+8*(argc+2):       NULL  (envp[0] = end-of-envp)
     *        sp+8*(argc+3..4):    AT_PAGESZ pair (key=6, val=4096)
     *        sp+8*(argc+5..6):    AT_RANDOM pair (key=25, val=&pad)
     *        sp+8*(argc+7..8):    AT_NULL pair (key=0, val=0)
     *        ...                  16-byte AT_RANDOM pad (8-aligned)
     *        ...                  argv strings (8-aligned, at top)
     *
     *      sp_el0 lands at the `argc` slot.  16-byte-aligned per AAPCS.
     *
     *      AUXV is what musl's `__libc_start_main` walks during
     *      libc init.  AT_PAGESZ feeds malloc's allocation rounding;
     *      AT_RANDOM is dereferenced by `__init_ssp` to seed the
     *      stack canary.  v1 entropy comes from the ARM virtual
     *      counter (`nx_monotonic_raw`) — pseudo, not cryptographic;
     *      a real entropy source is a Phase-9 follow-up.  We don't
     *      emit AT_PHDR/AT_PHENT/AT_PHNUM (musl uses them only when
     *      walking PT_TLS — our static-no-thread programs don't have
     *      one, so the absent entries are equivalent to "loop runs
     *      zero iterations").
     */
    void *new_backing = mmu_address_space_user_backing(new_root);
    uint64_t window_base = mmu_user_window_base();
    uint64_t window_size = mmu_user_window_size();
    uint64_t new_sp;
    if (new_backing) {
        /* Strings sit at the high end of the window. */
        uint64_t str_offset = (window_size - argv_str_len) & ~(uint64_t)7u;
        memcpy((char *)new_backing + str_offset, argv_strs, argv_str_len);

        /* 16-byte AT_RANDOM pad sits below the strings, 8-aligned. */
        uint64_t at_random_offset = (str_offset - 16u) & ~(uint64_t)7u;
        {
            uint8_t *pad = (uint8_t *)new_backing + at_random_offset;
            bool ok = false;
            uint64_t seed = nx_monotonic_raw(&ok);
            /* Splat the seed into 16 bytes with a per-byte spreader
             * so the canary differs even when seed-bits-of-interest
             * are sparse.  Not cryptographic — see header comment. */
            for (int i = 0; i < 16; i++)
                pad[i] = (uint8_t)((seed >> (i * 4)) ^ (uint8_t)(i * 0x37));
        }

        /* Pointer area below the AT_RANDOM pad.  Slot count:
         *   1 (argc) + argc (argv body) + 1 (argv NULL)
         *   + 1 (envp NULL) + 12 (AUXV: 6 pairs × 2)
         *   = argc + 15 slots × 8 bytes.
         * Round sp down to 16 for AAPCS.  Slice 7.6d.3c added the
         * AT_PHDR / AT_PHENT / AT_PHNUM trio (3 pairs) on top of
         * the slice-7.6c.3b AT_PAGESZ + AT_RANDOM + AT_NULL — needed
         * because musl's static_init_tls walks the phdr table via
         * those AT_* values to find PT_TLS.  Without them, the
         * walk loops zero times and downstream TLS-allocation math
         * gets garbage state, eventually faulting on a NULL+0x20
         * write.  AT_PHDR is computed as `window_base + e_phoff` —
         * for our static-no-pie binaries the ELF's first PT_LOAD
         * starts at file offset 0 and maps to window_base, so
         * file offset e_phoff lives at VA window_base + e_phoff. */
        uint64_t fixed_size = 8u * (15u + argc);
        uint64_t sp_offset  = (at_random_offset - fixed_size) & ~(uint64_t)15u;

        uint64_t *u = (uint64_t *)((char *)new_backing + sp_offset);
        u[0] = argc;
        for (size_t i = 0; i < argc; i++)
            u[1 + i] = window_base + str_offset + argv_offsets[i];
        u[1 + argc] = 0;            /* argv terminator */
        u[2 + argc] = 0;            /* envp[0] = NULL */
        u[3 + argc] = 6;            /* AT_PAGESZ */
        u[4 + argc] = 4096;
        u[5 + argc] = 25;           /* AT_RANDOM */
        u[6 + argc] = window_base + at_random_offset;
        u[7 + argc] = 3;            /* AT_PHDR */
        u[8 + argc] = window_base + info.phoff;
        u[9 + argc] = 4;            /* AT_PHENT */
        u[10 + argc] = info.phentsize;
        u[11 + argc] = 5;           /* AT_PHNUM */
        u[12 + argc] = info.phnum;
        u[13 + argc] = 0;           /* AT_NULL */
        u[14 + argc] = 0;

        new_sp = window_base + sp_offset;
    } else {
        /* Defensive fallback if the backing alias isn't available
         * (host build, or a future caller without an MMU window). */
        new_sp = (window_base + window_size) & ~(uint64_t)0xfu;
    }

    /* 8. Clobber the trap frame so eret delivers to the ELF entry
     * with a clean register state.  The dispatcher will overwrite
     * `tf->x[0]` with our return value (0 for NX_OK), which is the
     * same value we're setting here — consistent either way. */
    extern struct trap_frame *nx_syscall_current_tf(void);
    struct trap_frame *tf = nx_syscall_current_tf();
    if (tf) {
        for (int i = 0; i < 31; i++) tf->x[i] = 0;
        tf->pc = entry;
        tf->sp_el0 = new_sp;
        /* Clear PSTATE to a clean "EL0t, IRQs enabled" state.  The
         * existing drop_to_el0 writes SPSR_EL1=0 for the same reason:
         * EL0t mode bits are 0, DAIF=0 gives IRQs enabled. */
        tf->pstate = 0;
    }

    /* 9. Flip TTBR0 to the new address space.  Kernel code page
     * stays reachable because every address-space root shares the
     * kernel identity map. */
    mmu_switch_address_space(new_root);

    /* Slice 7.6d.3c: reset TPIDR_EL0 to the kernel-pre-init TLS
     * area in the new address space.  exec replaces the entire
     * image — if the pre-exec process had run musl and called
     * `__set_thread_area(td)`, TPIDR_EL0 would be pointing into
     * the now-freed old user_backing.  The new image's musl
     * `__init_libc` reads TPIDR_EL0 immediately, so we have to
     * point it at a valid zeroed region in the new address space.
     * The corresponding nx_task field is updated too so the next
     * context switch out + back in restores the correct value. */
    {
        uint64_t fresh_tls =
            mmu_user_window_base() + NX_PROCESS_TLS_OFFSET;
        asm volatile ("msr tpidr_el0, %0" :: "r"(fresh_tls));
        struct nx_task *self = nx_task_current();
        if (self) self->tpidr_el0 = fresh_tls;
    }

    /* 10. Reclaim the old address space. */
    mmu_destroy_address_space(old_root);

    /* 11. Return.  Dispatcher writes tf->x[0] = NX_OK = 0 (same
     * value we already wrote).  Then RESTORE_TRAPFRAME + eret
     * delivers control to the ELF's entry with x0=0 + zeroed
     * other registers + fresh user stack. */
    return NX_OK;
#endif
}

/*
 * NX_SYS_PIPE — (int fds[2]).
 *
 * Slice 7.5.  Wraps the slice-5.6 channel primitive into a POSIX-
 * style pipe: one endpoint carries NX_RIGHT_READ (fds[0], the read
 * side), the other NX_RIGHT_WRITE (fds[1], the write side).  No
 * NX_RIGHT_TRANSFER in v1 — pipes don't cross process boundaries
 * via IPC cap transfer yet (they cross naturally through fork,
 * which inherits the handle table once that's plumbed; for now
 * the pipe test has parent + child share by handing both ends to
 * the child via its own fork before drop_to_el0).
 *
 * Returns NX_OK on success; the two handles land in fds[0] and
 * fds[1] via `copy_to_user`.  The ABI intentionally uses 32-bit
 * int for the handle values because POSIX pipe writes int fds; our
 * `nx_handle_t` is already `uint32_t` so the width matches.
 *
 * Handle rollback on partial-allocation failure uses
 * `nx_handle_close` + `nx_channel_endpoint_close` mirror calls,
 * matching `sys_channel_create`'s unwind.
 */
static nx_status_t sys_pipe(uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    int *user_fds = (int *)(uintptr_t)a0;
    if (!user_fds) return NX_EINVAL;

    struct nx_channel_endpoint *e0 = 0, *e1 = 0;
    int rc = nx_channel_create(&e0, &e1);
    if (rc != NX_OK) return rc;

    struct nx_handle_table *t = nx_syscall_current_table();
    nx_handle_t h_read  = NX_HANDLE_INVALID;
    nx_handle_t h_write = NX_HANDLE_INVALID;

    rc = nx_handle_alloc(t, NX_HANDLE_CHANNEL, NX_RIGHT_READ, e0, &h_read);
    if (rc != NX_OK) {
        nx_channel_endpoint_close(e0);
        nx_channel_endpoint_close(e1);
        return rc;
    }
    rc = nx_handle_alloc(t, NX_HANDLE_CHANNEL, NX_RIGHT_WRITE, e1, &h_write);
    if (rc != NX_OK) {
        nx_handle_close(t, h_read);
        nx_channel_endpoint_close(e0);
        nx_channel_endpoint_close(e1);
        return rc;
    }

    int fds[2];
    fds[0] = (int)h_read;
    fds[1] = (int)h_write;
    rc = copy_to_user(user_fds, fds, sizeof fds);
    if (rc != NX_OK) {
        nx_handle_close(t, h_read);
        nx_handle_close(t, h_write);
        nx_channel_endpoint_close(e0);
        nx_channel_endpoint_close(e1);
        return rc;
    }
    return NX_OK;
}

/*
 * NX_SYS_SIGNAL — (uint32_t pid, int signo).
 *
 * Slice 7.5.  Sets the matching bit in the target process's
 * `pending_signals` bitmask.  Unknown pid (or pid 0) returns
 * NX_ENOENT; unsupported signo returns NX_EINVAL.  v1 supports
 * only SIGTERM (15) and SIGKILL (9) — both are terminating; the
 * distinction (catchable vs not) lands with signal handlers in a
 * later slice.
 *
 * Delivery itself is polled, not async.  Actual exit happens the
 * next time the target enters `sched_check_resched` (timer
 * preemption, yield, or syscall entry via the dispatch shim).
 * v1's "Real signal delivery (async interrupt of the target)
 * lands with a later slice" promise from the Implementation Guide.
 *
 * Sending to your own process is allowed — useful for raise().
 */
static nx_status_t sys_signal(uint64_t a0, uint64_t a1, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    uint32_t pid   = (uint32_t)a0;
    int      signo = (int)a1;

    if (signo != NX_SIGTERM && signo != NX_SIGKILL) return NX_EINVAL;

    struct nx_process *target = nx_process_lookup_by_pid(pid);
    if (!target || target == &g_kernel_process) return NX_ENOENT;

    __atomic_fetch_or(&target->pending_signals,
                      (uint32_t)(1u << signo),
                      __ATOMIC_RELEASE);
    return NX_OK;
}

/*
 * NX_SYS_WRITEV — (nx_handle_t h, const struct iovec *iov, int iovcnt)
 * → total bytes written / NX_E*.
 *
 * Slice 7.6c.3c.  musl's stdio (`__stdio_write`) flushes buffered
 * output via SYS_writev — one writev per fflush.  Without this
 * syscall musl's printf silently sets F_ERR and discards the
 * output.
 *
 * Implementation: walk the iovec array from user memory (each
 * entry is `{ void *iov_base; size_t iov_len }` = 16 bytes on
 * aarch64), copy_from_user each iovec slot to a kernel-side struct,
 * dispatch each entry through sys_write so all the existing
 * magic-fd / handle-table / CHANNEL / FILE branches apply.
 *
 * Cap iovcnt at IOV_MAX_LOCAL = 16.  Linux's IOV_MAX is 1024;
 * v1's stdio buffer-flush + a few iovecs is well below 16.
 */
struct nx_iovec_user {
    uint64_t iov_base;   /* user VA */
    uint64_t iov_len;
};

#define NX_IOV_MAX_LOCAL 16u

static nx_status_t sys_writev(uint64_t a0, uint64_t a1, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    nx_handle_t h     = (nx_handle_t)a0;
    const void *iovs  = (const void *)(uintptr_t)a1;
    int         count = (int)a2;

    if (count < 0) return NX_EINVAL;
    if (count == 0) return 0;
    if ((unsigned int)count > NX_IOV_MAX_LOCAL) return NX_EINVAL;

    struct nx_iovec_user kiov[NX_IOV_MAX_LOCAL];
    int rc = copy_from_user(kiov, iovs,
                            (size_t)count * sizeof(struct nx_iovec_user));
    if (rc != NX_OK) return rc;

    int64_t total = 0;
    for (int i = 0; i < count; i++) {
        if (kiov[i].iov_len == 0) continue;
        nx_status_t got = sys_write((uint64_t)h,
                                    kiov[i].iov_base,
                                    kiov[i].iov_len,
                                    0, 0, 0);
        if (got < 0) return total > 0 ? (nx_status_t)total : got;
        total += got;
        /* Short write: a partial iovec entry was accepted.  Match
         * Linux's writev semantics: stop on the first short entry
         * and return the running total. */
        if ((uint64_t)got < kiov[i].iov_len) break;
    }
    return (nx_status_t)total;
}

/*
 * NX_SYS_READV — (nx_handle_t h, const struct iovec *iov, int iovcnt)
 * → total bytes read / NX_E*.
 *
 * Slice 7.6d.N.6.  Symmetric pair to sys_writev.  musl's
 * `__stdio_read` flushes buffered input via SYS_readv — cat's main
 * loop reads chunks of stdin via fread → __stdio_read → readv.
 * Without this syscall, cat sees -ENOSYS (which our v1 collides
 * with -EPERM but the failure shape is the same: cat aborts with
 * "read error").
 *
 * Implementation mirrors sys_writev: walk the iovec, dispatch each
 * entry through sys_read so the magic-fd / handle-table / CHANNEL
 * / FILE branches apply.  Stop on first short read per Linux readv
 * convention.  iovcnt cap NX_IOV_MAX_LOCAL = 16.
 */
static nx_status_t sys_readv(uint64_t a0, uint64_t a1, uint64_t a2,
                             uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    nx_handle_t h     = (nx_handle_t)a0;
    const void *iovs  = (const void *)(uintptr_t)a1;
    int         count = (int)a2;

    if (count < 0) return NX_EINVAL;
    if (count == 0) return 0;
    if ((unsigned int)count > NX_IOV_MAX_LOCAL) return NX_EINVAL;

    struct nx_iovec_user kiov[NX_IOV_MAX_LOCAL];
    int rc = copy_from_user(kiov, iovs,
                            (size_t)count * sizeof(struct nx_iovec_user));
    if (rc != NX_OK) return rc;

    int64_t total = 0;
    for (int i = 0; i < count; i++) {
        if (kiov[i].iov_len == 0) continue;
        nx_status_t got = sys_read((uint64_t)h,
                                   kiov[i].iov_base,
                                   kiov[i].iov_len,
                                   0, 0, 0);
        if (got < 0) return total > 0 ? (nx_status_t)total : got;
        total += got;
        /* Short read (including 0 = EOF): match Linux readv —
         * stop and return running total.  cat's loop will treat
         * 0 as EOF and exit cleanly. */
        if ((uint64_t)got < kiov[i].iov_len) break;
    }
    return (nx_status_t)total;
}

/*
 * NX_SYS_BRK — (uint64_t requested) → new break (slice 7.6c.3c).
 *
 * Linux brk(2) semantics: requested == 0 returns the current break;
 * otherwise tries to set the break to `requested` and returns the
 * resulting break.  On failure (out-of-range), Linux's convention is
 * to return the OLD break unchanged so the caller detects failure
 * by comparing the returned value to its requested value (musl's
 * mallocng does exactly this).  No errno code is returned.
 *
 * Heap region: [base + NX_PROCESS_HEAP_OFFSET ..
 *               base + NX_PROCESS_HEAP_LIMIT) — 1.5 MiB inside the
 * existing 8 MiB user-window backing (slice 7.6d.2b grew the window
 * from 2 MiB to 8 MiB; heap relocated from [+1 MiB, +1.5 MiB) to
 * [+6 MiB, +7.5 MiB) so it doesn't overlap busybox's data/bss).
 * No extra kernel allocation; we just track the high-water mark per
 * process.
 *
 * The host build path returns 0 (no MMU, no heap concept).  Kernel
 * tests exercise the real path.
 */
static nx_status_t sys_brk(uint64_t a0, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    uint64_t requested = a0;

    struct nx_process *p = nx_process_current();
    if (!p) return NX_EINVAL;

#if !__STDC_HOSTED__
    uint64_t base = mmu_user_window_base();
    uint64_t lo   = base + NX_PROCESS_HEAP_OFFSET;
    uint64_t hi   = base + NX_PROCESS_HEAP_LIMIT;
    /* brk(0): return current break.  Also the path the very first
     * mallocng call takes to learn where its arena starts. */
    if (requested == 0) return (nx_status_t)p->brk_addr;
    /* Out-of-range requests don't error — return the old break and
     * let the caller detect failure (musl convention). */
    if (requested < lo || requested > hi) return (nx_status_t)p->brk_addr;
    p->brk_addr = requested;
    return (nx_status_t)p->brk_addr;
#else
    (void)requested;
    return 0;
#endif
}

/*
 * NX_SYS_MMAP — (void *addr, size_t length, int prot, int flags,
 *                int fd, off_t offset) → user VA / -errno (slice 7.6d.N.1).
 *
 * v1 supports the mallocng shape only:
 *   - addr ignored (kernel always picks)
 *   - fd must be -1 (no file-backed mappings)
 *   - offset must be 0
 *   - flags must include MAP_ANONYMOUS|MAP_PRIVATE; MAP_FIXED rejected
 *   - prot ignored — the user window is uniformly EL0-RW
 *
 * Length is rounded up to a 4 KiB page.  The kernel picks an address
 * from a per-process bump arena carved out of the unused window
 * region [+NX_PROCESS_MMAP_OFFSET, +NX_PROCESS_MMAP_LIMIT) — bumped
 * up; out-of-arena returns -ENOMEM.
 *
 * Returns the chosen VA on success.  On failure, returns a small
 * negative value that musl's __syscall_ret turns into MAP_FAILED +
 * errno.  Specifically: NX_EINVAL (bad shape) or NX_ENOMEM (arena
 * exhausted).
 *
 * Pages are zeroed on the way out so MAP_ANONYMOUS's zero-init
 * contract holds — the underlying user_window backing is plain
 * malloc'd memory and can carry stale bytes from earlier exec
 * cycles or from un-touched memcpy alignment slack.  We're in the
 * caller's address space (the SVC trap entered from EL0 and didn't
 * flip TTBR0), so writing via the user VA from kernel context is
 * safe and goes to the right physical backing.
 *
 * Linux's MAP_ANONYMOUS flag has different numeric values on
 * different platforms; on aarch64 it's 0x20.  MAP_PRIVATE is 0x02.
 * MAP_FIXED is 0x10.  We only check MAP_ANONYMOUS + reject
 * MAP_FIXED; MAP_PRIVATE is implicit.
 *
 * The host build returns -ENOSYS-equivalent (NX_EINVAL) — host has
 * no MMU, no user window, and no test exercises mmap directly.
 */
#define NX_MAP_PRIVATE   0x02
#define NX_MAP_FIXED     0x10
#define NX_MAP_ANONYMOUS 0x20

static nx_status_t sys_mmap(uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a0;       /* addr — kernel chooses */
    (void)a2;       /* prot — uniformly EL0-RW */
    uint64_t length = a1;
    int      flags  = (int)a3;
    int      fd     = (int)(int32_t)a4;
    uint64_t offset = a5;

    if (length == 0) return NX_EINVAL;
    if (fd != -1)    return NX_EINVAL;
    if (offset != 0) return NX_EINVAL;
    if (!(flags & NX_MAP_ANONYMOUS)) return NX_EINVAL;
    if (flags & NX_MAP_FIXED)        return NX_EINVAL;

    /* Round up to a 4 KiB page.  Length overflow on the rounding
     * add gets treated as "too big to fit in the arena" further
     * down; no need for a separate overflow check here. */
    const uint64_t page = 4096u;
    uint64_t want = (length + page - 1u) & ~(page - 1u);

    struct nx_process *p = nx_process_current();
    if (!p) return NX_EINVAL;

#if !__STDC_HOSTED__
    uint64_t base = mmu_user_window_base();
    uint64_t lo   = base + NX_PROCESS_MMAP_OFFSET;
    uint64_t hi   = base + NX_PROCESS_MMAP_LIMIT;

    /* First call: bump_pointer hasn't been touched (or process was
     * just exec'd).  Initialize lazily so a future caller that
     * sets `mmap_bump = 0` to mean "reset" still works.  In v1 the
     * bump is always primed in nx_process_create / sys_exec, but
     * the lazy init costs nothing and protects against the bump
     * drifting below `lo` from a hypothetical future munmap. */
    if (p->mmap_bump < lo || p->mmap_bump > hi) p->mmap_bump = lo;

    if (want > hi - p->mmap_bump) return NX_ENOMEM;

    uint64_t va = p->mmap_bump;
    p->mmap_bump += want;

    /* Zero the returned region.  We're in the caller's address
     * space (SVC entered from EL0 doesn't flip TTBR0), so writing
     * via `va` from kernel context goes to this process's user
     * backing.  memset rather than a loop because the helper is
     * written with vector ops on aarch64 — fastest path. */
    memset((void *)(uintptr_t)va, 0, (size_t)want);

    return (nx_status_t)va;
#else
    (void)want;
    return NX_EINVAL;
#endif
}

/*
 * NX_SYS_MUNMAP — (void *addr, size_t length) → 0 (slice 7.6d.N.1).
 *
 * v1 doesn't reclaim mmap'd pages — PMM reclaim happens at process
 * exit when the whole user_window backing is freed.  Returning a
 * no-op success matches mallocng's expectation that munmap never
 * fails (it treats failure as fatal).  We leak the page until
 * process exit; for ash's startup that's at most a few hundred
 * KiB held until the parent reaps the child.
 *
 * Argument validation is intentionally loose: we don't check that
 * `addr` falls inside the arena or that `length` matches a prior
 * mmap call — mallocng can call munmap with arbitrary derived
 * pointers and we'd rather not fail it.
 */
static nx_status_t sys_munmap(uint64_t a0, uint64_t a1, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return NX_OK;
}

/*
 * Slice 7.6d.N.4 — minimal `fstatat` for ash's PATH walk.
 *
 * ash's `find_command()` stat()s each PATH-prefixed candidate
 * (`/sbin/ls`, `/usr/sbin/ls`, `/bin/ls`, ...) before invoking
 * execve, and bails with "command not found" if every candidate
 * stat fails.  Without this syscall every candidate's stat
 * returns -ENOSYS (or whatever musl's translate-fail produces),
 * and ash never reaches execve even when `/bin/ls` exists.
 *
 * Linux struct stat layout on aarch64 — kernel-side only fills
 * the fields ash actually reads (mode + size); the rest stay
 * zero-initialized via kbuf clearing.
 *
 * Returns Linux errno values directly (-2 for ENOENT, -22 for
 * EINVAL).  Our internal NX_E* don't match Linux numbering;
 * other syscalls escape that mismatch because their callers
 * are libnxlibc/musl wrappers that don't strictly check errno.
 * ash's path-search loop DOES check errno: ENOENT means "next
 * candidate" while other values can mean "fatal" (e.g. EACCES
 * on a real Unix means "found but not executable, stop").  So
 * here we deliberately return Linux errno.
 *
 * Implementation: open the path read-only via vfs.  If open
 * succeeds the file exists; we seek to end to read the size,
 * then close.  Hand-build the stat buf in kernel memory and
 * copy_to_user.  AT_FDCWD (dirfd = -100) is implicit because
 * vfs_simple takes absolute paths only — `dirfd` is ignored.
 *
 * `flags` (e.g. AT_SYMLINK_NOFOLLOW) is also ignored — vfs has
 * no symlinks in v1.
 */
struct nx_user_stat_aarch64 {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t __pad;
    int64_t  st_size;
    int32_t  st_blksize;
    int32_t  __pad2;
    int64_t  st_blocks;
    int64_t  st_atim_sec;  int64_t st_atim_nsec;
    int64_t  st_mtim_sec;  int64_t st_mtim_nsec;
    int64_t  st_ctim_sec;  int64_t st_ctim_nsec;
    uint32_t __unused[2];
};

#define NX_LINUX_ENOENT     (-2)
#define NX_LINUX_EBADF      (-9)
#define NX_LINUX_EINVAL    (-22)
#define NX_LINUX_S_IFREG  0100000u
#define NX_LINUX_S_IFDIR   040000u
#define NX_LINUX_FILE_MODE (NX_LINUX_S_IFREG | 0755u)
#define NX_LINUX_DIR_MODE  (NX_LINUX_S_IFDIR | 0755u)
#define NX_LINUX_DT_REG     8u
#define NX_LINUX_DT_DIR     4u

static nx_status_t sys_fstatat(uint64_t a0, uint64_t a1, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a0; (void)a3; (void)a4; (void)a5;
    const char *user_path = (const char *)(uintptr_t)a1;
    void       *user_buf  = (void *)(uintptr_t)a2;

    if (!user_path || !user_buf) return NX_LINUX_EINVAL;

    char kpath[NX_PATH_MAX];
    int rc = copy_path_from_user(kpath, NX_PATH_MAX, user_path);
    if (rc != NX_OK) return NX_LINUX_EINVAL;

    /* Slice 7.6d.N.5: special-case "/" as the root directory.
     * vfs_simple has no directory inode for "/" (flat namespace,
     * files at top level), so going through vops->open would
     * NX_ENOENT and ash/ls would conclude "/" doesn't exist. */
    int is_root_dir = (kpath[0] == '/' && kpath[1] == '\0');

    int64_t size = 0;
    uint32_t mode = NX_LINUX_FILE_MODE;
    if (is_root_dir) {
        mode = NX_LINUX_DIR_MODE;
    } else {
        const struct nx_vfs_ops *vops; void *vself;
        rc = resolve_vfs(&vops, &vself);
        if (rc != NX_OK) return NX_LINUX_EINVAL;

        void *file = 0;
        rc = vops->open(vself, kpath, NX_VFS_OPEN_READ, &file);
        if (rc != NX_OK) return NX_LINUX_ENOENT;

        size = vops->seek(vself, file, 0, NX_VFS_SEEK_END);
        vops->close(vself, file);
        if (size < 0) size = 0;
    }

    struct nx_user_stat_aarch64 kbuf;
    memset(&kbuf, 0, sizeof kbuf);
    kbuf.st_mode = mode;
    kbuf.st_nlink = 1;
    kbuf.st_size = size;
    kbuf.st_blksize = 512;
    kbuf.st_blocks = (size + 511) / 512;

    rc = copy_to_user(user_buf, &kbuf, sizeof kbuf);
    if (rc != NX_OK) return NX_LINUX_EINVAL;
    return NX_OK;
}

/*
 * Slice 7.6d.N.5 — `getdents64` for ash's `ls /`.
 *
 * Linux ABI: `int getdents64(int fd, struct linux_dirent64 *buf,
 * size_t count)` — packs as many variable-length records as fit
 * in `count` bytes, returns total bytes written, 0 at EOF, or
 * a small negative -errno.
 *
 * The fd must point at a HANDLE_DIR (allocated by sys_open with
 * path "/").  The cursor lives inside the handle's per-handle
 * object, so successive calls advance through the namespace.
 *
 * Layout per record:
 *
 *   uint64_t d_ino;        +0
 *   int64_t  d_off;        +8   (cookie of NEXT entry — opaque
 *                                seek hint, not actually used by
 *                                ls but included for ABI fidelity)
 *   uint16_t d_reclen;    +16   (record length, 8-byte aligned)
 *   uint8_t  d_type;      +18   (DT_REG, DT_DIR, ...)
 *   char     d_name[];    +19   (NUL-terminated)
 *
 * v1: every entry from ramfs is reported as DT_REG (no real
 * type info in our flat namespace).  ls's plain (non-`-l`) mode
 * doesn't care about d_type — it just prints names.
 */
struct nx_linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[1];     /* trailing flexible array */
};

static nx_status_t sys_getdents64(uint64_t a0, uint64_t a1, uint64_t a2,
                                  uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
#if __STDC_HOSTED__
    (void)a0; (void)a1; (void)a2;
    return NX_LINUX_EINVAL;
#else
    nx_handle_t  h        = (nx_handle_t)a0;
    void        *user_buf = (void *)(uintptr_t)a1;
    size_t       cap      = (size_t)a2;
    if (!user_buf || cap == 0) return NX_LINUX_EINVAL;

    struct nx_handle_table *t = nx_syscall_current_table();
    enum nx_handle_type type;
    void               *obj = 0;
    int rc = nx_handle_lookup(t, h, &type, 0, &obj);
    if (rc != NX_OK || type != NX_HANDLE_DIR || !obj)
        return NX_LINUX_EBADF;

    struct nx_dir_cursor *cur = (struct nx_dir_cursor *)obj;

    const struct nx_vfs_ops *vops; void *vself;
    rc = resolve_vfs(&vops, &vself);
    if (rc != NX_OK) return NX_LINUX_EINVAL;

    /* Pack entries into a kernel staging buffer first.  Cap to a
     * single page so the staging stays bounded; ls calls
     * getdents64 in a loop until it returns 0, so a partial
     * fill is fine. */
    enum { STAGING_MAX = 4096 };
    uint8_t staging[STAGING_MAX];
    size_t  out_off = 0;
    if (cap > STAGING_MAX) cap = STAGING_MAX;

    for (;;) {
        struct nx_fs_dirent kent;
        rc = vops->readdir(vself, &cur->cookie, &kent);
        if (rc == NX_ENOENT) break;          /* end of dir */
        if (rc != NX_OK) return NX_LINUX_EINVAL;

        /* Compute name length from the fixed-size 64-byte field. */
        size_t name_len = 0;
        while (name_len < sizeof kent.name - 1 && kent.name[name_len])
            name_len++;
        if (name_len == 0) continue;          /* skip empty slot */

        /* Strip leading '/' from ramfs entries — getdents64
         * returns basenames, not absolute paths.  Our ramfs
         * stores names without the '/' but `ramfs_create_file`
         * (and the cpio loader) keeps the leading '/' for some
         * entries.  Be defensive. */
        const char *src = kent.name;
        if (src[0] == '/') { src++; name_len--; }

        /* d_reclen rounded up to 8-byte alignment.  Header is
         * 19 bytes (8+8+2+1) + name + '\0' + alignment pad. */
        size_t hdr   = (size_t)((char *)&((struct nx_linux_dirent64 *)0)->d_name);
        size_t want  = hdr + name_len + 1;
        size_t reclen = (want + 7u) & ~7u;

        if (out_off + reclen > cap) {
            /* No room — rewind cookie so the next call resumes
             * with this entry.  Cookie is just an index over the
             * flat table, so the rewind is "decrement by 1" after
             * advancing for the entry we just read. */
            cur->cookie--;
            break;
        }

        struct nx_linux_dirent64 *e =
            (struct nx_linux_dirent64 *)(staging + out_off);
        e->d_ino   = cur->cookie;             /* opaque-but-stable */
        e->d_off   = cur->cookie;             /* same — ls doesn't use */
        e->d_reclen = (uint16_t)reclen;
        e->d_type  = NX_LINUX_DT_REG;         /* v1: all-regular */
        memcpy(e->d_name, src, name_len);
        e->d_name[name_len] = '\0';
        /* Zero any alignment padding so user reads are deterministic. */
        for (size_t p = hdr + name_len + 1; p < reclen; p++)
            ((char *)e)[p] = 0;

        out_off += reclen;
    }

    if (out_off > 0) {
        rc = copy_to_user(user_buf, staging, out_off);
        if (rc != NX_OK) return NX_LINUX_EINVAL;
    }
    return (nx_status_t)out_off;
#endif
}

/*
 * Slice 7.6d.N.5 — Linux-shape openat wrapper.
 *
 * musl's `open()` becomes `openat(AT_FDCWD, path, flags, mode)`
 * on aarch64 (no plain `open` syscall).  The Linux ABI:
 *   - dirfd at a0 (we ignore — vfs_simple is absolute-only)
 *   - path  at a1
 *   - flags at a2 (Linux O_* bits, not our NX_VFS_OPEN_*)
 *   - mode  at a3 (ignored — no perms in v1)
 *
 * Linux O_* (octal):  O_RDONLY=0, O_WRONLY=1, O_RDWR=2,
 *                     O_CREAT=0o100, O_DIRECTORY=0o200000.
 *
 * Our NX_VFS_OPEN_*:  READ=1, WRITE=2, CREATE=4.
 *
 * Conversion table is small enough to inline here.
 */
#define NX_LINUX_O_RDONLY     0u
#define NX_LINUX_O_WRONLY     1u
#define NX_LINUX_O_RDWR       2u
#define NX_LINUX_O_CREAT   0100u
#define NX_LINUX_O_DIRECTORY 0200000u

static nx_status_t sys_openat(uint64_t a0, uint64_t a1, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a0; (void)a3; (void)a4; (void)a5;
    /* Forward to sys_open with the Linux flags converted to
     * our NX_VFS_OPEN_* shape.  sys_open already special-cases
     * "/" → HANDLE_DIR (slice 7.6d.N.5 above), so opendir's
     * `openat(AT_FDCWD, "/", O_RDONLY|O_DIRECTORY)` lands on
     * the directory-cursor path automatically. */
    uint32_t lin_flags = (uint32_t)a2;
    uint32_t nx_flags  = 0;
    uint32_t lin_acc   = lin_flags & 3u;
    if (lin_acc == NX_LINUX_O_RDONLY)
        nx_flags |= NX_VFS_OPEN_READ;
    else if (lin_acc == NX_LINUX_O_WRONLY)
        nx_flags |= NX_VFS_OPEN_WRITE;
    else if (lin_acc == NX_LINUX_O_RDWR)
        nx_flags |= NX_VFS_OPEN_READ | NX_VFS_OPEN_WRITE;
    if (lin_flags & NX_LINUX_O_CREAT) nx_flags |= NX_VFS_OPEN_CREATE;
    /* O_DIRECTORY is informational — sys_open's "/" branch
     * already returns HANDLE_DIR.  Other O_* bits (O_CLOEXEC,
     * O_NONBLOCK, O_TRUNC, ...) are quietly dropped. */
    return sys_open(a1, (uint64_t)nx_flags, 0, 0, 0, 0);
}

/*
 * NX_SYS_DUP3 — (int oldfd, int newfd, int flags) → newfd | -errno.
 *
 * Slice 7.6d.N.6 minimum.  ash uses dup3 to redirect stdin/stdout to
 * pipe ends before exec'ing pipeline stages: e.g. for `echo | cat`,
 * stage 1 does `dup3(pipe_write, STDOUT_FILENO, 0)` so subsequent
 * `write(1, ...)` lands in the pipe.
 *
 * Encoding subtlety: our handles encode `(generation << 8) | (idx + 1)`.
 * ash treats newfd as an opaque integer that must keep working as the
 * caller's literal handle value after dup3 returns.  We honour that by
 * decoding newfd's embedded generation and forcing the slot's gen to
 * match — so the encoded handle for the slot equals newfd exactly,
 * regardless of how many close/alloc cycles the slot lived through
 * before.  This breaks the usual "stale-handle protection bumps gen"
 * invariant for THIS slot, but the caller is explicitly replacing —
 * any pre-dup3 handle value to the same slot was about to be invalid
 * anyway.
 */
static nx_status_t sys_dup3(uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    nx_handle_t oldfd = (nx_handle_t)a0;
    nx_handle_t newfd = (nx_handle_t)a1;
    int flags = (int)a2;
    (void)flags;  /* O_CLOEXEC etc. ignored in v1 */

    /* POSIX: dup3 with oldfd == newfd returns -EINVAL (unlike dup2 which
     * returns newfd unchanged).  ash uses dup2 (= dup3 with flags=0) and
     * doesn't hit this case in normal pipeline setup, but match the
     * Linux semantic anyway. */
    if (oldfd == newfd) {
        /* dup2 (flags=0) returns newfd; dup3 returns -EINVAL.  We can't
         * tell here which was the libc call (musl unconditionally
         * issues SYS_dup3) — pick the dup2 semantic since that's the
         * common path for ash and the failure mode is more friendly. */
        return (nx_status_t)newfd;
    }

    struct nx_handle_table *t = nx_syscall_current_table();
    if (!t) return NX_EINVAL;

    /* 1. Look up oldfd. */
    enum nx_handle_type src_type;
    uint32_t            src_rights;
    void               *src_object = 0;
    int rc = nx_handle_lookup(t, oldfd, &src_type, &src_rights, &src_object);
    if (rc != NX_OK) return rc;

    /* 2. Decode newfd's slot index + generation.  Layout matches
     * framework/handle.c's encode_handle: low 8 bits = idx + 1, high
     * 24 = generation.  Special case: newfd == 0 (POSIX STDIN_FILENO)
     * has no encoded form in our scheme — encoded value 0 is reserved
     * for NX_HANDLE_INVALID.  Slice 7.6d.N.6b reserves slot 2 for the
     * pre-installed CONSOLE STDIN handle, and the matching h==0
     * special case in sys_read / sys_handle_close routes to that
     * slot.  ash uses dup2(_, 0) to redirect stdin to a pipe read
     * end; install at slot 2 with gen=0 so subsequent read(0, ...)
     * picks up the redirected handle.  We lie about the returned
     * encoded value so it equals what ash passed in. */
    size_t   new_idx;
    uint32_t new_gen;
    if (newfd == 0) {
        new_idx = 2;
        new_gen = 0;
    } else {
        uint32_t enc = newfd & 0xffu;
        if (enc == 0 || enc > NX_HANDLE_TABLE_CAPACITY) return NX_EINVAL;
        new_idx = (size_t)(enc - 1);
        new_gen = newfd >> 8;
    }

    /* 3. If newfd's slot has an entry, close its object.  Don't bump
     * the slot's generation — we're about to overwrite it with a
     * specific generation embedded in newfd. */
    struct nx_handle_entry *e = &t->entries[new_idx];
    if (e->type != NX_HANDLE_INVALID) {
        if (e->type == NX_HANDLE_CHANNEL) {
            nx_channel_endpoint_close(e->object);
        } else if (e->type == NX_HANDLE_FILE) {
            const struct nx_vfs_ops *vops; void *vself;
            if (resolve_vfs(&vops, &vself) == NX_OK)
                vops->close(vself, e->object);
        } else if (e->type == NX_HANDLE_DIR) {
#if !__STDC_HOSTED__
            free(e->object);
#endif
        }
        /* count stays the same — we're replacing one entry with one. */
    } else {
        t->count++;
    }

    /* 4. For channel handles, retain the source endpoint — the new
     * slot now holds an additional reference.  Same for file handles
     * (slice 7.6d.N.8): the per-open struct now lives in two slots
     * and must survive the first close. */
    if (src_type == NX_HANDLE_CHANNEL) {
        nx_channel_endpoint_retain(src_object);
    } else if (src_type == NX_HANDLE_FILE) {
        const struct nx_vfs_ops *vops; void *vself;
        if (resolve_vfs(&vops, &vself) == NX_OK && vops->retain)
            vops->retain(vself, src_object);
    }

    /* 5. Install the source's (type, rights, object) at the destination
     * slot, forcing generation to match newfd's encoded gen.  After
     * this, the encoded handle for `new_idx` equals newfd exactly. */
    e->type       = src_type;
    e->rights     = src_rights;
    e->object     = src_object;
    e->generation = new_gen;

    return (nx_status_t)newfd;
}

/*
 * NX_SYS_FCNTL — (int fd, int cmd, long arg) → cmd-specific / -errno.
 *
 * Slice 7.6d.N.8.  Minimal set covering the calls ash makes during
 * builtin redirection setup:
 *
 *   F_DUPFD (0)            — find the lowest free slot whose encoded
 *   F_DUPFD_CLOEXEC (1030)   handle is ≥ arg, install a copy of fd's
 *                            (type, rights, object), retain the
 *                            underlying object (CHANNEL endpoint
 *                            refcount, FILE per-open refcount), and
 *                            return the new encoded handle.  CLOEXEC
 *                            is moot in v1 — handles aren't inherited
 *                            across exec by default — so both commands
 *                            collapse to the same body.
 *   F_GETFD/F_SETFD/        — return 0; we don't track per-handle FD
 *   F_GETFL/F_SETFL           or open-file flags.  Lets ash think the
 *                            cloexec bit is set without us having to
 *                            actually plumb it.
 *   anything else           — -ENOSYS.
 *
 * Encoded-handle reminder: `(generation << 8) | (idx + 1)`.  For a
 * fresh slot we use generation 0, so the encoded value is `idx + 1`.
 * F_DUPFD's `arg` is the minimum POSIX fd; with gen 0 that maps to
 * `idx ≥ arg - 1` (clamped to 0 for arg ≤ 0).
 */
#define NX_LINUX_F_DUPFD          0
#define NX_LINUX_F_GETFD          1
#define NX_LINUX_F_SETFD          2
#define NX_LINUX_F_GETFL          3
#define NX_LINUX_F_SETFL          4
#define NX_LINUX_F_DUPFD_CLOEXEC  1030

#define NX_LINUX_EBADF   (-9)
#define NX_LINUX_EINVAL  (-22)
#define NX_LINUX_EMFILE  (-24)

static nx_status_t sys_fcntl(uint64_t a0, uint64_t a1, uint64_t a2,
                             uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    nx_handle_t fd  = (nx_handle_t)a0;
    int         cmd = (int)a1;
    long        arg = (long)a2;

    switch (cmd) {
    case NX_LINUX_F_GETFD:
    case NX_LINUX_F_SETFD:
    case NX_LINUX_F_GETFL:
    case NX_LINUX_F_SETFL:
        /* No per-handle FD/OFD flags in v1.  Stub returning 0 is the
         * "always-success, no-op" answer ash treats as "the bit you
         * asked about is clear / was set".  Doesn't break anything as
         * long as nothing actually relies on FD_CLOEXEC being honoured
         * across exec — which it isn't in v1, since exec rebuilds the
         * handle table from scratch in sys_exec. */
        return 0;

    case NX_LINUX_F_DUPFD:
    case NX_LINUX_F_DUPFD_CLOEXEC:
        break;

    default:
        return -38;  /* ENOSYS */
    }

    /* F_DUPFD / F_DUPFD_CLOEXEC body. */
    struct nx_handle_table *t = nx_syscall_current_table();
    if (!t) return NX_EINVAL;

    enum nx_handle_type src_type;
    uint32_t            src_rights;
    void               *src_object = 0;
    int rc = nx_handle_lookup(t, fd, &src_type, &src_rights, &src_object);
    if (rc != NX_OK) return NX_LINUX_EBADF;

    /* `arg` is the minimum POSIX fd.  Convert to a min table index:
     * encoded fd N at generation 0 = idx (N - 1), so idx ≥ arg - 1. */
    size_t min_idx = (arg <= 0) ? 0 : (size_t)(arg - 1);
    if (min_idx >= NX_HANDLE_TABLE_CAPACITY) return NX_LINUX_EINVAL;

    /* Find the first free slot at or above min_idx.  Skip slot 2 if
     * arg ≤ 0 — encoded fd 0 collides with NX_HANDLE_INVALID. */
    for (size_t i = min_idx; i < NX_HANDLE_TABLE_CAPACITY; i++) {
        struct nx_handle_entry *e = &t->entries[i];
        if (e->type != NX_HANDLE_INVALID) continue;

        /* Retain the underlying object before installing the copy. */
        if (src_type == NX_HANDLE_CHANNEL) {
            nx_channel_endpoint_retain(src_object);
        } else if (src_type == NX_HANDLE_FILE) {
            const struct nx_vfs_ops *vops; void *vself;
            if (resolve_vfs(&vops, &vself) == NX_OK && vops->retain)
                vops->retain(vself, src_object);
        }

        e->type       = src_type;
        e->rights     = src_rights;
        e->object     = src_object;
        e->generation = 0;
        t->count++;

        /* Encoded handle: (gen << 8) | (idx + 1) with gen = 0. */
        return (nx_status_t)(i + 1);
    }
    return NX_LINUX_EMFILE;
}

/* ---------- Dispatch table ------------------------------------------- */

static const syscall_fn g_syscall_table[NX_SYSCALL_COUNT] = {
    [NX_SYS_RESERVED_0]     = NULL,             /* caught below → NX_ENOSYS */
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
    [NX_SYS_FORK]           = sys_fork,
    [NX_SYS_WAIT]           = sys_wait,
    [NX_SYS_EXEC]           = sys_exec,
    [NX_SYS_PIPE]           = sys_pipe,
    [NX_SYS_SIGNAL]         = sys_signal,
    [NX_SYS_BRK]            = sys_brk,
    [NX_SYS_WRITEV]         = sys_writev,
    [NX_SYS_MMAP]           = sys_mmap,
    [NX_SYS_MUNMAP]         = sys_munmap,
    [NX_SYS_FSTATAT]        = sys_fstatat,
    [NX_SYS_GETDENTS64]     = sys_getdents64,
    [NX_SYS_OPENAT]         = sys_openat,
    [NX_SYS_DUP3]           = sys_dup3,
    [NX_SYS_READV]          = sys_readv,
    [NX_SYS_FCNTL]          = sys_fcntl,
};

/* ---------- Entry point ---------------------------------------------- */

/*
 * Slice 7.4: per-call stash of the active trap frame.  Most syscalls
 * just consume their 6 argument registers and never need the frame;
 * `sys_fork` is the exception — it builds a child task whose first
 * EL0 resume replays the parent's frame, and needs to byte-copy it.
 *
 * Single-CPU v1 so a plain static suffices.  Multi-CPU wraps this
 * behind `nx_task_current()`-keyed per-CPU state; the public
 * accessor's signature is stable.
 */
static struct trap_frame *g_current_tf;

struct trap_frame *nx_syscall_current_tf(void)
{
    return g_current_tf;
}

void nx_syscall_dispatch(struct trap_frame *tf)
{
    if (!tf) return;

    uint64_t num = tf->x[8];
    nx_status_t rc;

    g_current_tf = tf;
    if (num >= NX_SYSCALL_COUNT || g_syscall_table[num] == NULL) {
        rc = NX_ENOSYS;
    } else {
        rc = g_syscall_table[num](tf->x[0], tf->x[1], tf->x[2],
                                  tf->x[3], tf->x[4], tf->x[5]);
    }
    g_current_tf = NULL;
    tf->x[0] = (uint64_t)rc;
}
