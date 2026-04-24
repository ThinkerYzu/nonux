#ifndef NX_POSIX_SHIM_POSIX_H
#define NX_POSIX_SHIM_POSIX_H

/*
 * posix_shim — POSIX-style C wrappers over the NX_SYS_* syscall set
 * (slice 7.4d).
 *
 * Header-only: every entry is a `static inline` function that emits
 * `svc #0` with the NX_SYS_* number in x8 and arguments in x0..x5,
 * matching the calling convention in framework/syscall.h.  No .c
 * file, no link dependency — an EL0 C program just `#include`s this
 * header and gets the whole surface.  That keeps posix_shim entirely
 * off kernel.bin's link line (it's a userspace library, not a
 * kernel component that binds to a slot).
 *
 * Mapping to POSIX semantics is best-effort for v1:
 *   - No errno — every wrapper returns the raw nx_status_t.  Callers
 *     test for negative values (NX_E*).  A future slice can add an
 *     `errno` thread-local and translate.
 *   - No argv / envp for execve — v1 NX_SYS_EXEC takes only a path.
 *     The signature accepts argv/envp for source-compatibility with
 *     POSIX but ignores them.
 *   - No WIFEXITED / WEXITSTATUS macros — wait writes the raw
 *     exit_code into *status.  Consumers treat status as the exit
 *     code directly.
 *   - No signal delivery — slice 7.5 adds signal handling; today
 *     `posix_signal` is absent.
 *
 * The wrappers are intentionally trivial; the kernel-side sanity
 * checks (path length, handle validity, etc.) are what enforce
 * correctness.  This header is structured so an AArch64 C compiler
 * with `-ffreestanding -nostdlib -mgeneral-regs-only` can emit
 * it as position-dependent EL0 code.
 */

#include <stddef.h>
#include <stdint.h>

/* POSIX-compatible type aliases.  Intentionally int for source-
 * compatibility with stock POSIX headers — our kernel's pid is
 * uint32_t, but a signed int round-trips through the SVC return
 * register just fine for every pid value we generate (< 2^31). */
typedef int       nx_posix_pid_t;
typedef int       nx_posix_fd_t;
typedef int64_t   nx_posix_ssize_t;

/* Syscall numbers — must track framework/syscall.h's
 * `enum nx_syscall_number`.  Kept as named constants here so this
 * header has no framework includes (EL0 code doesn't get the
 * kernel-private header). */
#define NX_POSIX_SYS_DEBUG_WRITE    1
#define NX_POSIX_SYS_HANDLE_CLOSE   2
#define NX_POSIX_SYS_OPEN           6
#define NX_POSIX_SYS_READ           7
#define NX_POSIX_SYS_WRITE          8
#define NX_POSIX_SYS_SEEK           9
#define NX_POSIX_SYS_READDIR       10
#define NX_POSIX_SYS_EXIT          11
#define NX_POSIX_SYS_FORK          12
#define NX_POSIX_SYS_WAIT          13
#define NX_POSIX_SYS_EXEC          14
#define NX_POSIX_SYS_PIPE          15
#define NX_POSIX_SYS_SIGNAL        16

/* Signal numbers — subset supported by v1's NX_SYS_SIGNAL.  Values
 * match POSIX.  Real signal handlers land with a later slice; today
 * both of these turn into `nx_process_exit(128 + signo)` at the
 * target's next `sched_check_resched`. */
#define NX_POSIX_SIGKILL   9
#define NX_POSIX_SIGTERM   15

/* Open flags — subset of NX_VFS_OPEN_* bits.  Values match
 * interfaces/fs.h exactly so vfs_simple forwards them verbatim. */
#define NX_POSIX_O_READ    0x1u
#define NX_POSIX_O_WRITE   0x2u
#define NX_POSIX_O_CREATE  0x4u
#define NX_POSIX_O_RDONLY  NX_POSIX_O_READ
#define NX_POSIX_O_WRONLY  NX_POSIX_O_WRITE
#define NX_POSIX_O_RDWR    (NX_POSIX_O_READ | NX_POSIX_O_WRITE)

/* Seek whence values — match interfaces/fs.h. */
#define NX_POSIX_SEEK_SET  0
#define NX_POSIX_SEEK_CUR  1
#define NX_POSIX_SEEK_END  2

/* ---------- Low-level SVC dispatchers ------------------------------- */
/*
 * Each svcN() takes N argument registers + a syscall number and
 * returns the kernel's x0.  The `volatile` keeps the compiler from
 * reordering the SVC across surrounding memory traffic; the
 * "memory" clobber forces a reload of any caller-cached memory on
 * return (channels + file I/O touch user memory via the dispatcher,
 * so this is necessary for correctness).
 */
static inline int64_t nx_posix_svc0(uint64_t nr)
{
    register uint64_t x8 asm("x8") = nr;
    register int64_t  x0 asm("x0");
    asm volatile ("svc #0"
                  : "=r"(x0)
                  : "r"(x8)
                  : "memory", "cc");
    return x0;
}

static inline int64_t nx_posix_svc1(uint64_t nr, uint64_t a0)
{
    register uint64_t x8 asm("x8") = nr;
    register int64_t  x0 asm("x0") = (int64_t)a0;
    asm volatile ("svc #0"
                  : "+r"(x0)
                  : "r"(x8)
                  : "memory", "cc");
    return x0;
}

static inline int64_t nx_posix_svc2(uint64_t nr, uint64_t a0, uint64_t a1)
{
    register uint64_t x8 asm("x8") = nr;
    register int64_t  x0 asm("x0") = (int64_t)a0;
    register uint64_t x1 asm("x1") = a1;
    asm volatile ("svc #0"
                  : "+r"(x0)
                  : "r"(x8), "r"(x1)
                  : "memory", "cc");
    return x0;
}

