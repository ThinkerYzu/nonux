#include "framework/console.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#if !__STDC_HOSTED__
#include "core/lib/lib.h"   /* uart_putc, kprintf */
#include "core/irq/irq.h"
#include "core/sched/sched.h" /* nx_task_yield */
#endif
#include "framework/process.h"
#include "framework/syscall.h"  /* NX_SIGTERM */

/*
 * Console implementation — slice 7.6d.N.6b (write half) + 7.6d.N.final.a
 * (read half + IRQ wiring).  See console.h for the contract.
 */

int g_nx_console;   /* sentinel — address used as the handle's object */

/* Test-only call counter, mirroring `g_debug_write_calls` in syscall.c. */
static _Atomic uint64_t g_console_write_calls;

/*
 * RX ring — single-producer (PL011 RX ISR or test injector), single-
 * consumer (`nx_console_read` from EL0 syscall path).  256 bytes is
 * plenty for one line of typed input; the IRQ runs with IRQs masked
 * at the GIC, so plain head/tail with C11 atomics gives the right
 * ordering on the consumer side.
 *
 * Head is incremented by the producer when a byte is pushed; tail by
 * the consumer when a byte is drained.  Ring is empty when head ==
 * tail and full when (head + 1) % SIZE == tail.
 */
#define RX_RING_SIZE 256

static char            g_rx_buf[RX_RING_SIZE];
static _Atomic size_t  g_rx_head;
static _Atomic size_t  g_rx_tail;

/*
 * Slice 7.6d.N.final.c — control-byte side channels.
 *
 * Ctrl-C (0x03) and Ctrl-D (0x04) bytes are interpreted by the RX
 * path rather than pushed to the ring.  v1 doesn't have a real
 * line-discipline driver (so no ICANON, no IEXTEN, no per-tty
 * disposition table); the two flags below approximate enough of
 * cooked-mode semantics to demo Ctrl-C interrupt + Ctrl-D EOF
 * against busybox.
 *
 *   - `g_eof_pending`: set by the RX path when 0x04 arrives; consumed
 *     by `nx_console_read` once the ring is drained — the next call
 *     returns 0 (POSIX read EOF), then the flag clears so subsequent
 *     reads block normally if more bytes arrive.
 *
 *   - `g_intr_pending`: set by the RX path when 0x03 arrives.
 *     Consumed by `nx_console_drain_intr` from a non-ISR context
 *     (typically `sys_read`'s pre-yield check) which posts SIGTERM
 *     to every ACTIVE non-kernel process via the existing polled
 *     `pending_signals` path (sched_check_resched delivers the
 *     terminate).  v1 has no per-process signal-handler dispatch
 *     yet — full Ctrl-C → SIGINT-with-handler is the deferred
 *     remainder of slice 7.6d.N.final.c.
 */
static _Atomic int     g_eof_pending;
static _Atomic int     g_intr_pending;

static inline size_t rx_count(void)
{
    size_t h = __atomic_load_n(&g_rx_head, __ATOMIC_ACQUIRE);
    size_t t = __atomic_load_n(&g_rx_tail, __ATOMIC_RELAXED);
    return (h - t) & (RX_RING_SIZE - 1);
}

static inline int rx_push_one(char c)
{
    size_t h = __atomic_load_n(&g_rx_head, __ATOMIC_RELAXED);
    size_t t = __atomic_load_n(&g_rx_tail, __ATOMIC_ACQUIRE);
    size_t next = (h + 1) & (RX_RING_SIZE - 1);
    if (next == t) return 0;   /* full — drop */
    g_rx_buf[h] = c;
    __atomic_store_n(&g_rx_head, next, __ATOMIC_RELEASE);
    return 1;
}

static inline int rx_pop_one(char *out)
{
    size_t t = __atomic_load_n(&g_rx_tail, __ATOMIC_RELAXED);
    size_t h = __atomic_load_n(&g_rx_head, __ATOMIC_ACQUIRE);
    if (h == t) return 0;
    *out = g_rx_buf[t];
    __atomic_store_n(&g_rx_tail, (t + 1) & (RX_RING_SIZE - 1),
                     __ATOMIC_RELEASE);
    return 1;
}

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
    if (cap == 0) return 0;
    if (!buf)     return -1;
    char *out = (char *)buf;

