#ifndef NONUX_EXCEPTION_H
#define NONUX_EXCEPTION_H

#include <stdint.h>

struct trap_frame {
    uint64_t x[31];      /* x0..x30 */
    uint64_t sp_el0;
    uint64_t pc;         /* ELR_EL1 at time of exception */
    uint64_t pstate;     /* SPSR_EL1 */
};

/* Install the vector table into VBAR_EL1.  Call once at boot, before
 * unmasking interrupts. */
void vectors_install(void);

/* Unmask / mask IRQ on the current CPU (DAIF I bit). */
static inline void irq_enable_local(void)  { asm volatile("msr daifclr, #2" ::: "memory"); }
static inline void irq_disable_local(void) { asm volatile("msr daifset, #2" ::: "memory"); }

#endif /* NONUX_EXCEPTION_H */
