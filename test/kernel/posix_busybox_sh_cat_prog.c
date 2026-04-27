/*
 * Slice 7.6d.N.7 — busybox `sh -c "cat /banner"`.
 *
 * First file-input escalation past the pipe slice.  Single fork +
 * execve of the cat applet against an actual ramfs file (/banner,
 * seeded by initramfs as "hello from initramfs\n").  The busybox
 * pipe test (slice 7.6d.N.6b) drove cat reading from a CHANNEL
 * handle; this drives cat reading from a FILE handle, which has
 * different fast-path heuristics in cat (sendfile / splice / read
 * fall-through) and a different EOF source.
 *
 * Status decode for the captured marker:
 *   0x00 — pipeline ran to completion, ash exited 0.  Success.
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
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-cat-fork-failed]", 22);
        nxlibc_exit(1);
    }

    if (pid == 0) {
        static char a0[] = "sh";
        static char a1[] = "-c";
        static char a2[] = "cat /banner";
        char *cargv[] = { a0, a1, a2, 0 };
        nxlibc_execve("/bin/busybox", cargv, 0);

        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-cat-exec-failed]", 22);
        nxlibc_exit(97);
    }

    nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-cat-parent]", 17);

    int status = 0;
    (void)nxlibc_waitpid(pid, &status, 0);

    static const char hex[] = "0123456789abcdef";
    char marker[]  = "[bbsh-cat-status=00]";
    int s = status & 0xff;
    marker[17] = hex[(s >> 4) & 0xf];
    marker[18] = hex[s & 0xf];
    nxlibc_write(NXLIBC_STDOUT_FILENO, marker, 20);

    if (status == 0)
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-cat-ok]", 13);
    else
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-cat-failed]", 17);

    nxlibc_exit(0);
}
