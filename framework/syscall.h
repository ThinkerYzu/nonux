#ifndef NX_FRAMEWORK_SYSCALL_H
#define NX_FRAMEWORK_SYSCALL_H

#include "framework/handle.h"
#include "framework/registry.h"

#include <stdint.h>

/*
 * Syscall framework — Phase 5 slice 5.4.
 *
 * v1 wires up the SVC-from-EL1 path only — tests issue `svc #0` from
 * kernel code to exercise the dispatch plumbing before slice 5.5 drops
 * to EL0.  Calling convention follows the Linux AArch64 ABI so later
 * EL0 consumers need no adapter:
 *
 *   x8      syscall number (enum nx_syscall_number)
 *   x0..x5  arguments
 *   x0      return value (sign-extended nx_status_t)
 *
 * Unknown syscall numbers return NX_ENOSYS (Linux-compat -38) so musl
 * surfaces them as `errno = ENOSYS` instead of EPERM; invalid argument
 * pointers (and other in-handler errors) return NX_EINVAL or other
 * NX_E* codes.  Handle operations pass through to the current owner's
 * handle table (for slice 5.4 that's a single process-agnostic global
 * table; slice 5.5 moves it per-task).
 */

/* Signed 64-bit status so negative NX_E* codes survive the x0 trip
 * through register storage without ambiguity.  Non-negative returns are
 * op-defined (bytes written, handle value, etc). */
typedef int64_t nx_status_t;

/* Syscall numbers.  Gaps are reserved for later growth without
 * renumbering already-shipped calls — this enum is effectively a stable
 * ABI surface from slice 5.5 onward. */
