#ifndef NX_FRAMEWORK_CONSOLE_H
#define NX_FRAMEWORK_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Console primitive — slice 7.6d.N.6b.
 *
 * Replaces the magic-fd hack in `sys_read` / `sys_write` (where fd
 * 0/1/2 with no allocated handle fell through to debug_write / EOF)
 * with a proper handle-backed console object.
 *
 * Each process gets three pre-installed handles at table slots 0/1/2
 * pointing at the singleton console object — see `nx_process_create`.
 * The mapping is:
 *
 *     POSIX fd       slot   encoded handle   rights
 *     ----------     ----   --------------   ----------
 *     STDOUT_FILENO  0      1                NX_RIGHT_WRITE
 *     STDERR_FILENO  1      2                NX_RIGHT_WRITE
 *     STDIN_FILENO   2      (3 normally,     NX_RIGHT_READ
 *                            but POSIX = 0 —
 *                            see h==0 routing
 *                            in sys_read /
 *                            sys_handle_close
 *                            / sys_dup3)
 *
 * Slot 2 is reached via a `h == 0` special case in the syscall layer
 * because encoded value 0 is reserved for `NX_HANDLE_INVALID` — the
 * caller's `read(0, ...)` can't be matched by `nx_handle_lookup`
 * without that route.
 *
 * The "object" stored in the handle entry is the address of
 * `g_nx_console` — a sentinel marker.  All instances share the same
 * underlying device (the PL011 UART) so we don't need per-handle
 * state; the handle's type tag is enough to dispatch.
 *
 * UART RX is not wired in v1 — `nx_console_read` returns 0 = EOF
 * unconditionally.  Interactive shells (slice 7.6d.N.final) need a
 * real RX path: a UART RX IRQ + a per-process line buffer.  For now
 * any read on the STDIN console produces EOF, which is what musl's
 * `__stdio_read` interprets as "input closed" — fine for the
 * non-interactive `sh -c "..."` workloads we drive in 7.6d.N.*.
 */

/* Sentinel object — every CONSOLE handle's `object` field points here.
 * The address is what matters; the struct's contents are unused. */
extern int g_nx_console;

/*
 * Write `len` bytes from `buf` to the console (kernel-visible buffer
 * — caller has already done copy_from_user if needed).  Returns
 * `len` on success; partial writes don't happen because the PL011
 * write is unconditional.
 *
 * Host build: no-op success — there's no UART, but tests that exercise
 * the dispatch path still want a positive return value.
 */
int nx_console_write(const void *buf, size_t len);

/*
 * Read up to `cap` bytes into `buf` from the console.  v1 returns 0 =
 * EOF unconditionally (UART RX not wired).  When RX lands in slice
 * 7.6d.N.final this grows a wait-for-line / non-blocking-poll mode;
 * for now the contract is "always EOF, never blocks".
 */
int nx_console_read(void *buf, size_t cap);

/*
 * Test-only: how many times nx_console_write has been successfully
 * invoked since boot (or since the last `nx_console_reset_for_test`).
 * Mirrors `nx_syscall_debug_write_calls` — kernel tests that need to
 * verify "EL0 program reached the UART through stdio fd 1/2" can
 * watch this counter instead of (or in addition to) the debug_write
 * counter, because slice 7.6d.N.6b routes write(1, ...) through
 * console handles rather than the magic-fd-debug_write fallback.
 */
uint64_t nx_console_write_calls(void);
void     nx_console_reset_for_test(void);

#endif /* NX_FRAMEWORK_CONSOLE_H */
