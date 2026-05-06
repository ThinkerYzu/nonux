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
 * UART RX wiring landed in slice 7.6d.N.final.a:
 *   - `nx_console_init` registers the PL011 RX IRQ handler (QEMU virt
 *     SPI 1 = IRQ 33) and enables the UART's RX FIFO + RX-IRQ.
 *   - The RX ISR (`nx_console_rx_isr`) drains the UART FIFO into a
 *     fixed-size single-producer/single-consumer byte ring.
 *   - `nx_console_read` polls the ring, yielding the CPU until at
 *     least one byte is available, then drains up to `cap` bytes.
 *
 * No line discipline in v1 — bytes are delivered raw.  busybox's ash
 * runs cmdedit in raw mode (after `tcsetattr` succeeds via the
 * sys_ioctl stub), so it does its own line editing.
 *
 * Host build keeps the v1 EOF behaviour (no UART, no IRQs); host
 * tests can use `nx_console_test_inject_bytes` to push bytes into
 * the ring directly, bypassing the IRQ.
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
 * Read up to `cap` bytes into `buf` from the console.  Blocks (via
 * `nx_task_yield`) until at least one byte is available in the RX
 * ring, then drains up to `cap` bytes and returns the count.  Never
 * returns 0 from a non-zero `cap` request after RX is wired —
 * "no input yet" is a yield, not an EOF.
 *
 * Host build keeps the v1 EOF behaviour (returns 0) unless a host
 * test has primed the ring via `nx_console_test_inject_bytes`.
 */
int nx_console_read(void *buf, size_t cap);

/*
 * One-time init — called from `boot_main` after `gic_init` and before
 * `irq_enable_local`.  Registers the PL011 RX IRQ handler with the
 * core IRQ table, enables IRQ 33 at the GIC, and toggles the UART's
 * RXIM mask + RX FIFO threshold so the device interrupts on each
 * incoming byte.  Host build is a no-op.
 */
void nx_console_init(void);

/*
 * Test-only: push bytes directly into the RX ring, bypassing the
 * IRQ.  Used by ktests that want to drive the read path without
 * needing real UART input.  Drops bytes silently if the ring is full
 * (matches the IRQ behaviour).  Returns the number of bytes pushed.
 *
 * Pushes raw bytes — does NOT apply the live RX ISR's control-byte
 * interpretation (Ctrl-C / Ctrl-D become flags, not ring bytes).
 * Tests that want to drive the control-byte paths use
 * `nx_console_test_inject_intr` / `_eof` directly.
 */
size_t nx_console_test_inject_bytes(const char *buf, size_t len);

/*
 * Test-only: arm the Ctrl-C / Ctrl-D control-byte side channels.
 * Kept separate from `_inject_bytes` so the byte-range tests can
 * keep injecting the full 0x00..0x7F space without accidentally
 * tripping a control event.
 */
void nx_console_test_inject_intr(void);
void nx_console_test_inject_eof(void);

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

/*
 * Slice 7.6d.N.final.c — drain a pending Ctrl-C, posting SIGTERM to
 * every ACTIVE non-kernel process.  Returns 1 if an interrupt was
 * consumed, 0 otherwise.  Called from `sys_read` so that read entry
 * is the ack point for input-side control bytes.  MUST NOT be called
 * from an ISR (walks the process table).
 */
int nx_console_drain_intr(void);

/*
 * Slice 7.8b — pollset integration.
 *
 *   `nx_console_register_pollset(listener)` adds a borrowed listener
 *   to the singleton console's RX-readiness watcher list.  After
 *   registration the RX ISR (or `_inject_bytes` from a host test, or
 *   `_inject_eof`) wakes every listener's parent waitq when a byte
 *   becomes available in the ring (or EOF arms).
 *
 *   `nx_console_unregister_pollset(listener)` unlinks the listener.
 *
 *   `nx_console_readiness(want)` computes POLLIN if the RX ring is
 *   non-empty OR a Ctrl-D EOF is queued; POLLOUT is always set
 *   (writes are unconditional).
 */
struct nx_pollset_listener;

void  nx_console_register_pollset(struct nx_pollset_listener *l);
void  nx_console_unregister_pollset(struct nx_pollset_listener *l);
short nx_console_readiness(short want);

#if !__STDC_HOSTED__
/*
 * Non-blocking read for dispatcher-context handlers.  Returns > 0
 * (bytes), 0 (EOF / Ctrl-D), or NX_EAGAIN (ring empty, no EOF).
 * sys_read wraps nx_char_device_read (which routes here) in a pollset
 * retry loop so EL0 stdin blocking happens in the caller's task context.
 */
int nx_console_read_nonblocking(void *buf, size_t cap);

/* Predicate for nx_waitq_wait_unless: non-zero when the ring is
 * non-empty or EOF is queued (i.e. nx_console_read_nonblocking would
 * not return NX_EAGAIN). */
int nx_console_read_ready(void *ctx);
#endif

#endif /* NX_FRAMEWORK_CONSOLE_H */