enum nx_syscall_number {
    NX_SYS_RESERVED_0     = 0,   /* reserved: SVC #0 with x8 == 0 should
                                  * be a clear "syscall number not set"
                                  * error rather than some real op. */
    NX_SYS_DEBUG_WRITE    = 1,   /* (const char *buf, size_t len) → bytes written */
    NX_SYS_HANDLE_CLOSE   = 2,   /* (nx_handle_t h)               → NX_OK / NX_E* */
    NX_SYS_CHANNEL_CREATE = 3,   /* (nx_handle_t *e0, nx_handle_t *e1)
                                  *   → NX_OK; writes 2 handles via copy_to_user */
    NX_SYS_CHANNEL_SEND   = 4,   /* (nx_handle_t h, const void *buf, size_t len)
                                  *   → bytes sent / NX_EBUSY (full or closed) */
    NX_SYS_CHANNEL_RECV   = 5,   /* (nx_handle_t h, void *buf, size_t cap)
                                  *   → bytes received / NX_EAGAIN / NX_ENOMEM */
    NX_SYS_OPEN           = 6,   /* (const char *path, uint32_t flags)
                                  *   → HANDLE_FILE handle / NX_E* */
    NX_SYS_READ           = 7,   /* (nx_handle_t h, void *buf, size_t cap)
                                  *   → bytes read / NX_E* */
    NX_SYS_WRITE          = 8,   /* (nx_handle_t h, const void *buf, size_t len)
                                  *   → bytes written / NX_E* */
    NX_SYS_SEEK           = 9,   /* (nx_handle_t h, int64_t offset, int whence)
                                  *   → new absolute position / NX_E* */
    NX_SYS_READDIR        = 10,  /* (uint32_t *cookie, struct nx_fs_dirent *out)
                                  *   → NX_OK / NX_ENOENT / NX_EINVAL */
    NX_SYS_EXIT           = 11,  /* (int code) → noreturn; marks current
                                  *   process EXITED, parks in wfe loop */
    NX_SYS_FORK           = 12,  /* () → pid (parent) / 0 (child) / NX_E*.
                                  *   Duplicates address space, empty
                                  *   child handle table (v1). */
    NX_SYS_WAIT           = 13,  /* (uint32_t pid, int *user_status)
                                  *   → pid / NX_ENOENT / NX_EINVAL.
                                  *   Polls with yield until target is
                                  *   EXITED; writes exit_code to user. */
    NX_SYS_EXEC           = 14,  /* (const char *path,
                                  *    char *const argv[],
                                  *    char *const envp[]) → noreturn
                                  *   on success; NX_E* on failure.
                                  *   Loads an ELF from the vfs path
                                  *   into a fresh address space,
                                  *   swaps it in, builds the System V
                                  *   argv layout on the new user
                                  *   stack (argc + argv ptrs + envp
                                  *   NULL + strings), erets to the
                                  *   ELF's entry with `sp_el0` at the
                                  *   new layout's `argc` slot.  argv
                                  *   == NULL synthesises `{ path,
                                  *   NULL }`.  envp is currently
                                  *   always the empty environment.
                                  *   v1 caps argc at 16 and total
                                  *   argv-string bytes at 1024. */
    NX_SYS_PIPE           = 15,  /* (int fds[2]) → NX_OK; writes two
                                  *   handles (read side at fds[0],
                                  *   write side at fds[1]) via
                                  *   copy_to_user.  Backed by the
                                  *   slice-5.6 channel primitive. */
    NX_SYS_SIGNAL         = 16,  /* (uint32_t pid, int signo)
                                  *   → NX_OK / NX_ENOENT / NX_EINVAL.
                                  *   Sets the matching bit in the
                                  *   target process's pending_signals.
                                  *   v1 supports SIGTERM (15) and
                                  *   SIGKILL (9); both cause the
                                  *   target to exit with status
                                  *   128+signo at its next
                                  *   sched_check_resched.  */
    NX_SYS_BRK            = 17,  /* (uint64_t requested) → new break
                                  *   (uint64_t).  Linux's brk(2)
                                  *   semantics: if `requested` is 0,
                                  *   returns the current break;
                                  *   otherwise tries to set the break
                                  *   to `requested` and returns the
                                  *   new break (== requested on
                                  *   success, == old break if
                                  *   out-of-range).  Heap lives in
                                  *   the process's existing 8 MiB
                                  *   user-window backing —
                                  *   [base + 6 MiB .. base + 7.5 MiB)
                                  *   reserves 1.5 MiB.  See
                                  *   NX_PROCESS_HEAP_{OFFSET,LIMIT}
                                  *   in framework/process.h. */
    NX_SYS_WRITEV         = 18,  /* (nx_handle_t h, struct iovec *iov,
                                  *   int iovcnt) → bytes written /
                                  *   NX_E*.  Walks the iovec array
                                  *   from user memory + dispatches
                                  *   each entry through sys_write
                                  *   (so the magic-fd-handle for
                                  *   fd 1/2 + handle-table lookup
                                  *   for opened fds + CHANNEL/FILE
                                  *   dispatch all work).  Needed by
                                  *   musl's __stdio_write — its
                                  *   buffered printf pipeline emits
                                  *   one writev per fwrite/fflush;
                                  *   without this syscall musl's
                                  *   stdout silently swallows
                                  *   output. */
    NX_SYS_MMAP           = 19,  /* (void *addr, size_t length,
                                  *    int prot, int flags,
                                  *    int fd,   off_t offset)
                                  *   → user VA on success, small
                                  *   negative -errno on failure
                                  *   (musl's __syscall_ret turns
                                  *   either of those into
                                  *   MAP_FAILED + errno).
                                  *   v1 supports the mallocng shape
                                  *   only: addr ignored, fd must be
                                  *   -1, offset 0, flags must
                                  *   include MAP_ANONYMOUS|MAP_PRIVATE,
                                  *   no MAP_FIXED, no per-page
                                  *   protection (the user window is
                                  *   uniformly EL0-RW).  Length is
                                  *   rounded up to a 4 KiB page.
                                  *   The kernel picks the address
                                  *   from a per-process bump arena
                                  *   carved out of the unused window
                                  *   region [+2 MiB, +5 MiB).  See
                                  *   NX_PROCESS_MMAP_{OFFSET,LIMIT}
                                  *   in framework/process.h.  Pages
                                  *   are zeroed on the way out so
                                  *   MAP_ANONYMOUS's zero-init
                                  *   contract holds (the underlying
                                  *   user_window backing is a
                                  *   straight malloc and may carry
                                  *   stale bytes from earlier exec
                                  *   cycles or from un-touched
                                  *   memcpy alignment slack). */
    NX_SYS_MUNMAP         = 20,  /* (void *addr, size_t length)
                                  *   → 0 unconditionally.  v1
                                  *   doesn't reclaim mmap'd pages —
                                  *   PMM reclaim happens at process
                                  *   exit when the whole user_window
                                  *   backing is freed.  Returning a
                                  *   no-op success matches mallocng's
                                  *   expectation that munmap never
                                  *   fails (it treats failure as
                                  *   fatal); we leak the page until
                                  *   process exit. */
    NX_SYS_FSTATAT        = 21,  /* (int dirfd, const char *path,
                                  *    struct nx_user_stat *buf,
                                  *    int flags)
                                  *   → 0 on success (buf populated),
                                  *   -2 (Linux ENOENT) if the path
                                  *   doesn't resolve in vfs, or
                                  *   another small Linux-errno on
                                  *   bad args.  Slice 7.6d.N.4
                                  *   minimum: ash uses this to walk
                                  *   PATH looking for executables.
                                  *   We populate just enough fields
                                  *   for ash's `S_ISREG && X_OK`
                                  *   check: `st_mode = S_IFREG |
                                  *   0755`, `st_size = file size`,
                                  *   everything else 0.  `dirfd` is
                                  *   ignored (treated as AT_FDCWD)
                                  *   because vfs_simple takes
                                  *   absolute paths only.  Slice
                                  *   7.6d.N.5 special-cases path "/"
                                  *   to return `S_IFDIR | 0755` so
                                  *   `ls` (and busybox's opendir)
                                  *   can recognise the root as a
                                  *   directory. */
    NX_SYS_GETDENTS64     = 22,  /* (int fd, struct linux_dirent64 *buf,
                                  *    size_t count)
                                  *   → bytes written on success
                                  *   (0 at EOF), or small Linux
                                  *   errno on failure.  Slice
                                  *   7.6d.N.5: `fd` must reference
                                  *   a HANDLE_DIR (allocated by
                                  *   sys_open when called with
                                  *   path "/").  Records emitted
                                  *   in Linux ABI shape (variable-
                                  *   length, 8-byte aligned).  v1:
                                  *   every entry is reported as
                                  *   DT_REG; ls's plain mode
                                  *   doesn't care about d_type. */
    NX_SYS_OPENAT         = 23,  /* (int dirfd, const char *path,
                                  *    int flags, mode_t mode)
                                  *   Linux-shape wrapper over
                                  *   sys_open.  `dirfd` ignored
                                  *   (vfs_simple takes absolute
                                  *   paths only), `mode` ignored
                                  *   (no permission bits in v1).
                                  *   `flags` interpreted as the
                                  *   Linux O_* bit-field — at
                                  *   least O_RDONLY (0) /
                                  *   O_WRONLY (1) / O_RDWR (2) /
                                  *   O_CREAT (0o100) /
                                  *   O_DIRECTORY (0o200000).
                                  *   musl's `open()` becomes
                                  *   `openat(AT_FDCWD, ...)` on
                                  *   aarch64 so this entry is
                                  *   what every libc-driven open
                                  *   reaches.  Slice 7.6d.N.5. */
    NX_SYS_DUP3           = 24,  /* (int oldfd, int newfd, int flags)
                                  *   → newfd on success (the encoded
                                  *   handle value), or small Linux-
                                  *   errno on failure.  Slice
                                  *   7.6d.N.6: ash uses dup3 to
                                  *   redirect stdin/stdout to pipe
                                  *   ends before exec'ing pipeline
                                  *   stages.  `flags` ignored
                                  *   (O_CLOEXEC isn't a thing in v1
                                  *   — handles aren't inherited
                                  *   across exec by default since
                                  *   exec replaces the whole table).
                                  *   Replaces newfd's slot atomically:
                                  *   if it had an entry, that entry's
                                  *   object is closed (channel endpoint
                                  *   decref, file vfs close, dir
                                  *   cursor free).  Then a copy of
                                  *   oldfd's (type, rights, object) is
                                  *   installed at newfd's slot, with
                                  *   the slot's generation explicitly
                                  *   set to match newfd's encoded gen
                                  *   so the encoded handle returned
                                  *   exactly equals newfd (callers
                                  *   like ash assume the literal value
                                  *   they passed in keeps working).
                                  *   For channel handles, retains the
                                  *   endpoint refcount so close-on-
                                  *   exec / close-on-pipeline-cleanup
                                  *   stays balanced. */
    NX_SYS_READV          = 25,  /* (int fd, const struct iovec *iov,
                                  *    int iovcnt)
                                  *   → total bytes read / -errno.
                                  *   Slice 7.6d.N.6: musl's
                                  *   `__stdio_read` flushes buffered
                                  *   input via SYS_readv (cat's
                                  *   read loop hits this).  Mirror
                                  *   of NX_SYS_WRITEV: walk iovec,
                                  *   dispatch each entry through
                                  *   sys_read so the magic-fd /
                                  *   handle-table / CHANNEL / FILE
                                  *   branches apply.  Cap iovcnt at
                                  *   16; stop on first short read
                                  *   per Linux readv semantic. */
    NX_SYS_FCNTL          = 26,  /* (int fd, int cmd, long arg)
                                  *   → cmd-specific value / -errno.
                                  *   Slice 7.6d.N.8: ash uses
                                  *   `fcntl(fd, F_DUPFD_CLOEXEC, 10)`
                                  *   in `save_fd_on_redirect` to stash
                                  *   the original stdout before
                                  *   installing a redirection.  Without
                                  *   this syscall ash dies with
                                  *   xfunc_die() before any redirected
                                  *   output is written.
                                  *   Supported cmds:
                                  *     F_DUPFD (0) / F_DUPFD_CLOEXEC
                                  *       (1030) — find the lowest free
                                  *       slot whose encoded handle ≥
                                  *       arg, install a copy of fd's
                                  *       (type, rights, object), retain
                                  *       the underlying object (CHANNEL
                                  *       endpoint refcount, FILE per-
                                  *       open refcount), and return the
                                  *       new encoded handle.  CLOEXEC
                                  *       is moot in v1 (handles aren't
                                  *       inherited across exec) so both
                                  *       commands behave identically.
                                  *     F_GETFD (1) / F_GETFL (3) —
                                  *       return 0.  F_SETFD (2) /
                                  *       F_SETFL (4) — return 0 and
                                  *       ignore the new flags.  We
                                  *       don't track per-handle FD or
                                  *       O_* flags.
                                  *   Other cmds return -ENOSYS so
                                  *   programs that need real fcntl
                                  *   surface the gap explicitly. */

