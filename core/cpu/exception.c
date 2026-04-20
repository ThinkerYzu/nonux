#include "exception.h"
#include "core/lib/lib.h"
#include "core/irq/irq.h"

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

void on_sync(struct trap_frame *tf)
{
    uint64_t esr, far;
    asm volatile("mrs %0, esr_el1" : "=r"(esr));
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
