/*
 * Slice 7.6d.2c — first attempt at exec'ing busybox.
 *
 * Discovery-driven sub-slice: fork + execve("/bin/busybox",
 * { "/bin/busybox", "--help", NULL }, NULL) and see what happens.
 *
 * busybox is a 2.29 MB statically-linked aarch64 ELF (slice 7.6d.1
 * vendored + cross-compiled it; slice 7.6d.2a re-linked it at
 * 0x48000000 to fit our user-window VA).  All preconditions for
 * the exec to even reach `__libc_start_main` are now in place:
 *   - 8 MiB user window (slice 7.6d.2b) holds busybox's ~1.91 MiB
 *     text+data span with room for stack and 1.5 MiB heap
 *   - RAMFS_FILE_CAP = 4 MiB (this slice) holds the binary at
 *     /bin/busybox in initramfs
 *   - SYS_EXEC_MAX_FILE = 4 MiB (this slice) lets sys_exec slurp
 *     the full ELF
 *   - AUXV push (slice 7.6c.3b) feeds AT_PAGESZ / AT_RANDOM to
 *     musl's __init_libc — which busybox is linked against
 *
 * What we DON'T expect: a happy-path `[busybox-help-ok]` marker in
 * the live log.  Most likely failure modes (in order of expected
 * frequency):
 *   1. musl's mallocng calls mmap() for its first slab → ENOSYS
 *      from the syscall-translation layer → mallocng probably
 *      bails out (exit code TBD, depends on musl internals).
 *   2. musl's __init_libc reaches some pthread-init or signal-
 *      mask code path that calls __clone or rt_sigprocmask via
 *      svc 0 with an un-translated x8 → -ENOSYS bubbles up.
 *   3. busybox's main() reads /etc/something or /proc/self/exe
 *      via open() — those paths don't exist in our ramfs.
 *
 * The deliverable is a kernel test that captures whichever
 * trap/exit behaviour we observe, plus a written enumeration of
 * 7.6d.3+ sub-slices in HANDOFF.md based on actual observation
 * rather than the crystal-ball.
 */

#include "components/posix_shim/nxlibc.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    nxlibc_pid_t pid = nxlibc_fork();
    if (pid < 0) {
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[busybox-fork-failed]", 21);
        nxlibc_exit(1);
    }

    if (pid == 0) {
        /* Child branch.  `--help` is a NOFORK applet entry in
         * busybox's applet table — it just prints usage to stdout
         * and exits 0.  No need for the shell, no fork-into-applet
         * dispatch.  If musl + busybox start up at all, we should
         * see a `BusyBox v1.36.1 ...` line on the UART. */
        static char prog[]  = "/bin/busybox";
        static char optarg[] = "--help";
        char *cargv[] = { prog, optarg, 0 };
        nxlibc_execve("/bin/busybox", cargv, 0);

        /* Unreachable on success.  Sentinel for exec-load failure
         * — distinguishes "couldn't even start the binary" (96)
         * from "binary started but exited with N" (any other
         * non-zero code propagated through waitpid). */
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[busybox-exec-failed]", 21);
        nxlibc_exit(96);
    }

    /* Parent.  Presence marker → wait → status marker. */
    nxlibc_write(NXLIBC_STDOUT_FILENO, "[busybox-parent]", 16);

    int status = 0;
    (void)nxlibc_waitpid(pid, &status, 0);

    /* Print the child's exit status as a hex byte so the failure
     * mode is visible in the live ktest log without having to
     * parse a numeric formatter.  We don't have printf in libnxlibc
     * here — just hand-format two hex digits. */
    static const char hex[] = "0123456789abcdef";
    char marker[]  = "[busybox-status=00]";
    int s = status & 0xff;
    marker[16] = hex[(s >> 4) & 0xf];
    marker[17] = hex[s & 0xf];
    nxlibc_write(NXLIBC_STDOUT_FILENO, marker, 19);

    /* Distinguish the fail/success cases for the ktest's parent-
     * level assertion.  Status 0 → exec succeeded + busybox ran
     * to completion (extremely unlikely on first try; would need
     * mmap + clone + a clean exit).  Anything else → discovery
     * captured. */
    if (status == 0)
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[busybox-help-ok]", 17);
    else
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[busybox-help-failed]", 21);

    nxlibc_exit(0);
}
