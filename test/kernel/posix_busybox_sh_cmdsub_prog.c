/*
 * Slice 7.6d.N.10b — busybox `sh -c "echo $(cat /banner)"`.
 *
 * Command-substitution escalation.  The parent shell — not an
 * exec'd child like in slice 7.6d.N.6b — is the consumer of a
 * pipe.  ash sets up a pipe, forks once for the inner `cat`, the
 * child execve's /bin/cat with stdout redirected to the pipe
 * write end; the parent ash reads the pipe read end via the
 * same `sys_read` CHANNEL arm exercised by N.6b but from a
 * non-exec'd context (no address-space swap between fork and
 * read), tokenises the captured bytes, then dispatches the outer
 * `echo` builtin which writes the substituted text to stdout
 * (CONSOLE).
 *
 * Concretely the path is:
 *   parent ash         pipe(p)
 *   parent ash         fork
 *   child  ash         dup3(p[1], 1, 0)            (pipe-write -> fd 1)
 *   child  ash         close(p[0]), close(p[1])
 *   child  ash         execve("/bin/cat", { "cat", "/banner" })
 *   child  cat         read /banner -> write fd 1 -> pipe
 *   child  cat         exit
 *   parent ash         close(p[1])                 (so cat's read-end
 *                                                   sees EOF on cat
 *                                                   exit; symmetric
 *                                                   to N.6b)
 *   parent ash         read p[0] until EOF         (CHANNEL arm of
 *                                                   sys_read in the
 *                                                   parent process)
 *   parent ash         tokenise; dispatch echo
 *   parent ash         write fd 1 -> CONSOLE       (substituted text
 *                                                   + newline)
 *   parent ash         exit
 *
 * The captured banner is "hello from initramfs\n"; ash's word-
 * splitting collapses the trailing newline, so echo's output to
 * the live UART is `hello from initramfs\n` — same content, but
 * routed through the CHANNEL+sys_read+tokenise+CONSOLE+sys_write
 * chain entirely inside the parent process.
 *
 * Status decode for the captured marker:
 *   0x00 — pipeline ran end-to-end, ash exited 0.  Success.
 *   0x01 — generic failure.
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
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-cmdsub-fork-failed]", 25);
        nxlibc_exit(1);
    }

    if (pid == 0) {
        static char a0[] = "sh";
        static char a1[] = "-c";
        static char a2[] = "echo $(cat /banner)";
        char *cargv[] = { a0, a1, a2, 0 };
        nxlibc_execve("/bin/busybox", cargv, 0);

        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-cmdsub-exec-failed]", 25);
        nxlibc_exit(97);
    }

    nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-cmdsub-parent]", 20);

    int status = 0;
    (void)nxlibc_waitpid(pid, &status, 0);

    static const char hex[] = "0123456789abcdef";
    char marker[]  = "[bbsh-cmdsub-status=00]";
    int s = status & 0xff;
    marker[20] = hex[(s >> 4) & 0xf];
    marker[21] = hex[s & 0xf];
    nxlibc_write(NXLIBC_STDOUT_FILENO, marker, 23);

    if (status == 0)
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-cmdsub-ok]", 16);
    else
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-cmdsub-failed]", 20);

    nxlibc_exit(0);
}
