#include "framework/console.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#if !__STDC_HOSTED__
#include "core/lib/lib.h"   /* uart_putc */
#endif

/*
 * Console implementation — slice 7.6d.N.6b.  See console.h for the
 * contract.
 */

int g_nx_console;   /* sentinel — address used as the handle's object */

/* Test-only call counter, mirroring `g_debug_write_calls` in syscall.c.
 * Incremented on every successful nx_console_write — tests that need
 * to verify "EL0 program reached UART through fd 1/2" watch this
 * value.  Atomic because syscall paths can run on different CPUs in
 * a future SMP build (single-CPU v1 makes the atomic redundant but
 * the project rule is C11 atomics on shared counters from day 1). */
static _Atomic uint64_t g_console_write_calls;

int nx_console_write(const void *buf, size_t len)
{
    if (!buf && len > 0) return -1;  /* caller bug; matches sys_debug_write */
#if __STDC_HOSTED__
    (void)buf;
#else
    const char *s = (const char *)buf;
    for (size_t i = 0; i < len; i++) uart_putc(s[i]);
#endif
    __atomic_fetch_add(&g_console_write_calls, 1, __ATOMIC_RELAXED);
    return (int)len;
}

int nx_console_read(void *buf, size_t cap)
{
    (void)buf; (void)cap;
    /* v1: UART RX not wired — every read reports EOF.  Slice
     * 7.6d.N.final replaces this with a real receive path (IRQ-driven
     * line buffer + wait queue). */
    return 0;
}

uint64_t nx_console_write_calls(void)
{
    return __atomic_load_n(&g_console_write_calls, __ATOMIC_RELAXED);
}

void nx_console_reset_for_test(void)
{
    __atomic_store_n(&g_console_write_calls, 0, __ATOMIC_RELAXED);
}