static inline int64_t nx_posix_svc3(uint64_t nr, uint64_t a0,
                                    uint64_t a1, uint64_t a2)
{
    register uint64_t x8 asm("x8") = nr;
    register int64_t  x0 asm("x0") = (int64_t)a0;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x2 asm("x2") = a2;
    asm volatile ("svc #0"
                  : "+r"(x0)
                  : "r"(x8), "r"(x1), "r"(x2)
                  : "memory", "cc");
    return x0;
}

/* ---------- POSIX-style wrappers ------------------------------------ */

/* write(STDOUT_FILENO, buf, len) equivalent against the kernel's
 * debug UART.  No fd concept for this call in v1 — a future slice
 * can wire STDOUT/STDERR to a console handle. */
static inline nx_posix_ssize_t
nx_posix_debug_write(const char *buf, size_t len)
{
    return nx_posix_svc2(NX_POSIX_SYS_DEBUG_WRITE,
                         (uint64_t)(uintptr_t)buf, (uint64_t)len);
}

/* exit(status) — noreturn.  The kernel parks the current task in
 * wfe after marking the process EXITED, so this function never
 * returns to the caller.  `__builtin_unreachable` tells the
 * compiler it can drop any code after the call site. */
static inline void __attribute__((noreturn))
nx_posix_exit(int status)
{
    (void)nx_posix_svc1(NX_POSIX_SYS_EXIT, (uint64_t)status);
    __builtin_unreachable();
}

/* fork() — returns child pid in parent, 0 in child, negative on
 * error.  Child resumes at the instruction after the SVC with x0
 * clobbered to 0 (via the slice-7.4a trap-frame replay path). */
static inline nx_posix_pid_t nx_posix_fork(void)
{
    return (nx_posix_pid_t)nx_posix_svc0(NX_POSIX_SYS_FORK);
}

/* execve(path, argv, envp) — replaces the current process image
 * with the ELF at `path`.  argv/envp are reserved (v1 exec takes
 * only a path).  On success the call never returns — control
 * transfers to the new program's entry.  On failure returns
 * negative NX_E*. */
static inline int
nx_posix_execve(const char *path, char *const argv[], char *const envp[])
{
    (void)argv; (void)envp;
    return (int)nx_posix_svc1(NX_POSIX_SYS_EXEC,
                              (uint64_t)(uintptr_t)path);
}

/* waitpid(pid, &status, 0) — blocks until the target process
 * exits, writes its exit_code to *status (if status is non-NULL),
 * and returns the target's pid.  Options are reserved (v1 has
 * no WNOHANG / WUNTRACED). */
static inline nx_posix_pid_t
nx_posix_waitpid(nx_posix_pid_t pid, int *status, int options)
{
    (void)options;
    return (nx_posix_pid_t)nx_posix_svc2(NX_POSIX_SYS_WAIT,
                                         (uint64_t)pid,
                                         (uint64_t)(uintptr_t)status);
}

/* open(path, flags, mode) — mode is ignored in v1 (no per-file
 * permissions yet).  Returns a handle-shaped fd (positive) on
 * success, negative NX_E* on failure. */
static inline nx_posix_fd_t
nx_posix_open(const char *path, int flags, int mode)
{
    (void)mode;
    return (nx_posix_fd_t)nx_posix_svc2(NX_POSIX_SYS_OPEN,
                                        (uint64_t)(uintptr_t)path,
                                        (uint64_t)(unsigned int)flags);
}

/* close(fd) — releases the underlying handle. */
static inline int nx_posix_close(nx_posix_fd_t fd)
{
    return (int)nx_posix_svc1(NX_POSIX_SYS_HANDLE_CLOSE, (uint64_t)fd);
}

static inline nx_posix_ssize_t
nx_posix_read(nx_posix_fd_t fd, void *buf, size_t len)
{
    return nx_posix_svc3(NX_POSIX_SYS_READ,
                         (uint64_t)fd,
                         (uint64_t)(uintptr_t)buf,
                         (uint64_t)len);
}

static inline nx_posix_ssize_t
nx_posix_write(nx_posix_fd_t fd, const void *buf, size_t len)
{
    return nx_posix_svc3(NX_POSIX_SYS_WRITE,
                         (uint64_t)fd,
                         (uint64_t)(uintptr_t)buf,
                         (uint64_t)len);
}

static inline int64_t
nx_posix_lseek(nx_posix_fd_t fd, int64_t offset, int whence)
{
    return nx_posix_svc3(NX_POSIX_SYS_SEEK,
                         (uint64_t)fd,
                         (uint64_t)offset,
                         (uint64_t)(unsigned int)whence);
}

/* pipe(fds) — writes two fds (read side at [0], write side at [1])
 * via copy_to_user.  Returns 0 on success, negative on failure. */
static inline int nx_posix_pipe(int fds[2])
{
    return (int)nx_posix_svc1(NX_POSIX_SYS_PIPE,
                              (uint64_t)(uintptr_t)fds);
}

/* kill(pid, signo) — delivers a signal to a target process.  v1
 * supports `NX_POSIX_SIGTERM` and `NX_POSIX_SIGKILL`; both cause
 * the target to exit with `128 + signo` at its next scheduler
 * reschedule point.  Returns 0 on success, negative on failure. */
static inline int nx_posix_kill(nx_posix_pid_t pid, int signo)
{
    return (int)nx_posix_svc2(NX_POSIX_SYS_SIGNAL,
                              (uint64_t)pid,
                              (uint64_t)(unsigned int)signo);
}

#endif /* NX_POSIX_SHIM_POSIX_H */
