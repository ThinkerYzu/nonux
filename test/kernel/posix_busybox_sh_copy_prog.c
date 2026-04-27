/*
 * Slice 7.6d.N.9 — busybox `sh -c "cat /banner > /tmp/copy"`.
 *
 * First STDOUT-redirection-to-file escalation against an EXTERNAL
 * command.  Slice 7.6d.N.8 covered the builtin form (echo); the
 * structural difference here is that ash forks before the redirect
 * setup runs, so the FILE handle the redirection installs at fd 1
 * has to survive sys_exec into /bin/cat.
 *
 * Concretely the path is:
 *   parent ash         fork
 *   child  ash         openat("/tmp/copy", O_CREAT|O_WRONLY|O_TRUNC) -> fdN
 *   child  ash         dup3(fdN, 1, 0)   (FILE retain; slot 0 was CONSOLE)
 *   child  ash         close(fdN)        (FILE refs back to 1)
 *   child  ash         execve("/bin/cat", { "cat", "/banner" })
 *   child  cat         openat("/banner") -> HANDLE_FILE
 *   child  cat         read loop -> EOF
 *   child  cat         write(1, ...)     (HANDLE_FILE write into /tmp/copy)
 *   child  cat         exit
 *   parent ash         wait + exit 0
 *
 * After the parent exits, the ktest re-opens /tmp/copy through
 * vfs_simple and asserts the contents are exactly the banner text
 * ("hello from initramfs\n").
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
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-copy-fork-failed]", 23);
        nxlibc_exit(1);
    }

    if (pid == 0) {
        static char a0[] = "sh";
        static char a1[] = "-c";
        static char a2[] = "cat /banner > /tmp/copy";
        char *cargv[] = { a0, a1, a2, 0 };
        nxlibc_execve("/bin/busybox", cargv, 0);

        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-copy-exec-failed]", 23);
        nxlibc_exit(97);
    }

    nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-copy-parent]", 18);

    int status = 0;
    (void)nxlibc_waitpid(pid, &status, 0);

    static const char hex[] = "0123456789abcdef";
    char marker[]  = "[bbsh-copy-status=00]";
    int s = status & 0xff;
    marker[18] = hex[(s >> 4) & 0xf];
    marker[19] = hex[s & 0xf];
    nxlibc_write(NXLIBC_STDOUT_FILENO, marker, 21);

    if (status == 0)
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-copy-ok]", 14);
    else
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-copy-failed]", 18);

    nxlibc_exit(0);
}
