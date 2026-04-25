/*
 * Slice 7.6c.3b — first EL0 demo against musl's libc.a.
 *
 * Validates that:
 *   - musl's crt1.o + libc.a + crti.o/crtn.o link cleanly with our
 *     freestanding aarch64-linux-gnu-gcc toolchain.
 *   - musl's `_start` -> `_start_c` -> `__libc_start_main` ->
 *     `__init_libc` -> `__init_tls` -> `__init_ssp` -> stage2 ->
 *     `main` flow runs to completion.
 *   - `write(STDOUT_FILENO, ...)` lands in our kernel via the
 *     translated `__NR_write` (64) -> `NX_SYS_WRITE` (8).
 *   - `_exit(rv)` lands in `_Exit` -> `exit_group` (94) ->
 *     `NX_SYS_EXIT` (11), with the exit_code making it back through
 *     the process table.
 *
 * The matching ktest exits with code 57 (deliberately distinct from
 * libnxlibc's posix_libc_prog at 53 + crt0's posix_main_prog at 47).
 *
 * Headers are declared inline because <unistd.h> in our build pulls
 * in much more than this demo needs (POSIX_C_SOURCE feature gating,
 * stat/select/etc.).  The declarations below match musl's signatures.
 */

typedef long          ssize_t;
typedef unsigned long size_t;

extern ssize_t write(int fd, const void *buf, size_t n);
extern void    _exit(int status) __attribute__((noreturn));

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    write(1, "[musl-ok]", 9);
    _exit(57);
}
