/*
 * Slice 7.6c.2 — EL0 C demo exercising libnxlibc's printf + stdio.
 *
 * Calls every supported conversion at least once + nxlibc_atoi.
 * Each printf flushes via nxlibc_write(STDOUT_FILENO, ...), which
 * routes through NX_SYS_DEBUG_WRITE — so the output appears in the
 * live ktest log verbatim.
 *
 * The ktest body asserts the exit code (37) plus the markers
 * `[printf-int=42]`, `[printf-hex=ff]`, `[printf-str=nonux]`,
 * `[printf-pct=%]` are all present in the live UART log.
 *
 * Why exercise every conversion in one program: a future libc swap
 * (slice 7.6c.3 → musl) may quietly break a single conversion if
 * argument promotion or va_list assembly differs.  Catching every
 * conversion here means a single ktest failure pinpoints which
 * formatter regressed.
 */

#include "components/posix_shim/nxlibc.h"

int main(int argc, char **argv, char **envp)
{
    (void)envp;

    /* Each printf produces a marker tagged with its conversion so a
     * UART-log diff can spot a regression in a specific formatter. */
    nxlibc_printf("[printf-int=%d]",       42);
    nxlibc_printf("[printf-uint=%u]",      (unsigned)0xDEADBEEF);
    nxlibc_printf("[printf-hex=%x]",       (unsigned)0xff);
    nxlibc_printf("[printf-HEX=%X]",       (unsigned)0xCAFE);
    nxlibc_printf("[printf-str=%s]",       argv ? argv[0] : "(no-argv)");
    nxlibc_printf("[printf-char=%c]",      'Q');
    nxlibc_printf("[printf-pct=%%]");
    nxlibc_printf("[printf-multi=%d/%s/%x]", argc, argv[0], 0xab);

    /* atoi smoke — convert a literal string and printf the result. */
    int n = nxlibc_atoi("  -23xyz");
    nxlibc_printf("[atoi=%d]", n);

    nxlibc_puts("[printf-puts]");

    nxlibc_exit(37);
}
