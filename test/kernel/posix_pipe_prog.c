/*
 * Slice 7.5 — EL0 C demo for NX_SYS_PIPE.
 *
 * Single-process round-trip: create a pipe, write "hello\n" into
 * the write end, read it back from the read end, compare bytes,
 * emit `[pipe-ok]` + `exit(29)`.
 *
 * Single-process (not cross-process) because slice 7.4a deferred
 * handle-table duplication at fork: the child inherits an empty
 * table, so a parent + forked child can't share pipe fds without
 * the framework plumbing that a later slice will add.  This demo
 * therefore proves the *dispatch mechanics* — NX_SYS_PIPE
 * allocates two channel-backed handles with asymmetric rights;
 * `nx_posix_read` / `nx_posix_write` type-dispatch correctly
 * through sys_read / sys_write's CHANNEL branch — without
 * taking a dependency on fork-inheritance.  Real pipe semantics
 * across processes lands with slice 7.6's busybox integration
 * (echo | cat) or a dedicated fork-inherit slice before that.
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

    const char msg[] = "hello\n";
    nx_posix_ssize_t sent = nx_posix_write(fds[1], msg, sizeof msg - 1);
    if (sent != (nx_posix_ssize_t)(sizeof msg - 1)) nx_posix_exit(2);

    char got[8] = { 0 };
    nx_posix_ssize_t n = nx_posix_read(fds[0], got, sizeof got);
    if (n != (nx_posix_ssize_t)(sizeof msg - 1)) nx_posix_exit(3);
    if (!bufeq(got, msg, n)) nx_posix_exit(4);

    nx_posix_debug_write("[pipe-ok]", 9);
    nx_posix_close(fds[0]);
    nx_posix_close(fds[1]);
    nx_posix_exit(29);
}
