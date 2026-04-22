#include "exception.h"
#include "core/lib/lib.h"
#include "core/irq/irq.h"
#include "framework/syscall.h"

extern char vectors[];

void vectors_install(void)
{
    asm volatile("msr vbar_el1, %0" :: "r"((uint64_t)vectors) : "memory");
    asm volatile("isb" ::: "memory");
}

static void halt_forever(void)
{
    for (;;) asm volatile("wfe");
}

/* ESR_EL1.EC = 0x15: SVC instruction execution in AArch64 state.
 * Same value whether SVC came from EL0 or EL1 — the vector-table entry
 * already told us which EL; we only use EC here to distinguish SVC
 * from other synchronous exceptions (data/instruction aborts etc). */
#define ESR_EC_SVC64  0x15u

void on_sync(struct trap_frame *tf)
{
    uint64_t esr, far;
    asm volatile("mrs %0, esr_el1" : "=r"(esr));
    uint32_t ec = (uint32_t)((esr >> 26) & 0x3fu);

    if (ec == ESR_EC_SVC64) {
        nx_syscall_dispatch(tf);
        return;
    }

    asm volatile("mrs %0, far_el1" : "=r"(far));
    kprintf("\n[EXC] sync  ESR=%lx FAR=%lx ELR=%lx SPSR=%lx\n",
            esr, far, tf->pc, tf->pstate);
    halt_forever();
}

void on_irq(struct trap_frame *tf)
{
    (void)tf;
    irq_dispatch();
}

void on_fiq(struct trap_frame *tf)
{
    (void)tf;
    kprintf("\n[EXC] unexpected FIQ\n");
    halt_forever();
}

void on_serror(struct trap_frame *tf)
{
    (void)tf;
    kprintf("\n[EXC] SError\n");
    halt_forever();
}

void on_unimpl(struct trap_frame *tf)
{
    kprintf("\n[EXC] unimplemented vector ELR=%lx SPSR=%lx\n",
            tf->pc, tf->pstate);
    halt_forever();
}
