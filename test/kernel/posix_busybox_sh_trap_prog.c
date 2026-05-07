/*
 * Slice 7.6d.N.12 — busybox `sh -c "trap 'echo bye' EXIT; echo body"`.
 *
 * First workload that exercises the rt_sigaction / rt_sigprocmask
 * stubs.  ash unconditionally walks its trap-table on startup and
 * touches `sigprocmask` (to learn the inherited mask) and, depending
 * on the trap target, `sigaction` (to install the kernel-side
 * handler).  Without these syscalls returning success, ash bails
 * before parsing the script body.
 *
 * The EXIT pseudo-signal (slot 0 in busybox's trap table) is handled
 * entirely inside ash — it doesn't go through `sigaction(0, ...)` —
 * so this workload doesn't *yet* require the kernel-→user signal
 * trampoline.  It exercises only the no-op stub path.  A later
 * slice (N.final's interactive prompt) is the first to need real
 * SIGINT delivery to a user-installed handler.
 *
 * Expected output: "body\n" then "bye\n", parent exits with status 0.
 */

#include "lib/libnxlibc/nxlibc.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    nxlibc_pid_t pid = nxlibc_fork();
    if (pid < 0) {
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-trap-fork-failed]", 23);
        nxlibc_exit(1);
    }

    if (pid == 0) {
        static char a0[] = "sh";
        static char a1[] = "-c";
        static char a2[] = "trap 'echo bye' EXIT; echo body";
        char *cargv[] = { a0, a1, a2, 0 };
        nxlibc_execve("/bin/busybox", cargv, 0);

        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-trap-exec-failed]", 23);
        nxlibc_exit(97);
    }

    nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-trap-parent]", 18);

    int status = 0;
    (void)nxlibc_waitpid(pid, &status, 0);

    static const char hex[] = "0123456789abcdef";
    char marker[]  = "[bbsh-trap-status=00]";
    int s = status & 0xff;
    marker[18] = hex[(s >> 4) & 0xf];
    marker[19] = hex[s & 0xf];
    nxlibc_write(NXLIBC_STDOUT_FILENO, marker, 21);

    if (status == 0)
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-trap-ok]", 14);
    else
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-trap-failed]", 18);

    nxlibc_exit(0);
}
