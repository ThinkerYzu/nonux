/*
 * Slice 7.6d.N.15 — busybox `sh -c "(cat) < /banner"`.
 *
 * First workload that exercises FILE-fd inheritance through fork.
 * Outer shell parses `(cat) < /banner` as: fork a SUBSHELL with the
 * redirect applied; the subshell runs `cat`.  Sequence:
 *   1. outer ash forks for the subshell.
 *   2. subshell ash applies `< /banner`: open(/banner) + dup2(_, 0),
 *      so subshell-ash has the file open at its fd 0 (slot 2 in our
 *      scheme — the CONSOLE-STDIN-aliased slot).
 *   3. subshell ash forks for cat.  cat's child INHERITS subshell-
 *      ash's fd-0-as-FILE through the fork (this is the slice's
 *      load-bearing path).
 *   4. cat exec's; reads stdin → routes to slot 2 → FILE → /banner.
 *
 * Without slice 7.6d.N.15's FILE-handle inheritance in sys_fork,
 * step 3 leaves cat's child with the pre-installed CONSOLE at slot
 * 2 instead of the FILE — cat reads CONSOLE which returns 0 = EOF
 * in v1, and prints nothing.
 *
 * The test deliberately avoids the `exec 3< /banner; head <&3`
 * idiom because that hits a separate kernel gap (POSIX fd 3 aliases
 * onto our slot 2 via the `(gen << 8) | (idx + 1)` encoding —
 * encoded handle 3 == slot 2 == CONSOLE STDIN).  The fix for that
 * is slice 7.6d.N.16's POSIX-fd-alignment work; until then the
 * subshell idiom is what surfaces FILE-fd inheritance cleanly.
 *
 * Expected stdout: "hello from initramfs\n" (the entire banner, cat
 * just copies its stdin to its stdout).
 */

#include "lib/libnxlibc/nxlibc.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    nxlibc_pid_t pid = nxlibc_fork();
    if (pid < 0) {
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-xfile-fork-failed]", 24);
        nxlibc_exit(1);
    }

    if (pid == 0) {
        static char a0[] = "sh";
        static char a1[] = "-c";
        static char a2[] = "(cat) < /banner";
        char *cargv[] = { a0, a1, a2, 0 };
        nxlibc_execve("/bin/busybox", cargv, 0);

        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-xfile-exec-failed]", 24);
        nxlibc_exit(97);
    }

    nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-xfile-parent]", 19);

    int status = 0;
    (void)nxlibc_waitpid(pid, &status, 0);

    static const char hex[] = "0123456789abcdef";
    char marker[]  = "[bbsh-xfile-status=00]";
    int s = status & 0xff;
    marker[19] = hex[(s >> 4) & 0xf];
    marker[20] = hex[s & 0xf];
    nxlibc_write(NXLIBC_STDOUT_FILENO, marker, 22);

    if (status == 0)
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-xfile-ok]", 15);
    else
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-xfile-failed]", 19);

    nxlibc_exit(0);
}
