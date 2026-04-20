#include "irq.h"

/*
 * GICv2 driver for QEMU virt.
 *
 * Memory map (from QEMU's hw/arm/virt.c):
 *   0x08000000 - 0x0800FFFF : GICD (distributor)
 *   0x08010000 - 0x0801FFFF : GICC (CPU interface)
 *
 * Phase 2 scope:
 *   - distributor enable
 *   - CPU-interface enable, priority mask accept-all
 *   - per-IRQ enable/disable, with priority defaulted to highest (0)
 *   - ack/eoi helpers for the dispatcher
 *
 * SMP, affinity routing, and security extensions (GICv3+) are out of scope
 * until we actually bring up multiple CPUs.
 */

#define GICD_BASE           0x08000000UL
#define GICC_BASE           0x08010000UL

#define GICD_CTLR           (GICD_BASE + 0x000)
#define GICD_ISENABLER(n)   (GICD_BASE + 0x100 + ((n) * 4))
#define GICD_ICENABLER(n)   (GICD_BASE + 0x180 + ((n) * 4))
#define GICD_IPRIORITYR     (GICD_BASE + 0x400)  /* byte-addressed per IRQ */

#define GICC_CTLR           (GICC_BASE + 0x000)
#define GICC_PMR            (GICC_BASE + 0x004)
#define GICC_IAR            (GICC_BASE + 0x00C)
#define GICC_EOIR           (GICC_BASE + 0x010)

static inline void     wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t rd32(uintptr_t a)             { return *(volatile uint32_t *)a; }
static inline void     wr8 (uintptr_t a, uint8_t  v) { *(volatile uint8_t  *)a = v; }

void gic_init(void)
{
    /* GICv2: enable both groups on distributor and CPU interface.  With the
     * security extension absent on QEMU virt, bit 0 alone is enough on most
     * configurations, but setting both matches what Linux does and is
     * harmless. */
    wr32(GICD_CTLR, 0x3);
    wr32(GICC_PMR,  0xFF);
    wr32(GICC_CTLR, 0x3);
}

void gic_enable(unsigned int irq)
{
    wr8(GICD_IPRIORITYR + irq, 0x00);
    wr32(GICD_ISENABLER(irq / 32), 1U << (irq % 32));
}

void gic_disable(unsigned int irq)
{
    wr32(GICD_ICENABLER(irq / 32), 1U << (irq % 32));
}

unsigned int gic_ack(void)
{
    return rd32(GICC_IAR) & 0x3FFU;
}

void gic_eoi(unsigned int irq)
{
    wr32(GICC_EOIR, irq);
}
