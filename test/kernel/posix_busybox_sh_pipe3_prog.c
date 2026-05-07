/*
 * Slice 7.6d.N.14 — busybox `sh -c "echo hello | tr a-z A-Z | wc -c"`.
 *
 * First 3-stage pipe.  Verifies the CHANNEL→CHANNEL→CONSOLE pipeline
 * composition holds at three stages.  `tr` and `wc` are non-builtin
 * applets (each exec'd from /bin/{tr,wc}), so this also exercises
 * the read-loop-on-CHANNEL-stdin pattern in two distinct consumer
 * roles: tr reads stdin, transforms, writes to its stdout (next
 * pipe); wc reads from its stdin (the pipe out of tr), counts, and
 * writes to its stdout (CONSOLE).
 *
 * Expected stdout: "6\n" (echo hello prints "HELLO\n" once tr-d,
 * which is 6 characters including the newline).  Surfaced — and this
 * slice closes — a real kernel composition gap: slice 7.6a's CHANNEL
 * fork-inheritance was NOT slot-position-preserving (it densely
 * re-allocated CHANNELs starting at the child's first INVALID slot).
 * That worked for 2-stage pipes (parent's CHANNEL slots stay dense
 * across the single fork) but broke 3+-stage pipes because ash
 * closes its own pipe ends right after each fork, leaving gaps in
 * the parent's table.  The dense re-allocation in subsequent forks
 * then assigned CHANNELs to *different* child slot indices than the
 * parent's pre-fork values — so the child's user code, holding the
 * parent's encoded handles verbatim, dup3'd the wrong slot.
 *
 * Fix: rewrite sys_fork's inheritance loop to write the parent's
 * CHANNEL at the parent's slot index in the child (overwriting the
 * pre-installed CONSOLE if that index is 0/1/2 — which busybox's
 * pipeline code never trips because it dup3's into 0/1/2 only after
 * fork, never before).  Encoded handles now stay stable across fork.
 */

#include "lib/libnxlibc/nxlibc.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    nxlibc_pid_t pid = nxlibc_fork();
    if (pid < 0) {
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-pipe3-fork-failed]", 24);
        nxlibc_exit(1);
    }

    if (pid == 0) {
        static char a0[] = "sh";
        static char a1[] = "-c";
        static char a2[] = "echo hello | tr a-z A-Z | wc -c";
        char *cargv[] = { a0, a1, a2, 0 };
        nxlibc_execve("/bin/busybox", cargv, 0);

        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-pipe3-exec-failed]", 24);
        nxlibc_exit(97);
    }

    nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-pipe3-parent]", 19);

    int status = 0;
    (void)nxlibc_waitpid(pid, &status, 0);

    static const char hex[] = "0123456789abcdef";
    char marker[]  = "[bbsh-pipe3-status=00]";
    int s = status & 0xff;
    marker[19] = hex[(s >> 4) & 0xf];
    marker[20] = hex[s & 0xf];
    nxlibc_write(NXLIBC_STDOUT_FILENO, marker, 22);

    if (status == 0)
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-pipe3-ok]", 15);
    else
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-pipe3-failed]", 19);

    nxlibc_exit(0);
}
