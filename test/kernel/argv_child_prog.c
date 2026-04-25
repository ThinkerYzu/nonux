/*
 * Slice 7.6c.4 — exec'd child program for the argv-push round-trip.
 *
 * Loaded into ramfs as `/argv_child` (alongside `/init` and
 * `/banner` from slice 7.6b's initramfs).  The argv-parent program
 * fork()s and the child execve()s us with custom argv.  We use the
 * crt0-supplied argc/argv (slice 7.6c.4 reads them off the user
 * stack that sys_exec pushed) and emit markers tagged with the
 * received values, then exit with `argc` so the ktest can
 * cross-check.
 *
 * Expected argv from the parent: { "/argv_child", "hello", "world",
 * NULL } → argc == 3.  Demo asserts each slot byte-for-byte; any
 * mismatch exits with a sentinel code that pinpoints the failed
 * check, so the ktest can tell apart "argv didn't reach the
 * child" / "wrong slot value" / "right argv but wrong argc".
 */

#include "components/posix_shim/nxlibc.h"

int main(int argc, char **argv, char **envp)
{
    (void)envp;

    /* Print argc + each argv slot as a tagged marker.  printf
     * builds the formatted string in a stack buffer + writes once,
     * so each call is a single debug_write. */
    nxlibc_printf("[argv-child argc=%d]", argc);
    for (int i = 0; i < argc && i < 8; i++) {
        nxlibc_printf("[argv-child argv[%d]=%s]", i,
                      argv[i] ? argv[i] : "(null)");
    }

    /* Validate the expected layout — the parent passes
     * { "/argv_child", "hello", "world", NULL }.  Mismatch exits
     * with a sentinel so the ktest can pinpoint where the round
     * trip broke. */
    if (argc != 3)                                     return 81;
    if (!argv || !argv[0] ||
        nxlibc_strcmp(argv[0], "/argv_child") != 0)    return 82;
    if (!argv[1] || nxlibc_strcmp(argv[1], "hello") != 0) return 83;
    if (!argv[2] || nxlibc_strcmp(argv[2], "world") != 0) return 84;
    if (argv[3] != 0)                                  return 85;

    nxlibc_printf("[argv-child-ok]");
    nxlibc_exit(argc + 60);   /* 3 + 60 = 63 — what the ktest pins */
}
