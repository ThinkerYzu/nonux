/*
 * Slice 7.6d.N.11 — busybox `sh -c "echo a >> /tmp/ap; echo b >> /tmp/ap"`.
 *
 * First append-redirect escalation past slice 7.6d.N.8's truncating
 * `>` slice.  ash translates `>>` into `O_WRONLY|O_CREAT|O_APPEND` —
 * Linux O_APPEND (= 0o2000) was unmapped before this slice, so a
 * naive run would have stomped its own first write.  Two `>>` clauses
 * back-to-back force the append semantic to actually take effect: the
 * second open creates a fresh per-open struct with cursor=0; without
 * seek-to-end-before-write, the second `echo b\n` would land at
 * offset 0 and overwrite the first `a\n`.
 */

#include "components/libnxlibc/nxlibc.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    nxlibc_pid_t pid = nxlibc_fork();
    if (pid < 0) {
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-append-fork-failed]", 25);
        nxlibc_exit(1);
    }

    if (pid == 0) {
        static char a0[] = "sh";
        static char a1[] = "-c";
        static char a2[] = "echo a >> /tmp/ap; echo b >> /tmp/ap";
        char *cargv[] = { a0, a1, a2, 0 };
        nxlibc_execve("/bin/busybox", cargv, 0);

        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-append-exec-failed]", 25);
        nxlibc_exit(97);
    }

    nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-append-parent]", 20);

    int status = 0;
    (void)nxlibc_waitpid(pid, &status, 0);

    static const char hex[] = "0123456789abcdef";
    char marker[]  = "[bbsh-append-status=00]";
    int s = status & 0xff;
    marker[20] = hex[(s >> 4) & 0xf];
    marker[21] = hex[s & 0xf];
    nxlibc_write(NXLIBC_STDOUT_FILENO, marker, 23);

    if (status == 0)
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-append-ok]", 16);
    else
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-append-failed]", 20);

    nxlibc_exit(0);
}
