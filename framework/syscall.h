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
 * Unknown syscall numbers and invalid argument pointers return
 * NX_EINVAL; handle operations pass through to the current owner's
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
    NX_SYS_EXEC           = 14,  /* (const char *path) → noreturn on
                                  *   success; NX_E* on failure.
                                  *   Loads an ELF from the vfs path
                                  *   into a fresh address space,
                                  *   swaps it in, erets to the ELF's
                                  *   entry with zeroed registers. */
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
