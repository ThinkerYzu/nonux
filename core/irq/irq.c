#include "irq.h"
#include "core/lib/lib.h"

struct irq_entry {
    irq_handler_t fn;
    void         *data;
};

static struct irq_entry g_table[IRQ_TABLE_SIZE];

/* Forward decls from gic.c */
unsigned int gic_ack(void);
void         gic_eoi(unsigned int irq);

int irq_register(unsigned int irq, irq_handler_t fn, void *data)
{
    if (irq >= IRQ_TABLE_SIZE) return -1;
    if (g_table[irq].fn)       return -1;
    g_table[irq].data = data;
    __atomic_store_n((uintptr_t *)&g_table[irq].fn, (uintptr_t)fn,
                     __ATOMIC_RELEASE);
    return 0;
}

void irq_unregister(unsigned int irq)
{
    if (irq >= IRQ_TABLE_SIZE) return;
    __atomic_store_n((uintptr_t *)&g_table[irq].fn, (uintptr_t)0,
                     __ATOMIC_RELEASE);
    g_table[irq].data = NULL;
}

void irq_dispatch(void)
{
    unsigned int irq = gic_ack();
    /* IAR returns 1022 for spurious and 1023 for special group — neither
     * requires EOI. */
    if (irq >= 1020)
        return;

    irq_handler_t fn = NULL;
    if (irq < IRQ_TABLE_SIZE)
        fn = (irq_handler_t)__atomic_load_n(
                 (uintptr_t *)&g_table[irq].fn, __ATOMIC_ACQUIRE);

    if (fn) {
        fn(g_table[irq].data);
    } else {
        kprintf("[irq] unhandled IRQ %u — masking\n", irq);
        gic_disable(irq);
    }

    gic_eoi(irq);
}
