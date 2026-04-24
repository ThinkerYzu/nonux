/*
 * Slice 7.6 prereq — cross-process pipe demo.
 *
 * Validates the fork handle-inheritance plumbing that lands as a
 * prereq for slice 7.6 (busybox needs `pipe + fork + exec` to
 * actually work).  Flow:
 *
 *   1. parent: posix_pipe(fds) → fds[0] = read, fds[1] = write.
 *   2. parent: posix_fork().
 *   3. child branch (rv == 0):
 *        - posix_close(fds[1])         — drop write side
 *        - posix_read(fds[0], buf, ..) — drains "hello\n"
 *        - if got != 6 or buf != "hello\n" → exit with sentinel
 *        - posix_debug_write("[xpipe-child]", 13)
 *        - posix_exit(41)
 *   4. parent branch (pid > 0):
 *        - posix_close(fds[0])         — drop read side
 *        - posix_write(fds[1], "hello\n", 6)
 *        - posix_close(fds[1])         — sender closes after sending
 *        - posix_waitpid(pid, &status, 0)
 *        - if status == 41 → posix_debug_write("[xpipe-ok]", 10)
 *        - posix_exit(0)
 *
 * Three-marker invariant the ktest pins to:
 *   `[xpipe-parent]` (parent pre-write) →
 *   `[xpipe-child]` (child after read/compare) →
 *   `[xpipe-ok]` (parent after wait + status check).
 *
 * Failure modes the ktest distinguishes via exit code:
 *   1: pipe() failed
 *   2: fork() failed
 *   3: child read returned wrong byte count
 *   4: child read returned wrong bytes
 *   5: parent write failed
 *
 * Per-endpoint refcounts in `framework/channel.c` are what make this
 * work: when fork's handle-table inheritance bumps `handle_refs` on
 * each endpoint, the parent's `close(fds[0])` only decrements the
 * read endpoint's refs from 2 to 1 — the child still has a reference,
 * so the read side stays open until the child closes it (or exits).
 */

#include "components/posix_shim/posix.h"

static int bufeq(const char *a, const char *b, nx_posix_ssize_t n)
{
    for (nx_posix_ssize_t i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

void __attribute__((noreturn)) _start(void)
{
    int fds[2] = { -1, -1 };
    if (nx_posix_pipe(fds) != 0) nx_posix_exit(1);

    nx_posix_pid_t pid = nx_posix_fork();
    if (pid < 0) nx_posix_exit(2);

    if (pid == 0) {
        /* Child: drop the write side, drain the pipe. */
        nx_posix_close(fds[1]);

        char got[8] = { 0 };
        nx_posix_ssize_t n = nx_posix_read(fds[0], got, sizeof got);
        if (n != 6) nx_posix_exit(3);
        const char expect[] = "hello\n";
        if (!bufeq(got, expect, n)) nx_posix_exit(4);

        nx_posix_debug_write("[xpipe-child]", 13);
        nx_posix_close(fds[0]);
        nx_posix_exit(41);
    }

    /* Parent: drop the read side, send the message, close write
     * side, wait for child. */
    nx_posix_close(fds[0]);

    nx_posix_debug_write("[xpipe-parent]", 14);

    const char msg[] = "hello\n";
    nx_posix_ssize_t sent = nx_posix_write(fds[1], msg, sizeof msg - 1);
    if (sent != (nx_posix_ssize_t)(sizeof msg - 1)) nx_posix_exit(5);

    nx_posix_close(fds[1]);

    int status = 0;
    (void)nx_posix_waitpid(pid, &status, 0);

    if (status == 41) {
        nx_posix_debug_write("[xpipe-ok]", 10);
    }

    nx_posix_exit(0);
}
