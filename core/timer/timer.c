#include "timer.h"
#include "core/irq/irq.h"
#include "core/lib/lib.h"

#define TIMER_PPI 30   /* EL1 physical timer, QEMU virt */

static uint64_t        g_interval;
static _Atomic uint64_t g_ticks;

static inline uint64_t read_cntfrq(void)
{
    uint64_t v;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline void write_cntp_tval(uint64_t v)
{
    asm volatile("msr cntp_tval_el0, %0" :: "r"(v));
}

static inline void write_cntp_ctl(uint64_t v)
{
    asm volatile("msr cntp_ctl_el0, %0" :: "r"(v) : "memory");
}

static void on_tick(void *data)
{
    (void)data;
    /* Rearm the countdown before anything else so we don't drift. */
    write_cntp_tval(g_interval);
    uint64_t n = __atomic_add_fetch(&g_ticks, 1, __ATOMIC_RELAXED);
    /* Bounded serial output — one line per second at 10 Hz. */
    if (n % 10 == 0)
        kprintf("[tick] %lu\n", n);
}

void timer_init(unsigned int hz)
{
    uint64_t freq = read_cntfrq();
    if (hz == 0) hz = 1;
    g_interval = freq / hz;

    irq_register(TIMER_PPI, on_tick, 0);
    gic_enable(TIMER_PPI);

    write_cntp_tval(g_interval);
    write_cntp_ctl(1);          /* enable, unmasked */
    kprintf("[timer] cntfrq=%lu Hz, interval=%lu ticks, rate=%u Hz\n",
            freq, g_interval, hz);
}

uint64_t timer_ticks(void)
{
    return __atomic_load_n(&g_ticks, __ATOMIC_RELAXED);
}
