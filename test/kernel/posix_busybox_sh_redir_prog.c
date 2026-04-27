/*
 * Slice 7.6d.N.8 — busybox `sh -c "echo a > /tmp/foo"` discovery.
 *
 * First STDOUT-redirection-to-file escalation past the cat slice.
 * ash parses the redirection, opens /tmp/foo with
 * O_WRONLY|O_CREAT|O_TRUNC, dup2's it onto stdout, runs the echo
 * builtin (which writes "a\n"), then restores stdout.  Echo is a
 * NOFORK applet so all of this happens in the shell process (no
 * fork between open and write — file inheritance through fork is a
 * separate concern).
 *
 * Predicted failure mode (will be confirmed at runtime):
 *
 *   ash's `redirect()` calls `save_fd_on_redirect(1, ...)` before
 *   installing the redirection.  That path calls
 *   `dup_CLOEXEC(1, ...)` -> `fcntl(1, F_DUPFD_CLOEXEC, base)`.
 *   `__NR_fcntl = 25` is unmapped today, so musl returns -ENOSYS.
 *   ash sees errno != EBADF and calls `xfunc_die()` (libbb), which
 *   exit(1)'s.  Test parent observes status 1 and the file
 *   /tmp/foo is created but empty.
 *
 * Status decode for the captured marker:
 *   0x00 — redirection ran end-to-end, ash exited 0.  Success.
 *   0x01 — generic failure (most likely fcntl ENOSYS -> xfunc_die).
 *   0x7f — `command not found`.
 *   0x8b — SIGSEGV.
 *   0x84 — SIGILL.
 */

#include "components/posix_shim/nxlibc.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    nxlibc_pid_t pid = nxlibc_fork();
    if (pid < 0) {
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-redir-fork-failed]", 24);
        nxlibc_exit(1);
    }

    if (pid == 0) {
        static char a0[] = "sh";
        static char a1[] = "-c";
        static char a2[] = "echo a > /tmp/foo";
        char *cargv[] = { a0, a1, a2, 0 };
        nxlibc_execve("/bin/busybox", cargv, 0);

        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-redir-exec-failed]", 24);
        nxlibc_exit(97);
    }

    nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-redir-parent]", 19);

    int status = 0;
    (void)nxlibc_waitpid(pid, &status, 0);

    static const char hex[] = "0123456789abcdef";
    char marker[]  = "[bbsh-redir-status=00]";
    int s = status & 0xff;
    marker[19] = hex[(s >> 4) & 0xf];
    marker[20] = hex[s & 0xf];
    nxlibc_write(NXLIBC_STDOUT_FILENO, marker, 22);

    if (status == 0)
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-redir-ok]", 15);
    else
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-redir-failed]", 19);

    nxlibc_exit(0);
}
