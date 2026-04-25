/*
 * nxlibc.c — implementations for the POSIX-named C surface declared
 * in `nxlibc.h`.  Every function is a thin shim over the matching
 * `nx_posix_*` / `nx_*` inline from `posix.h`; the only "real"
 * logic is in `nxlibc_write`, which routes fds 1 and 2 to the UART
 * via `NX_SYS_DEBUG_WRITE`.
 *
 * Build flags match `POSIX_PROG_CFLAGS` from the top-level Makefile
 * (-ffreestanding -nostdlib -fno-pic, etc.) so the produced .o
 * links cleanly against any EL0 C program built the same way.
 *
 * Slice 7.6c.1.
 */

#include "components/posix_shim/posix.h"
#include "components/posix_shim/nxlibc.h"

void __attribute__((noreturn)) nxlibc_exit(int status)
{
    nx_posix_exit(status);
}

nxlibc_pid_t nxlibc_fork(void)
{
    return (nxlibc_pid_t)nx_posix_fork();
}

int nxlibc_execve(const char *path, char *const argv[], char *const envp[])
{
    return nx_posix_execve(path, argv, envp);
}

nxlibc_pid_t nxlibc_waitpid(nxlibc_pid_t pid, int *status, int options)
{
    return (nxlibc_pid_t)nx_posix_waitpid(pid, status, options);
}

int nxlibc_kill(nxlibc_pid_t pid, int signo)
{
    return nx_posix_kill(pid, signo);
}

nxlibc_fd_t nxlibc_open(const char *path, int flags, int mode)
{
    return (nxlibc_fd_t)nx_posix_open(path, flags, mode);
}

int nxlibc_close(nxlibc_fd_t fd)
{
    /* fds 0..2 are magic stdio handles; close on them is a no-op
     * (they don't correspond to real handle-table entries). */
    if (fd >= 0 && fd <= 2) return 0;
    return nx_posix_close((nx_posix_fd_t)fd);
}

nxlibc_ssize_t nxlibc_read(nxlibc_fd_t fd, void *buf, size_t count)
{
    if (fd == NXLIBC_STDIN_FILENO) {
        /* No console input yet — return 0 (EOF) so a program that
         * reads stdin sees a clean end-of-input.  POSIX `read`
         * returns 0 on EOF; programs that test for that exit
         * cleanly. */
        (void)buf; (void)count;
        return 0;
    }
    return nx_posix_read((nx_posix_fd_t)fd, buf, count);
}

nxlibc_ssize_t nxlibc_write(nxlibc_fd_t fd, const void *buf, size_t count)
{
    if (fd == NXLIBC_STDOUT_FILENO || fd == NXLIBC_STDERR_FILENO) {
        /* Both stdout and stderr go to the kernel debug UART in
         * v1.  Real stderr separation lands when nonux gains a
         * console component with multiple sinks. */
        return nx_posix_debug_write((const char *)buf, count);
    }
    return nx_posix_write((nx_posix_fd_t)fd, buf, count);
}

int64_t nxlibc_lseek(nxlibc_fd_t fd, int64_t offset, int whence)
{
    return nx_posix_lseek((nx_posix_fd_t)fd, offset, whence);
}

int nxlibc_pipe(int fds[2])
{
    return nx_posix_pipe(fds);
}

/* ---------- libc helpers --------------------------------------------- */

size_t nxlibc_strlen(const char *s)        { return nx_strlen(s); }
int    nxlibc_strcmp(const char *a, const char *b)
                                            { return nx_strcmp(a, b); }
void  *nxlibc_memcpy(void *d, const void *s, size_t n)
                                            { return nx_memcpy(d, s, n); }
void  *nxlibc_memset(void *d, int c, size_t n)
                                            { return nx_memset(d, c, n); }

/* ---------- atoi ----------------------------------------------------- *
 *
 * POSIX `atoi` minus the locale lookup: leading whitespace, optional
 * sign, decimal digits.  Does not validate; returns 0 if no digits
 * are seen.  Good enough for argv parsing where the caller has
 * already validated the shape.
 */
int nxlibc_atoi(const char *s)
{
    if (!s) return 0;
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;

    int sign = 1;
    if (*s == '+') s++;
    else if (*s == '-') { sign = -1; s++; }

    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return sign * v;
}

/* ---------- printf (slice 7.6c.2) ------------------------------------ *
 *
 * Self-contained: format into a stack buffer, then `nxlibc_write`
 * it to STDOUT_FILENO.  No allocations, no locks, no global state.
 *
 * Buffer cap is `NXLIBC_PRINTF_BUF` (256 bytes — same size class as
 * `NX_FILE_IO_MAX` / `NX_CHANNEL_MSG_MAX`).  Output longer than the
 * buffer is silently truncated; the return value reports how many
 * bytes WOULD HAVE been written if the buffer were unbounded
 * (matching glibc's `snprintf` convention).  When slice 7.6c.3's
 * musl arrives, this helper retires.
 *
 * Conversion implementation: %d / %u / %x / %X / %s / %c / %%.
 * No width / precision / flags — sufficient for hand-written EL0
 * demos.
 */