#if __STDC_HOSTED__
    /* Host build: no scheduler, no IRQs.  Drain whatever a test pushed
     * via nx_console_test_inject_bytes; if empty, honour any pending
     * EOF (Ctrl-D injection from a host test); else return 0=EOF (the
     * v1 fallback) so existing host tests keep working. */
    size_t got = 0;
    while (got < cap && rx_pop_one(&out[got])) got++;
    if (got == 0 &&
        __atomic_exchange_n(&g_eof_pending, 0, __ATOMIC_ACQ_REL))
        return 0;
    return (int)got;
#else
    /* Yield until at least one byte is available, then drain up to
     * `cap`.  POSIX read semantics: a successful read may return less
     * than `cap` — the shell's line editor will loop.
     *
     * Slice 7.6d.N.final.c: a pending Ctrl-D (0x04) consumed by the
     * RX ISR returns EOF (0) instead of yielding indefinitely.  The
     * flag is one-shot: subsequent reads block again until either
     * fresh bytes or another Ctrl-D arrive. */
    size_t got = 0;
    while (got < cap) {
        char c;
        if (rx_pop_one(&c)) {
            out[got++] = c;
            continue;
        }
        if (got > 0) break;        /* return what we have */
        if (__atomic_exchange_n(&g_eof_pending, 0, __ATOMIC_ACQ_REL))
            return 0;              /* Ctrl-D — POSIX read EOF */
        nx_task_yield();           /* wait for first byte */
    }
    return (int)got;
#endif
}

uint64_t nx_console_write_calls(void)
{
    return __atomic_load_n(&g_console_write_calls, __ATOMIC_RELAXED);
}

void nx_console_reset_for_test(void)
{
    __atomic_store_n(&g_console_write_calls, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_rx_head, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_rx_tail, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_eof_pending, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_intr_pending, 0, __ATOMIC_RELAXED);
}

size_t nx_console_test_inject_bytes(const char *buf, size_t len)
{
    if (!buf) return 0;
    size_t pushed = 0;
    for (size_t i = 0; i < len; i++) {
        if (!rx_push_one(buf[i])) break;
        pushed++;
    }
    return pushed;
}

/*
 * Slice 7.6d.N.final.c — test-only wakers for the control-byte paths.
 * Live RX ISR sets these flags directly; host tests want to set them
 * without simulating the IRQ.  Kept separate from
 * `nx_console_test_inject_bytes` so existing tests can keep pushing
 * the full 0x00..0x7F byte range without accidentally tripping a
 * control event.
 */
void nx_console_test_inject_intr(void)
{
    __atomic_store_n(&g_intr_pending, 1, __ATOMIC_RELEASE);
}

void nx_console_test_inject_eof(void)
{
    __atomic_store_n(&g_eof_pending, 1, __ATOMIC_RELEASE);
}

/*
 * Slice 7.6d.N.final.c — drain a pending Ctrl-C, posting SIGTERM to
 * every ACTIVE non-kernel process so the polled
 * sched_check_resched delivery picks them up next tick.  v1 doesn't
 * have process groups or per-process signal handlers, so a Ctrl-C
 * brings the whole shell session down — a known v1 limitation
 * documented in TESTING-GUIDE.md.  Returns 1 if an interrupt was
 * pending and consumed, 0 otherwise.
 *
 * Called from `sys_read`'s entry path (any read consults this before
 * the byte loop); MUST NOT be called from an ISR because
 * `nx_process_lookup_by_pid` walks the (non-IRQ-safe) process table.
 */
