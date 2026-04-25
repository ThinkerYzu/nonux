/*
 * Slice 7.6c.1 — EL0 C demo using POSIX-named libc symbols.
 *
 * Linked against `libnxlibc.a` instead of just including
 * `posix.h` static inlines.  Same `int main()` form as
 * `posix_main_prog.c` (slice 7.6c.0) but uses the POSIX-named
 * `write` / `_exit` / `strlen` / `memcpy` / `strcmp` instead of
 * `nx_posix_debug_write` / `nx_strlen` / etc.
 *
 * Why this matters: when slice 7.6c.2 swaps in real musl, busybox
 * + any other libc-using code calls `write(1, ...)` and `exit(rv)`
 * — exactly what this demo does.  Confirming the names work
 * end-to-end via libnxlibc means the eventual musl swap is a
 * link-line change, not an API change.
 *
 *   1. nxlibc_strlen + nxlibc_memcpy a marker into a stack buffer.
 *   2. nxlibc_write(STDOUT_FILENO, buf, n) — routes through
 *      nxlibc.c's stdout-specific branch into NX_SYS_DEBUG_WRITE.
 *   3. nxlibc_strcmp argv[0] against "nonux".
 *   4. nxlibc_exit(53) — matches the ktest's expected exit code.
 */

#include "components/posix_shim/nxlibc.h"

int main(int argc, char **argv, char **envp)
{
    (void)envp;

    static const char marker[] = "[libc-ok]";
    char buf[16];
    size_t n = nxlibc_strlen(marker);
    nxlibc_memcpy(buf, marker, n);
    nxlibc_write(NXLIBC_STDOUT_FILENO, buf, n);

    if (argc != 1) nxlibc_exit(1);
    if (!argv || !argv[0]) nxlibc_exit(2);
    if (nxlibc_strcmp(argv[0], "nonux") != 0) nxlibc_exit(3);

    nxlibc_exit(53);
}
