/*
 * Slice 7.6c.4 — argv-push round-trip: the parent that forks +
 * execves a child with explicit argv.
 *
 * Flow:
 *   1. fork().
 *   2. Child branch (rv == 0):
 *        argv = { "/argv_child", "hello", "world", NULL };
 *        execve("/argv_child", argv, NULL);
 *      execve only returns on failure — fall through to exit(99)
 *      so the parent's wait sees a distinct status if exec fails
 *      to load.
 *   3. Parent branch (pid > 0):
 *        nxlibc_write a `[argv-parent]` marker.
 *        waitpid + capture status.
 *        if (status == 63) → emit `[argv-ok]` (63 = argc + 60 from
 *        the child's success path).
 *        nxlibc_exit(0) on success.
 *
 * Five-marker live-log invariant for the success path:
 *   [argv-parent] →
 *   [argv-child argc=3] [argv-child argv[0]=/argv_child]
 *   [argv-child argv[1]=hello] [argv-child argv[2]=world]
 *   [argv-child-ok] →
 *   [argv-ok]
 *
 * If sys_exec doesn't push argv (or pushes the wrong values), the
 * child takes one of its sentinel-exit branches (codes 81..85) and
 * the parent's wait sees a non-63 status — the [argv-ok] marker
 * never fires, and the ktest's exit-code assertion fails.
 */

#include "components/posix_shim/nxlibc.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    nxlibc_pid_t pid = nxlibc_fork();
    if (pid < 0) nxlibc_exit(1);

    if (pid == 0) {
        /* Child branch.  argv is laid out as a stack-local array
         * of pointers into static storage.  sys_exec walks the
         * pointer array, copies each string into kernel staging,
         * then rebuilds the layout on the new user stack. */
        static char prog[]  = "/argv_child";
        static char arg1[]  = "hello";
        static char arg2[]  = "world";
        char *cargv[] = { prog, arg1, arg2, 0 };
        nxlibc_execve("/argv_child", cargv, 0);

        /* Unreachable on success.  Sentinel to expose exec
         * failures distinct from the child's own exit codes. */
        nxlibc_exit(99);
    }

    /* Parent.  Emit a presence marker, then wait for the child. */
    nxlibc_write(NXLIBC_STDOUT_FILENO, "[argv-parent]", 13);

    int status = 0;
    (void)nxlibc_waitpid(pid, &status, 0);
    if (status == 63) nxlibc_write(NXLIBC_STDOUT_FILENO, "[argv-ok]", 9);

    nxlibc_exit(0);
}