int nx_console_drain_intr(void)
{
    if (!__atomic_exchange_n(&g_intr_pending, 0, __ATOMIC_ACQ_REL))
        return 0;

    /* Walk the live pid range and post SIGTERM to every ACTIVE
     * non-kernel process.  v1 has no group concept; this matches
     * "Ctrl-C kills the whole shell session" behaviour. */
    for (uint32_t pid = 1; pid < 256; pid++) {
        struct nx_process *p = nx_process_lookup_by_pid(pid);
        if (!p) continue;
        if (p == nx_process_lookup_by_pid(0)) continue;   /* never the kernel */
        if (p->state != NX_PROCESS_STATE_ACTIVE) continue;
        __atomic_fetch_or(&p->pending_signals,
                          (uint32_t)(1u << NX_SIGTERM),
                          __ATOMIC_RELEASE);
    }
    return 1;
}

/* ---- PL011 RX wiring (kernel-only) ---------------------------------- */

#if !__STDC_HOSTED__

#define UART_BASE   0x09000000UL
#define UART_DR     0x000   /* Data Register (RW) */
#define UART_FR     0x018   /* Flag Register */
#define UART_IBRD   0x024
#define UART_FBRD   0x028
#define UART_LCRH   0x02C   /* Line Control */
#define UART_CR     0x030   /* Control */
#define UART_IFLS   0x034   /* Interrupt FIFO Level Select */
#define UART_IMSC   0x038   /* Interrupt Mask Set/Clear */
#define UART_ICR    0x044   /* Interrupt Clear Register */

#define UART_FR_RXFE  (1U << 4)   /* RX FIFO empty */
#define UART_IMSC_RXIM (1U << 4)
#define UART_IMSC_RTIM (1U << 6)  /* Receive timeout (FIFO partial) */

/* QEMU virt GIC: PL011 RX is SPI 1 → IRQ 33 (32 + 1). */
#define PL011_IRQ 33

static volatile uint8_t *const uart = (volatile uint8_t *)UART_BASE;

static inline uint32_t uart_rd(uintptr_t off)
{
    return *(volatile uint32_t *)(uart + off);
}
static inline void uart_wr(uintptr_t off, uint32_t v)
{
    *(volatile uint32_t *)(uart + off) = v;
}

static void nx_console_rx_isr(void *data)
{
    (void)data;
    /* Drain everything in the FIFO so we don't take a second IRQ for
     * bytes already buffered in hardware.  Slice 7.6d.N.final.c:
     * Ctrl-C (0x03) and Ctrl-D (0x04) are interpreted as control
     * events instead of pushed into the byte ring — see
     * `g_eof_pending` / `g_intr_pending` for what each one triggers. */
    while (!(uart_rd(UART_FR) & UART_FR_RXFE)) {
        char c = (char)(uart_rd(UART_DR) & 0xFF);
        if (c == 0x03) {
            __atomic_store_n(&g_intr_pending, 1, __ATOMIC_RELEASE);
            continue;
        }
        if (c == 0x04) {
            __atomic_store_n(&g_eof_pending, 1, __ATOMIC_RELEASE);
            continue;
        }
        rx_push_one(c);
    }
    /* Clear receive + receive-timeout interrupts.  Leave the rest
     * masked so we don't touch TX state. */
    uart_wr(UART_ICR, UART_IMSC_RXIM | UART_IMSC_RTIM);
}

void nx_console_init(void)
{
    /* QEMU's PL011 model accepts RX without explicit FIFO config —
     * but we still set IFLS to "1/8 full" (interrupt on first byte)
     * for parity with real hardware.  The IFLS RX field is bits 5:3;
     * 000 = 1/8 full. */
    uart_wr(UART_IFLS, 0);

    /* Unmask receive + receive-timeout (the timeout fires when the
     * FIFO has held some bytes but never crossed the threshold —
     * matters for slow typists). */
    uart_wr(UART_IMSC, UART_IMSC_RXIM | UART_IMSC_RTIM);

    if (irq_register(PL011_IRQ, nx_console_rx_isr, 0) != 0) {
        kprintf("[console] irq_register(%u) failed — RX disabled\n",
                (unsigned)PL011_IRQ);
        return;
    }
    gic_enable(PL011_IRQ);
    kprintf("[console] PL011 RX wired (irq=%u)\n", (unsigned)PL011_IRQ);
}

#else /* host build */

void nx_console_init(void)
{
    /* No UART on host; RX ring is driven by test injection only. */
}

#endif
