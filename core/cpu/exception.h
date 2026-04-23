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

/*
 * Drop the current CPU to EL0.  One-way — the kernel context that
 * called this is abandoned (its kernel stack is unreachable until an
 * exception brings us back to EL1).  Callers run in a dedicated
 * kthread whose only job is hosting one EL0 program; slice 5.6+ adds
 * proper task-exit / reap plumbing.
 *
 *   pc       entry PC for the EL0 program (written into ELR_EL1).
 *   sp_el0   initial EL0 stack pointer.
 *
 * Both addresses must lie in pages mapped with AP=EL0-accessible and
 * UXN=0 (for `pc`).  Slice 5.5 designates a single such window — see
 * `mmu_user_window_base` in core/mmu/mmu.h.
 */
void drop_to_el0(uint64_t pc, uint64_t sp_el0) __attribute__((noreturn));

#endif /* NONUX_EXCEPTION_H */
