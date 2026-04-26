/*
 * Slice 7.6d.N.0 — first attempt at exec'ing busybox AS A SHELL.
 *
 * Slice 7.6d.3c got busybox `--help` running end-to-end (NOFORK
 * applet entry: just prints usage to stdout and exits 0; no
 * shell, no fork-into-applet dispatch, minimal startup code).
 * This sub-slice takes the next discovery-driven step toward
 * the slice-7.6d.N goal of `/init = busybox sh`.
 *
 * Child does:
 *   execve("/bin/busybox", { "sh", "-c", "exit 42", NULL }, NULL)
 *
 * Why this argv shape:
 *
 *   - argv[0] = "sh" (NOT "/bin/busybox") — busybox dispatches by
 *     `basename(argv[0])`, so this routes into the `sh` applet
 *     (= ash, per CONFIG_SH_IS_ASH=y in nonux_defconfig) instead
 *     of busybox's own `--help` path.
 *
 *   - "-c \"exit 42\"" — one-shot, non-interactive shell run.
 *     Avoids ash's interactive setup (TTY mode set, line-editing
 *     init, prompt expansion, history file open).  No stdin
 *     reads, no fork-into-applet dispatch.  `exit` is a shell
 *     builtin so the script doesn't need execvp() either.  The
 *     exit code 42 is an arbitrary "shell parsed + ran the
 *     -c argument" sentinel — any of busybox's internal exit
 *     codes (1 syntax error, 127 command-not-found, 2 misuse)
 *     would mean the shell got further than we expect at this
 *     point.
 *
 * What we DON'T expect on first run: a clean status=42.  Most
 * likely failure modes (in approximate order of expected
 * frequency):
 *
 *   1. ash's startup calls `sigaction()` (always, even non-
 *      interactive) — `__NR_rt_sigaction` (134) is unmapped in
 *      our musl-syscall-arch translation table, returns -ENOSYS.
 *      ash treats this as fatal.
 *   2. ash's startup calls `getuid()` / `geteuid()` to decide
 *      `# vs $` prompt + suid handling — both unmapped → -ENOSYS,
 *      may or may not be fatal.
 *   3. ash uses `mmap()` somewhere (for arena allocation, even
 *      if musl's mallocng can fall back to brk).  `__NR_mmap`
 *      (222) is unmapped → -ENOSYS.
 *   4. ash references files we don't have: `/etc/passwd` for
 *      uid lookup, `/dev/tty` for the controlling terminal,
 *      `/etc/profile` if interactive (we won't be).
 *   5. ash reads `argv[0]` as the program name and looks up
 *      its applet — if our argv push doesn't carry through
 *      from sys_exec correctly, busybox falls through to
 *      "applet not found" + exits 127.
 *
 * Discovery-driven: the deliverable is captured failure mode +
 * a written enumeration of 7.6d.N.x sub-slices in HANDOFF.md
 * based on observation, not crystal-ball prediction.  This
 * test asserts only that the parent reaches its post-wait
 * status print; the actual child status is captured in the
 * `[bbsh-status=NN]` marker and triaged in the session log.
 */

#include "components/posix_shim/nxlibc.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    nxlibc_pid_t pid = nxlibc_fork();
    if (pid < 0) {
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-fork-failed]", 18);
        nxlibc_exit(1);
    }

    if (pid == 0) {
        /* argv[0] = "sh" so busybox dispatches to the ash applet
         * via basename(argv[0]).  No leading "/bin/" needed; the
         * applet table is keyed on the basename only. */
        static char a0[] = "sh";
        static char a1[] = "-c";
        static char a2[] = "exit 42";
        char *cargv[] = { a0, a1, a2, 0 };
        nxlibc_execve("/bin/busybox", cargv, 0);

        /* Unreachable on success.  Distinguishes "couldn't even
         * load the binary" (97) from "binary loaded but failed
         * inside its own startup path" (any other non-zero code
         * propagated through waitpid). */
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-exec-failed]", 18);
        nxlibc_exit(97);
    }

    /* Parent.  Presence marker → wait → status marker. */
    nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-parent]", 13);

    int status = 0;
    (void)nxlibc_waitpid(pid, &status, 0);

    /* Print the child's exit status as a hex byte so the failure
     * mode is visible in the live ktest log without a numeric
     * formatter.  See posix_busybox_help_prog.c for the same
     * pattern.  Status 0x2a (42) → shell ran the -c argument
     * and reached `exit 42`.  Status 0x8b (139 = 128+11) →
     * SIGSEGV during ash startup.  Status 0x84 (132 = 128+4) →
     * SIGILL.  Status 0x7f (127) → ash exited "command not
     * found" (means the shell started but couldn't find an
     * applet).  Status 0x01 → ash exited "syntax error" or
     * generic failure during -c parsing. */
    static const char hex[] = "0123456789abcdef";
    char marker[]  = "[bbsh-status=00]";
    int s = status & 0xff;
    marker[13] = hex[(s >> 4) & 0xf];
    marker[14] = hex[s & 0xf];
    nxlibc_write(NXLIBC_STDOUT_FILENO, marker, 16);

    /* Distinguish the success/fail cases for the session log
     * write-up.  Status 42 → ash got all the way through its
     * startup + -c parsing + exit handling.  Anything else →
     * captured failure mode for the next sub-slice plan. */
    if (status == 42)
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-ok]", 9);
    else
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-failed]", 13);

    nxlibc_exit(0);
}
