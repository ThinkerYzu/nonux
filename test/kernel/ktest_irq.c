#include "ktest.h"
#include "core/timer/timer.h"
#include "core/cpu/exception.h"

/*
 * IRQ-path smoke tests.  By the time ktest_main runs, the timer is armed
 * at 10 Hz with IRQs unmasked — so we just need to confirm that ticks
 * actually advance, both under a busy loop and under WFI.
 */

static void busy_wait_ms(unsigned ms)
{
    /* cntfrq is 62.5 MHz on QEMU virt → 62500 ticks per ms. */
    uint64_t freq;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    uint64_t deadline, now;
    asm volatile("mrs %0, cntpct_el0" : "=r"(now));
    deadline = now + (freq / 1000) * ms;
    do {
        asm volatile("mrs %0, cntpct_el0" : "=r"(now));
    } while (now < deadline);
}

KTEST(irq_timer_ticks_advance_under_busy_wait)
{
    uint64_t before = timer_ticks();
    busy_wait_ms(250);  /* 2–3 ticks at 10 Hz */
    uint64_t after = timer_ticks();
    if (after <= before) {
        kprintf("    before=%lu after=%lu\n", before, after);
        ktest_fail(__FILE__, __LINE__, "timer_ticks did not advance");
        return;
    }
}

KTEST(irq_wfi_wakes_on_timer)
{
    /* One wfi should wake on the very next timer IRQ.  If the IRQ path
     * is broken the CPU parks here and the test binary hangs; rely on
     * the QEMU-level timeout wrapping test-kernel to flag that. */
    uint64_t before = timer_ticks();
    asm volatile("wfi");
    uint64_t after = timer_ticks();
    KASSERT(after > before);
}
