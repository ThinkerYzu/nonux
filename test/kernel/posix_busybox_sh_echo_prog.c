/*
 * Slice 7.6d.N.2 — busybox `sh -c "echo hello"` discovery.
 *
 * Slice 7.6d.N.1 (session 56) closed the malloc gap: ash's
 * startup runs through `mmap` (via the new NX_SYS_MMAP) and
 * `sh -c "exit 42"` exits cleanly with status 42 via the
 * `exit` builtin — without consulting sigaction / getuid /
 * ioctl / fs lookup.
 *
 * This sub-slice escalates to the smallest non-trivial -c
 * string: `echo hello`.  Differences from the `exit 42` path:
 *
 *   - ash has to parse the -c string into a command tree
 *     (parser allocates more from mallocng — exercises the
 *     mmap arena past its initial bootstrap allocation).
 *   - The `echo` builtin issues `write(STDOUT_FILENO,
 *     "hello\n", 6)` from inside the shell.  Goes through
 *     musl's stdio buffering (`__stdio_write` → SYS_writev),
 *     which is wired up since slice 7.6c.3c.  Should land at
 *     the magic-fd-handle and emit on UART.
 *   - `echo` (unlike `exit`) doesn't unconditionally call
 *     `_exit`; ash returns from the builtin and proceeds to
 *     the script's normal end-of-input shutdown path —
 *     stdio flush, atexit, etc.
 *
 * Possible failure modes if anything new surfaces:
 *
 *   1. mallocng arena exhaustion: ash's parser may pull more
 *      slabs than the initial bootstrap; if total exceeds
 *      3 MiB we'll get an early OOM.  Unlikely for one short
 *      echo line, but possible if mallocng's geometric slab
 *      growth happens to land on a big slab early.
 *   2. New syscall reached by ash's normal-exit path.
 *      Candidates: `rt_sigaction` (ash sets default handlers
 *      on shutdown), `sigprocmask`, `tcgetattr` (terminal
 *      mode reset on exit if isatty fd 0).  All currently
 *      -ENOSYS in our translation table; ash MAY tolerate
 *      ENOSYS or MAY treat it as fatal.
 *   3. `getpgid` / `setpgid` / `tcsetpgrp` for job control
 *      cleanup.  Same -ENOSYS story.
 *
 * Discovery-only: no kernel changes.  The test asserts only
 * parent-side liveness (the `[bbsh-echo-...]` markers); the
 * actual child exit + the `hello` output between markers
 * tells us what happened.
 *
 * Status decode for the captured marker:
 *   0x00 — `echo hello` ran, ash exited 0.  Success.
 *   0x01 — ash exited 1 (generic failure — could be OOM).
 *   0x8b — SIGSEGV in ash (would imply a kernel-side gap).
 *   0x84 — SIGILL.
 *   0x7f — `command not found` (ash didn't find echo, but
 *          echo IS a builtin so this would be very strange).
 */

#include "components/posix_shim/nxlibc.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    nxlibc_pid_t pid = nxlibc_fork();
    if (pid < 0) {
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-echo-fork-failed]", 23);
        nxlibc_exit(1);
    }

    if (pid == 0) {
        static char a0[] = "sh";
        static char a1[] = "-c";
        static char a2[] = "echo hello";
        char *cargv[] = { a0, a1, a2, 0 };
        nxlibc_execve("/bin/busybox", cargv, 0);

        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-echo-exec-failed]", 23);
        nxlibc_exit(97);
    }

    nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-echo-parent]", 18);

    int status = 0;
    (void)nxlibc_waitpid(pid, &status, 0);

    static const char hex[] = "0123456789abcdef";
    char marker[]  = "[bbsh-echo-status=00]";
    int s = status & 0xff;
    marker[18] = hex[(s >> 4) & 0xf];
    marker[19] = hex[s & 0xf];
    nxlibc_write(NXLIBC_STDOUT_FILENO, marker, 21);

    if (status == 0)
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-echo-ok]", 14);
    else
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-echo-failed]", 18);

    nxlibc_exit(0);
}
