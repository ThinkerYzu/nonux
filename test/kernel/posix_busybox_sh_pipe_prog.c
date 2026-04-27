/*
 * Slice 7.6d.N.6 — busybox `sh -c "echo hello | cat"` discovery.
 *
 * First PIPE escalation.  Slices 7.6d.N.1..5 covered builtins-
 * only and a single fork+execve.  This sub-slice forces ash to
 * fork TWICE (once per stage of the pipeline) and wire up an
 * actual pipe between them.
 *
 * Likely failure modes (any subset may surface in one run):
 *
 *   1. pipe(2) shape.  We have NX_SYS_PIPE since slice 7.5,
 *      mapped from `__NR_pipe2 (59)` likely as well — needs
 *      verification.  musl 1.2.5's `pipe(int[2])` calls
 *      `pipe2(fds, 0)` on aarch64 (no plain pipe syscall);
 *      `__NR_pipe2 = 59` may or may not be in our translation
 *      table.
 *   2. dup2 / dup3.  ash redirects stdout of stage 1 and stdin
 *      of stage 2 via dup2.  musl's `dup2(o, n)` calls
 *      `dup3(o, n, 0)` on aarch64 (`__NR_dup3 = 24`).  Today
 *      we have no dup syscall at all; the kernel will return
 *      -ENOSYS and ash's pipeline setup will bail.
 *   3. cat's read path.  cat calls `read` repeatedly on fd 0
 *      until EOF — but musl's stdio buffered read uses
 *      `readv` (`__NR_readv = 65`).  We have writev (slice
 *      7.6c.3c) but no readv.  cat's `__stdio_read` will
 *      return -ENOSYS the moment it reads its first byte.
 *   4. Channel handle close-as-pipe-EOF.  Once stage 1 exits
 *      and its stdout-end of the pipe is closed, stage 2's
 *      read on the read-end must observe EOF (zero-byte
 *      return) rather than block forever.  Slice 7.5's
 *      pipe_prog single-process test never exercised the
 *      cross-process EOF transition.
 *   5. Process-table waterline.  Each `sh -c "echo | cat"`
 *      invocation now leaks 3 processes (ash + echo + cat)
 *      ≈ 24 MiB; cumulative across the suite stays well
 *      under the 64-slot cap from slice 7.6d.N.2.
 *
 * Discovery-only.  Test asserts only parent-side liveness;
 * the actual child status + interleaved markers tell the
 * story.  Status decode for the captured marker:
 *   0x00 — pipeline ran to completion, ash exited 0.  Success.
 *   0x01 — generic failure (one of the stages bailed).
 *   0x7f — `command not found`.
 *   0x8b — SIGSEGV (kernel-side gap).
 *   0x84 — SIGILL.
 */

#include "components/posix_shim/nxlibc.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    nxlibc_pid_t pid = nxlibc_fork();
    if (pid < 0) {
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-pipe-fork-failed]", 23);
        nxlibc_exit(1);
    }

    if (pid == 0) {
        static char a0[] = "sh";
        static char a1[] = "-c";
        static char a2[] = "echo hello | cat";
        char *cargv[] = { a0, a1, a2, 0 };
        nxlibc_execve("/bin/busybox", cargv, 0);

        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-pipe-exec-failed]", 23);
        nxlibc_exit(97);
    }

    nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-pipe-parent]", 18);

    int status = 0;
    (void)nxlibc_waitpid(pid, &status, 0);

    static const char hex[] = "0123456789abcdef";
    char marker[]  = "[bbsh-pipe-status=00]";
    int s = status & 0xff;
    marker[18] = hex[(s >> 4) & 0xf];
    marker[19] = hex[s & 0xf];
    nxlibc_write(NXLIBC_STDOUT_FILENO, marker, 21);

    if (status == 0)
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-pipe-ok]", 14);
    else
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-pipe-failed]", 18);

    nxlibc_exit(0);
}