#define NXLIBC_PRINTF_BUF  256u

static size_t emit_char(char *buf, size_t cap, size_t pos, char c)
{
    if (pos < cap) buf[pos] = c;
    return pos + 1;
}

static size_t emit_str(char *buf, size_t cap, size_t pos, const char *s)
{
    if (!s) s = "(null)";
    while (*s) pos = emit_char(buf, cap, pos, *s++);
    return pos;
}

static size_t emit_uint(char *buf, size_t cap, size_t pos,
                        uint64_t v, unsigned base, int upper)
{
    /* Largest 64-bit decimal: "18446744073709551615" — 20 chars. */
    char tmp[24];
    int  i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) {
        unsigned digit = (unsigned)(v % base);
        tmp[i++] = (char)(digit < 10 ? '0' + digit
                                     : (upper ? 'A' : 'a') + digit - 10);
        v /= base;
    }
    while (i--) pos = emit_char(buf, cap, pos, tmp[i]);
    return pos;
}

static size_t emit_int(char *buf, size_t cap, size_t pos, int64_t v)
{
    if (v < 0) {
        pos = emit_char(buf, cap, pos, '-');
        return emit_uint(buf, cap, pos, (uint64_t)-v, 10, 0);
    }
    return emit_uint(buf, cap, pos, (uint64_t)v, 10, 0);
}

int nxlibc_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    size_t pos = 0;
    while (*fmt) {
        if (*fmt != '%') {
            pos = emit_char(buf, cap, pos, *fmt++);
            continue;
        }
        fmt++;                    /* skip '%' */
        switch (*fmt) {
        case 'd':
        case 'i': {
            int v = va_arg(ap, int);
            pos = emit_int(buf, cap, pos, (int64_t)v);
            break;
        }
        case 'u': {
            unsigned v = va_arg(ap, unsigned);
            pos = emit_uint(buf, cap, pos, (uint64_t)v, 10, 0);
            break;
        }
        case 'x': {
            unsigned v = va_arg(ap, unsigned);
            pos = emit_uint(buf, cap, pos, (uint64_t)v, 16, 0);
            break;
        }
        case 'X': {
            unsigned v = va_arg(ap, unsigned);
            pos = emit_uint(buf, cap, pos, (uint64_t)v, 16, 1);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            pos = emit_str(buf, cap, pos, s);
            break;
        }
        case 'c': {
            int c = va_arg(ap, int);
            pos = emit_char(buf, cap, pos, (char)c);
            break;
        }
        case '%':
            pos = emit_char(buf, cap, pos, '%');
            break;
        case '\0':
            /* Trailing '%' with no conversion — silently emit it
             * verbatim and stop the format walk.  glibc treats this
             * as undefined; we choose lenient. */
            pos = emit_char(buf, cap, pos, '%');
            goto done;
        default:
            /* Unknown conversion — emit `%X` literally so the
             * mistake is visible in the output. */
            pos = emit_char(buf, cap, pos, '%');
            pos = emit_char(buf, cap, pos, *fmt);
            break;
        }
        fmt++;
    }
done:
    if (cap > 0) buf[(pos < cap ? pos : cap - 1)] = '\0';
    return (int)pos;
}

int nxlibc_snprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int rv = nxlibc_vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return rv;
}

int nxlibc_printf(const char *fmt, ...)
{
    char buf[NXLIBC_PRINTF_BUF];
    va_list ap;
    va_start(ap, fmt);
    int n = nxlibc_vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    /* nxlibc_vsnprintf returned the would-have-been length; cap
     * the actual write at the buffer size. */
    size_t to_write = (n < 0) ? 0
                              : ((size_t)n < sizeof buf ? (size_t)n
                                                        : sizeof buf - 1);
    nxlibc_ssize_t w = nxlibc_write(NXLIBC_STDOUT_FILENO, buf, to_write);
    return (w < 0) ? (int)w : n;
}

int nxlibc_putchar(int c)
{
    char ch = (char)c;
    nxlibc_ssize_t w = nxlibc_write(NXLIBC_STDOUT_FILENO, &ch, 1);
    return (w < 0) ? (int)w : (int)(unsigned char)ch;
}

int nxlibc_puts(const char *s)
{
    if (!s) s = "(null)";
    size_t n = nxlibc_strlen(s);
    nxlibc_ssize_t w1 = nxlibc_write(NXLIBC_STDOUT_FILENO, s, n);
    if (w1 < 0) return (int)w1;
    char nl = '\n';
    nxlibc_ssize_t w2 = nxlibc_write(NXLIBC_STDOUT_FILENO, &nl, 1);
    return (w2 < 0) ? (int)w2 : (int)(n + 1);
}
