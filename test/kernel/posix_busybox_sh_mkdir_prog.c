/*
 * Slice 7.7b.1 — busybox `sh -c "mkdir /m && > /m/x && ls /m"`.
 *
 * Exercises NX_SYS_MKDIRAT end-to-end through musl + ash + the new
 * vops->mkdir + hierarchical readdir.  Uses a unique directory name
 * (`/m`) to avoid colliding with `/tmp` — which already exists as a
 * synthesised dir in this kernel test image because slices 7.6d.N.8/.9/.11
 * left `/tmp/foo`, `/tmp/copy`, `/tmp/ap` files behind, making
 * `stat("/tmp")` report DIR via the slice-7.7b.1 stat synthesis path
 * and `mkdir /tmp` fail with EEXIST.
 *
 * Workflow:
 *   1. ash forks; child execs `/bin/busybox sh -c "..."`.
 *   2. Inside busybox: `mkdir /m` calls musl mkdir(2) → __NR_mkdirat=34
 *      → NX_SYS_MKDIRAT → vops->mkdir → ramfs_op_mkdir creates a
 *      RAMFS_KIND_DIR entry named "/m".
 *   3. `> /m/x` opens (O_WRONLY|O_CREAT) — slice 7.6d.N.8 path; creates
 *      a file `/m/x` with no parent-existence check (ramfs is
 *      permissive in v1).
 *   4. `ls /m` opendir → openat with O_DIRECTORY → sys_open's slice-7.7b.1
 *      stat probe sees `/m` is a DIR → HANDLE_DIR with cur->path = "/m";
 *      getdents64 calls vops->readdir(_, "/m", ...) → ramfs returns
 *      basename `x`.
 *
 * Expected captured output: `x` somewhere in the ls output.
 */

#include "components/libnxlibc/nxlibc.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    nxlibc_pid_t pid = nxlibc_fork();
    if (pid < 0) {
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-mkdir-fork-failed]", 24);
        nxlibc_exit(1);
    }

    if (pid == 0) {
        static char a0[] = "sh";
        static char a1[] = "-c";
        static char a2[] = "mkdir /m && > /m/x && ls /m";
        char *cargv[] = { a0, a1, a2, 0 };
        nxlibc_execve("/bin/busybox", cargv, 0);

        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-mkdir-exec-failed]", 24);
        nxlibc_exit(97);
    }

    nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-mkdir-parent]", 19);

    int status = 0;
    (void)nxlibc_waitpid(pid, &status, 0);

    static const char hex[] = "0123456789abcdef";
    char marker[]  = "[bbsh-mkdir-status=00]";
    int s = status & 0xff;
    marker[19] = hex[(s >> 4) & 0xf];
    marker[20] = hex[s & 0xf];
    nxlibc_write(NXLIBC_STDOUT_FILENO, marker, 22);

    if (status == 0)
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-mkdir-ok]", 15);
    else
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-mkdir-failed]", 19);

    nxlibc_exit(0);
}
