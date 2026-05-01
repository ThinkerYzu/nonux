/*
 * Slice 7.6d.N.13 — busybox `sh -c "id; uname -a"`.
 *
 * First workload to exercise the tolerable-syscall stubs sweep.  `id`
 * touches getuid/geteuid/getgid/getegid + glibc-name-resolution paths
 * that fall back to "uid=0(root)" / "gid=0(root)" with our zero
 * stubs.  `uname -a` calls uname(2) and prints sysname + nodename +
 * release + version + machine.
 *
 * Both commands are non-builtin applets dispatched via PATH lookup,
 * so this also re-exercises the slice 7.6d.N.4 PATH walk + slice
 * 7.6d.N.6b CONSOLE write path.
 *
 * Expected stdout: a line beginning "uid=0" followed by a `nonux ...
 * aarch64` line.  Parent exits 0 either way; the kernel-side ktest
 * only asserts the parent's exit status (the byte content lives in
 * the QEMU log for human inspection).
 */

#include "components/libnxlibc/nxlibc.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    nxlibc_pid_t pid = nxlibc_fork();
    if (pid < 0) {
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-idun-fork-failed]", 23);
        nxlibc_exit(1);
    }

    if (pid == 0) {
        static char a0[] = "sh";
        static char a1[] = "-c";
        static char a2[] = "id; uname -a";
        char *cargv[] = { a0, a1, a2, 0 };
        nxlibc_execve("/bin/busybox", cargv, 0);

        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-idun-exec-failed]", 23);
        nxlibc_exit(97);
    }

    nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-idun-parent]", 18);

    int status = 0;
    (void)nxlibc_waitpid(pid, &status, 0);

    static const char hex[] = "0123456789abcdef";
    char marker[]  = "[bbsh-idun-status=00]";
    int s = status & 0xff;
    marker[18] = hex[(s >> 4) & 0xf];
    marker[19] = hex[s & 0xf];
    nxlibc_write(NXLIBC_STDOUT_FILENO, marker, 21);

    if (status == 0)
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-idun-ok]", 14);
    else
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-idun-failed]", 18);

    nxlibc_exit(0);
}
