/*
 * Slice 7.6c.3c — sys_exec → musl-linked child round-trip.
 *
 * Validates the AUXV consumption path end-to-end.  Slice 7.6c.3b
 * implemented the AUXV push in sys_exec (AT_PAGESZ, AT_RANDOM,
 * AT_NULL between envp's NULL terminator and the argv strings) but
 * only exercised it through drop_to_el0 — which doesn't use
 * sys_exec, so AUXV is never consumed.  This program drives the
 * full path: fork() + execve("/musl_prog", argv, NULL) loads the
 * musl-linked posix_musl_prog ELF from the initramfs-seeded ramfs
 * file; sys_exec builds the System V layout including AUXV; musl's
 * crt1 + __libc_start_main + __init_libc walk the layout, read
 * AT_RANDOM for the stack canary + AT_PAGESZ for libc.page_size,
 * and reach main().  Child emits `[musl-ok]` and _exit(57); parent
 * waits + emits `[musl-exec-ok]` if status == 57.
 *
 * Five-marker live-log invariant for the success path:
 *   [musl-exec-parent] -> [musl-ok] (from the musl-linked child) ->
 *   [musl-exec-ok] (parent's success marker).
 *
 * If AUXV consumption breaks (e.g. AT_RANDOM dereferences a bad
 * pointer, AT_PAGESZ is malformed and crashes a malloc-pulling
 * code path), musl's __init_libc faults and the child never
 * reaches `_exit(57)` — parent's wait sees a non-57 status, the
 * ktest assertion fails.
 */

#include "components/posix_shim/nxlibc.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    nxlibc_pid_t pid = nxlibc_fork();
    if (pid < 0) nxlibc_exit(1);

    if (pid == 0) {
        /* Child branch.  Single-element argv ({"musl_prog", NULL})
         * — same shape Linux would produce for `./musl_prog` with
         * no args.  envp is NULL = empty environment.  sys_exec
         * pushes AT_PAGESZ + AT_RANDOM + AT_NULL after the envp
         * NULL; musl's __init_libc reads them. */
        static char prog[] = "/musl_prog";
        char *cargv[] = { prog, 0 };
        nxlibc_execve("/musl_prog", cargv, 0);

        /* Unreachable on success.  Sentinel to distinguish
         * exec-load failure (98) from the child's own exit codes
         * (the musl-linked child exits with 57 on success; any
         * other low-positive value indicates a failure inside
         * musl's init or main). */
        nxlibc_exit(98);
    }

    /* Parent.  Presence marker + wait + success marker. */
    nxlibc_write(NXLIBC_STDOUT_FILENO, "[musl-exec-parent]", 18);

    int status = 0;
    (void)nxlibc_waitpid(pid, &status, 0);
    if (status == 57)
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[musl-exec-ok]", 14);

    nxlibc_exit(0);
}