    NX_SYSCALL_COUNT,            /* sentinel — keep last */
};

/*
 * Signal numbers — POSIX-compatible values.  Both are polite in v1
 * (neither triggers an async interrupt of the target); the
 * sched_check_resched poll turns either into a `nx_process_exit`
 * with the standard POSIX `128 + signo` exit status.  The real
 * distinction (SIGTERM being catchable, SIGKILL not) lands with
 * the slice that introduces signal handlers.
 */
#define NX_SIGKILL   9
#define NX_SIGTERM   15

/*
 * File-syscall limits (slice 6.3).
 *
 * `NX_PATH_MAX` bounds the path string copy_from_user reads on open —
 * paths longer than this return NX_EINVAL.  128 bytes is generous for
 * the single-mount v1 filesystem (ramfs's NAME_MAX is 32); a future
 * slice can raise this without breaking the syscall ABI.
 *
 * `NX_FILE_IO_MAX` is the staging-buffer size read/write use on each
 * syscall.  Consumers passing larger `cap` / `len` get at most this
 * many bytes per call — users loop to transfer more.  Matches the
 * channel syscall pattern (NX_CHANNEL_MSG_MAX = 256) so the two
 * bulk-IO kernel buffers share a size class.
 */
#define NX_PATH_MAX     128u
#define NX_FILE_IO_MAX  256u

