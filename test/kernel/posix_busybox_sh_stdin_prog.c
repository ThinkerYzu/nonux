/*
 * Slice 7.6d.N.10a — busybox `sh -c "cat < /banner"`.
 *
 * First STDIN-redirection-from-file escalation.  Slices N.6b/N.7
 * exercised cat-as-pipeline-stage (CHANNEL on fd 0) and cat-with-
 * path-arg (FILE at slot 3+); this slice replaces the inherited
 * STDIN CONSOLE at slot 2 with a HANDLE_FILE.
 *
 * Concretely the path is:
 *   parent ash         fork
 *   child  ash         openat("/banner", O_RDONLY) -> fdN
 *   child  ash         dup3(fdN, 0, 0)              (dup3 newfd=0
 *                                                    -> slot 2 per
 *                                                    slice N.6b)
 *   child  ash         close(fdN)                   (FILE refs --)
 *   child  ash         execve("/bin/cat", { "cat" })
 *   child  cat         (no path arg) read loop on fd 0
 *                      -> FILE arm of sys_read via the slot-2
 *                         redirection, byte-for-byte echo to
 *                         CONSOLE on fd 1
 *   child  cat         exit
 *   parent ash         wait + exit 0
 *
 * Status decode for the captured marker:
 *   0x00 — pipeline ran end-to-end, ash exited 0.  Success.
 *   0x01 — generic failure.
 *   0x7f — `command not found`.
 *   0x8b — SIGSEGV (kernel-side gap).
 *   0x84 — SIGILL.
 */

#include "lib/libnxlibc/nxlibc.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    nxlibc_pid_t pid = nxlibc_fork();
    if (pid < 0) {
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-stdin-fork-failed]", 24);
        nxlibc_exit(1);
    }

    if (pid == 0) {
        static char a0[] = "sh";
        static char a1[] = "-c";
        static char a2[] = "cat < /banner";
        char *cargv[] = { a0, a1, a2, 0 };
        nxlibc_execve("/bin/busybox", cargv, 0);

        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-stdin-exec-failed]", 24);
        nxlibc_exit(97);
    }

    nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-stdin-parent]", 19);

    int status = 0;
    (void)nxlibc_waitpid(pid, &status, 0);

    static const char hex[] = "0123456789abcdef";
    char marker[]  = "[bbsh-stdin-status=00]";
    int s = status & 0xff;
    marker[19] = hex[(s >> 4) & 0xf];
    marker[20] = hex[s & 0xf];
    nxlibc_write(NXLIBC_STDOUT_FILENO, marker, 22);

    if (status == 0)
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-stdin-ok]", 15);
    else
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-stdin-failed]", 19);

    nxlibc_exit(0);
}
