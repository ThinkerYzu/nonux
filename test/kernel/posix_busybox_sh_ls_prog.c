/*
 * Slice 7.6d.N.4 — busybox `sh -c "ls /"` discovery.
 *
 * First NON-builtin escalation.  Slices 7.6d.N.1/.2/.3 ran
 * builtins-only (`exit`, `echo`, `echo;echo`).  This sub-slice
 * crosses into ash forking + execve'ing an external command.
 *
 * Initramfs prep (in Makefile): `/bin/busybox` is duplicated as
 * `/bin/ls` (1.22 MB extra; ramfs has 8 file slots, each 4 MiB
 * capacity, so the duplicate fits with margin).  When ash
 * forks + execve's `/bin/ls`, the freshly-loaded busybox sees
 * `argv[0] = "ls"` and dispatches to the `ls` applet via
 * `basename(argv[0])` — same in-process applet-table lookup
 * that routed `argv[0] = "sh"` to ash for slices 7.6d.N.0+.
 *
 * Likely failure modes (multiple gaps may surface at once):
 *
 *   1. PATH lookup.  ash's default PATH is
 *      `/sbin:/usr/sbin:/bin:/usr/bin`.  We only have `/bin/ls`
 *      (newly added).  ash's `find_command()` walks PATH
 *      stat'ing each candidate; if `stat` is unmapped (likely)
 *      ash either bails or only walks until it hits one that
 *      doesn't return -ENOSYS.
 *   2. ash's fork+wait codepath.  Different from our test
 *      parent's: may pull in `sigprocmask` to mask SIGCHLD
 *      around fork, `tcsetpgrp` to give the child the
 *      foreground process group, or `setpgid`.  All currently
 *      -ENOSYS in our translation table.
 *   3. ls's syscalls.  Once the applet runs, it needs:
 *        - `getdents64` (`__NR_getdents64 = 61`) for directory
 *          listing.  We have NX_SYS_READDIR (slice 6.4) but
 *          the translation isn't wired.
 *        - `stat` / `fstatat` (`__NR_fstatat = 79`,
 *          `__NR_newfstatat`).  Completely unmapped.  ls calls
 *          stat per-entry to print type/perms/size.
 *        - `ioctl(1, TIOCGWINSZ)` for column-width sizing.
 *          Probably tolerable as -ENOTTY (ls falls back to
 *          single-column or fixed width).
 *   4. process-table cap.  ash forks ls, both leak per current
 *      no-reap behaviour.  Cumulative leak per `sh -c "ls /"`
 *      ≈ 3 processes × 8 MiB.  64-slot cap from session 57
 *      should still absorb this for now.
 *
 * Discovery-only.  The test asserts only parent-side liveness;
 * the actual child exit + the markers between parent prints
 * tell us what happened.
 *
 * Status decode for the captured marker:
 *   0x00 — `ls /` ran, listed contents, ash exited 0.  Success.
 *   0x01 — generic failure (ash or ls bail; could be PATH,
 *          could be a syscall gap mid-execution).
 *   0x02 — ash parse error (very unlikely — same parser as
 *          slice 7.6d.N.3).
 *   0x7f — `command not found` (ash's path-search bailed).
 *   0x8b — SIGSEGV (kernel-side gap).
 *   0x84 — SIGILL.
 */

#include "components/posix_shim/nxlibc.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    nxlibc_pid_t pid = nxlibc_fork();
    if (pid < 0) {
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-ls-fork-failed]", 21);
        nxlibc_exit(1);
    }

    if (pid == 0) {
        static char a0[] = "sh";
        static char a1[] = "-c";
        static char a2[] = "ls /";
        char *cargv[] = { a0, a1, a2, 0 };
        nxlibc_execve("/bin/busybox", cargv, 0);

        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-ls-exec-failed]", 21);
        nxlibc_exit(97);
    }

    nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-ls-parent]", 16);

    int status = 0;
    (void)nxlibc_waitpid(pid, &status, 0);

    static const char hex[] = "0123456789abcdef";
    char marker[]  = "[bbsh-ls-status=00]";
    int s = status & 0xff;
    marker[16] = hex[(s >> 4) & 0xf];
    marker[17] = hex[s & 0xf];
    nxlibc_write(NXLIBC_STDOUT_FILENO, marker, 19);

    if (status == 0)
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-ls-ok]", 12);
    else
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-ls-failed]", 16);

    nxlibc_exit(0);
}
