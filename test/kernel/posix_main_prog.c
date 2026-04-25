/*
 * Slice 7.6c.0 — EL0 C demo using POSIX `int main(int argc, char
 * **argv)` entry instead of `_start`.
 *
 * Linked with `components/posix_shim/crt0.S` whose `_start` synthesises
 * `argc = 1, argv = { "nonux", NULL }, envp = NULL` and calls
 * `main(argc, argv, envp)`, then `exit(main_rv)`.  Demo:
 *
 *   1. nx_strlen + nx_memcpy a marker into a stack buffer.
 *   2. nx_posix_debug_write the buffer.
 *   3. nx_strcmp argv[0] against "nonux"; mismatch → exit(2).
 *   4. return argc+46 — crt0 calls exit(47).
 *
 * Verifies the C-runtime bootstrap end-to-end:
 *   - crt0 invoked main with argc=1, argv[0] valid string.
 *   - main's return value reached nx_posix_exit through crt0.
 *   - the static-inline libc helpers (strlen / memcpy / strcmp)
 *     compile + link cleanly under -ffreestanding -nostdlib.
 *
 * If any of those drift, the ktest's exit-code assertion (47) and
 * marker check fail loudly.
 */

#include "components/posix_shim/posix.h"

int main(int argc, char **argv, char **envp)
{
    (void)envp;

    static const char marker[] = "[main-ok]";
    /* Round-trip through the libc helpers so they're exercised, not
     * just declared.  Strings are tiny — no overflow risk. */
    char buf[16];
    size_t n = nx_strlen(marker);
    nx_memcpy(buf, marker, n);
    nx_posix_debug_write(buf, (size_t)n);

    if (argc != 1) return 1;
    if (!argv || !argv[0]) return 2;
    if (nx_strcmp(argv[0], "nonux") != 0) return 3;

    return argc + 46;   /* crt0 passes this to exit() — 47 expected */
}