/*
 * Dispatch entry — invoked from `on_sync` after it has decoded
 * ESR_EL1.EC as SVC (0x15).  `tf->x[8]` holds the syscall number; args
 * live in `tf->x[0..5]`.  The return value is written into `tf->x[0]`
 * so it lands in x0 when the exception handler does `eret`.
 *
 * Safe to call from EL1 sync-exception context with preemption
 * implicitly disabled (DAIF-I masked by the hardware on exception
 * entry).  The dispatch table itself is `const`, so no locking needed.
 */
struct trap_frame;
void nx_syscall_dispatch(struct trap_frame *tf);

/*
 * Current owner's handle table.  Slice 5.4 returns a single global
 * kernel-wide table — there's no "process" concept yet.  Slice 5.5
 * replaces the body with `&nx_task_current()->handle_table` so each
 * task (later: each process) has its own handle namespace.
 *
 * Exposed in the header so kernel tests can seed + inspect the table
 * before/after issuing SVCs.  Userspace never calls this directly;
 * it's a kernel-internal helper that the syscall bodies consume.
 */
struct nx_handle_table *nx_syscall_current_table(void);

/*
 * Test-only: clear the kernel handle table.  Called by kernel tests
 * between cases so one test's leftover handles don't affect the
 * next.  No-op safe.
 */
void nx_syscall_reset_for_test(void);

/*
 * Test-only: how many times NX_SYS_DEBUG_WRITE has been successfully
 * invoked since kernel boot (or since the last nx_syscall_reset_for_test).
 * Slice 5.5 uses this as the verification signal for "EL0 code reached
 * the SVC" — UART output isn't programmatically observable from the
 * test, but the counter is.  Production code has no reason to call this.
 */
uint64_t nx_syscall_debug_write_calls(void);

#endif /* NX_FRAMEWORK_SYSCALL_H */
