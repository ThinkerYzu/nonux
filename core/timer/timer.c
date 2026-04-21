#include "timer.h"
#include "core/irq/irq.h"
#include "core/lib/lib.h"
#include "core/sched/sched.h"

#define TIMER_PPI 30   /* EL1 physical timer, QEMU virt */

static uint64_t         g_interval;
static _Atomic uint64_t g_ticks;
static _Atomic unsigned g_pause_nest;

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
    __atomic_add_fetch(&g_ticks, 1, __ATOMIC_RELAXED);
    /* Drive the scheduler.  No-op until sched_init has stashed a
     * policy pointer (early boot, or NX_KTEST builds that exit
     * before calling sched_init). */
    sched_tick();
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

void timer_pause(void)
{
    /* First entry masks the PPI; nested entries just bump the count. */
    unsigned prev = __atomic_fetch_add(&g_pause_nest, 1, __ATOMIC_ACQ_REL);
    if (prev == 0)
        gic_disable(TIMER_PPI);
}

void timer_resume(void)
{
    /* Saturate at zero rather than underflow — calling resume without
     * a matching pause is a no-op, not undefined. */
    unsigned cur;
    do {
        cur = __atomic_load_n(&g_pause_nest, __ATOMIC_RELAXED);
        if (cur == 0) return;
    } while (!__atomic_compare_exchange_n(&g_pause_nest, &cur, cur - 1,
                                          true, __ATOMIC_ACQ_REL,
                                          __ATOMIC_RELAXED));
    if (cur == 1)
        gic_enable(TIMER_PPI);
}
