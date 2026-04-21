#ifndef NONUX_TIMER_H
#define NONUX_TIMER_H

#include <stdint.h>

/*
 * ARM Generic Timer — EL1 physical timer.
 *
 * On QEMU virt the EL1 physical timer fires on PPI 30.  Frequency comes
 * from CNTFRQ_EL0 (typically 62.5 MHz under QEMU).
 */

/* Arm the timer to fire `hz` times per second.  Registers the IRQ handler,
 * enables the IRQ at the GIC, and starts the countdown.  The PSTATE I bit
 * must still be cleared separately once the rest of boot setup is done. */
void timer_init(unsigned int hz);

/* Total tick count since init.  Incremented by the IRQ handler. */
uint64_t timer_ticks(void);

/*
 * Slice 4.4 — recomposition timer quiescence (DESIGN.md §Recomposition
 * Protocol — Timer quiescence).
 *
 * `timer_pause` masks the timer PPI at the GIC; `timer_resume` unmasks
 * it.  The pair is nested via an internal counter so overlapping pause
 * windows compose cleanly — resume only unmasks when the nesting
 * returns to zero.  While paused, no ticks fire (and therefore no
 * `sched_tick` runs), closing the race window during which the
 * recomposition path updates the stashed `g_sched` pointer.
 *
 * Calling `timer_resume` without a matching `timer_pause` is a no-op
 * (saturates at zero rather than going negative).
 */
void timer_pause(void);
void timer_resume(void);

#endif /* NONUX_TIMER_H */
