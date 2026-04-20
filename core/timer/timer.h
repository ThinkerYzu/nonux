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

#endif /* NONUX_TIMER_H */
