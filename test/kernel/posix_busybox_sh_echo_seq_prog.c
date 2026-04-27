/*
 * Slice 7.6d.N.3 — busybox `sh -c "echo a; echo b"` discovery.
 *
 * Slice 7.6d.N.2 (session 57) closed the single-statement
 * gap: `sh -c "echo hello"` parses + dispatches the `echo`
 * builtin, writes `hello\n` via musl stdio + magic-fd-handle,
 * runs end-of-input shutdown, exits 0.
 *
 * This sub-slice escalates one rung further to the smallest
 * MULTI-statement script: `echo a; echo b`.  Differences from
 * the single-statement path:
 *
 *   - ash's parser has to build a list (or sequence) node with
 *     two child commands, not just a single command tree.
 *     Pulls more from mallocng — the parse exercises whatever
 *     code path handles the `;` separator.
 *   - The shell's main eval loop has to actually iterate
 *     across statements: dispatch `echo a`, return, see the
 *     next statement, dispatch `echo b`, then end-of-input.
 *     Slice 7.6d.N.2 only ran one statement.
 *   - Two consecutive stdio writes (one per `echo`).  Confirms
 *     stdio buffering across calls (musl's line-buffered or
 *     fully-buffered behaviour with stdout-not-a-tty after our
 *     magic-fd-handle accepts everything).
 *   - Total output is `a\nb\n` instead of `hello\n` — six
 *     bytes split across two writev calls (or possibly
 *     coalesced by stdio buffering into one).
 *
 * Possible failure modes if anything new surfaces:
 *
 *   1. Parser path for `;` separator pulls a syscall we don't
 *      have — e.g. `clock_gettime` for some debug timestamp,
 *      or a deeper signal-handling primitive.  Plausible but
 *      unlikely for a non-interactive shell.
 *   2. Stdio flush ordering: if musl's stdout is fully
 *      buffered and only flushes on shutdown, both `echo`
 *      outputs land in one writev at exit — different byte
 *      sequence in the log.  Either way ash exits 0.
 *   3. mallocng arena exhaustion: the parser node count
 *      doubles vs. `echo hello`; if total exceeds 3 MiB
 *      we'll early-OOM.  Very unlikely for one short script.
 *
 * Discovery-only: no kernel changes anticipated.  The test
 * asserts only parent-side liveness; the actual child exit
 * + the `a\nb\n` (or coalesced) output between markers tells
 * us what happened.
 *
 * Status decode for the captured marker:
 *   0x00 — `echo a; echo b` ran, ash exited 0.  Success.
 *   0x01 — ash exited 1 (generic failure — could be OOM or
 *          a parser bail).
 *   0x02 — ash exited 2 (parse error — the most interesting
 *          early bail mode if the `;` separator surprises
 *          ash somehow).
 *   0x8b — SIGSEGV (would imply a kernel-side gap).
 *   0x84 — SIGILL.
 */

#include "components/posix_shim/nxlibc.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    nxlibc_pid_t pid = nxlibc_fork();
    if (pid < 0) {
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-seq-fork-failed]", 22);
        nxlibc_exit(1);
    }

    if (pid == 0) {
        static char a0[] = "sh";
        static char a1[] = "-c";
        static char a2[] = "echo a; echo b";
        char *cargv[] = { a0, a1, a2, 0 };
        nxlibc_execve("/bin/busybox", cargv, 0);

        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-seq-exec-failed]", 22);
        nxlibc_exit(97);
    }

    nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-seq-parent]", 17);

    int status = 0;
    (void)nxlibc_waitpid(pid, &status, 0);

    static const char hex[] = "0123456789abcdef";
    char marker[]  = "[bbsh-seq-status=00]";
    int s = status & 0xff;
    marker[17] = hex[(s >> 4) & 0xf];
    marker[18] = hex[s & 0xf];
    nxlibc_write(NXLIBC_STDOUT_FILENO, marker, 20);

    if (status == 0)
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-seq-ok]", 13);
    else
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[bbsh-seq-failed]", 17);

    nxlibc_exit(0);
}
