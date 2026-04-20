#ifndef NONUX_IRQ_H
#define NONUX_IRQ_H

#include <stddef.h>
#include <stdint.h>

/*
 * Core IRQ framework.
 *
 * Phase 2 scope: a simple per-IRQ dispatch table.  Later phases will layer
 * the hook/interception framework on top without changing this interface.
 * Handlers run in interrupt context with IRQ already masked; they must be
 * short and must not block (bounded-handler rule — see DESIGN.md).
 */

typedef void (*irq_handler_t)(void *data);

#define IRQ_TABLE_SIZE 128   /* covers all PPIs + early SPIs */

int  irq_register(unsigned int irq, irq_handler_t handler, void *data);
void irq_unregister(unsigned int irq);

/* Dispatch the currently-pending IRQ.  Called from the EL1 IRQ vector. */
void irq_dispatch(void);

/* GIC driver — QEMU virt (GICv2). */
void gic_init(void);
void gic_enable(unsigned int irq);
void gic_disable(unsigned int irq);

#endif /* NONUX_IRQ_H */
