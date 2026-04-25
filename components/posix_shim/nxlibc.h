#ifndef NX_POSIX_SHIM_NXLIBC_H
#define NX_POSIX_SHIM_NXLIBC_H

/*
 * nxlibc — POSIX-named C surface (slice 7.6c.1).
 *
 * Companion header to `posix.h`.  Exposes the same syscall and libc
 * helper surface as posix.h, but under the names a real libc / a
 * busybox-style program expects (`write`, `read`, `_exit`, `fork`,
 * `execve`, `waitpid`, `kill`, `pipe`, `open`, `close`, `lseek`,
 * `strlen`, `memcpy`, ...).  Internally, every nxlibc function is a
 * thin call into the matching `nx_posix_*` / `nx_*` inline from
 * `posix.h`.
 *
 * Why a separate header + a real `.c` file (not just static
 * inlines)?  Because slice 7.6c.1's deliverable is a static archive
 * `libnxlibc.a` that EL0 C programs can link against with
 * `-lnxlibc` — the same shape musl will plug into when slice 7.6c.2
 * vendors it.  Static-inline-only doesn't produce object files, so
 * an archive can't carry the symbols.
 *
 * Magic file descriptors:
 *   fd 0 (STDIN)  — no input source yet; `read` returns NX_EINVAL.
 *   fd 1 (STDOUT) — `write` routes to `NX_SYS_DEBUG_WRITE` (UART).
 *   fd 2 (STDERR) — `write` routes to `NX_SYS_DEBUG_WRITE` too —
 *                   we don't have separate stderr in v1.
 *   fd > 2        — a real `nx_handle_t`; `write` / `read` dispatch
 *                   through `NX_SYS_WRITE` / `_READ` to vfs_simple
 *                   or to a CHANNEL endpoint via slice 7.5's type-
 *                   polymorphic handlers.
 *
 * The split between `nx_posix_*` (`posix.h`, inline) and the
 * unprefixed POSIX names (here, in `libnxlibc.a`) lets a program
 * choose its own discipline:
 *
 *   - `posix.h` only — explicitly nonux-namespaced; no risk of
 *     name collision with an externally-linked libc; useful when
 *     debugging the syscall layer.
 *   - `nxlibc.h` + `-lnxlibc` — POSIX-named; ports busybox-style
 *     code in directly.  When slice 7.6c.2 swaps in musl, the
 *     header + library names stay; only the implementation
 *     underneath changes.
 */

#include <stddef.h>
#include <stdint.h>

/* POSIX-compatible types — these are aliased one layer up so we
 * don't pull `posix.h` into nxlibc's public surface. */
typedef int       nxlibc_pid_t;
typedef int       nxlibc_fd_t;
typedef int64_t   nxlibc_ssize_t;

/* Magic file descriptors.  Values match POSIX's STDIN_FILENO etc. */
#define NXLIBC_STDIN_FILENO    0
#define NXLIBC_STDOUT_FILENO   1
#define NXLIBC_STDERR_FILENO   2

/* Open flags + seek whence — bit-compatible with `interfaces/fs.h`. */
#define NXLIBC_O_RDONLY  0x1
#define NXLIBC_O_WRONLY  0x2
#define NXLIBC_O_RDWR    0x3
#define NXLIBC_O_CREAT   0x4

#define NXLIBC_SEEK_SET  0
#define NXLIBC_SEEK_CUR  1
#define NXLIBC_SEEK_END  2

#define NXLIBC_SIGKILL   9
#define NXLIBC_SIGTERM   15

/* ---------- Process lifecycle ---------------------------------------- */

void __attribute__((noreturn))
nxlibc_exit(int status);

nxlibc_pid_t nxlibc_fork(void);
int          nxlibc_execve(const char *path, char *const argv[],
                           char *const envp[]);
nxlibc_pid_t nxlibc_waitpid(nxlibc_pid_t pid, int *status, int options);
int          nxlibc_kill(nxlibc_pid_t pid, int signo);

/* ---------- File I/O -------------------------------------------------- */

nxlibc_fd_t      nxlibc_open(const char *path, int flags, int mode);
int              nxlibc_close(nxlibc_fd_t fd);
nxlibc_ssize_t   nxlibc_read(nxlibc_fd_t fd, void *buf, size_t count);
nxlibc_ssize_t   nxlibc_write(nxlibc_fd_t fd, const void *buf, size_t count);
int64_t          nxlibc_lseek(nxlibc_fd_t fd, int64_t offset, int whence);

/* ---------- IPC ------------------------------------------------------- */

int nxlibc_pipe(int fds[2]);

/* ---------- Tiny libc helpers ---------------------------------------- *
 *
 * These mirror the static-inline helpers in `posix.h` but as real
 * library symbols — busybox + most C code expects them as named
 * functions, not as compiler-inlined templates.
 */

size_t nxlibc_strlen(const char *s);
int    nxlibc_strcmp(const char *a, const char *b);
void  *nxlibc_memcpy(void *dst, const void *src, size_t n);
void  *nxlibc_memset(void *dst, int c, size_t n);

/* ASCII int parser — POSIX `atoi`.  Trims leading whitespace, accepts
 * an optional `+` / `-` sign, then reads decimal digits until the
 * first non-digit.  No errno; out-of-range silently overflows.  Good
 * enough for argv parsing where the caller has already validated the
 * input shape. */
int nxlibc_atoi(const char *s);

/* ---------- stdio (slice 7.6c.2) ------------------------------------- *
 *
 * Tiny printf-style formatter targeting STDOUT_FILENO.  Supported
 * conversions:
 *
 *   %d  / %i   signed decimal
 *   %u         unsigned decimal
 *   %x         lowercase hex (no 0x prefix)
 *   %X         uppercase hex (no 0x prefix)
 *   %s         NUL-terminated string ("(null)" if NULL)
 *   %c         single character
 *   %%         literal '%'
 *
 * No width, precision, padding, or floating-point.  The full
 * `printf(3)` surface lands when slice 7.6c.3 vendors musl; today's
 * subset is enough for hand-written EL0 demos and for slice 7.6d's
 * pre-busybox smoke tests.
 *
 * `nxlibc_printf` returns the number of bytes written, or a negative
 * `nx_status_t` if the underlying write failed (e.g. UART
 * disconnected — not a thing in v1, but the contract reserves the
 * negative return for forward compat).
 */

#include <stdarg.h>

int nxlibc_putchar(int c);
int nxlibc_puts(const char *s);                /* writes s + '\n' */
int nxlibc_vsnprintf(char *buf, size_t cap,
                     const char *fmt, va_list ap);
int nxlibc_snprintf(char *buf, size_t cap, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
int nxlibc_printf(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

#endif /* NX_POSIX_SHIM_NXLIBC_H */
