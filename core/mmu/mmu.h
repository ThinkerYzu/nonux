#ifndef NONUX_MMU_H
#define NONUX_MMU_H

#include <stdint.h>

/*
 * Phase 5 slice 5.1 — kernel MMU bring-up.
 *
 * `mmu_init` builds a boot-time identity-mapped page table and turns on
 * SCTLR_EL1.{M,C,I}.  The mapping covers:
 *
 *   0x00000000 – 0x40000000  Device-nGnRnE   (GIC, UART0, other MMIO)
 *   0x40000000 – 0x80000000  Normal WB-WA    (RAM, Inner Shareable, AF set)
 *
 * Both ranges are mapped with 2 MiB blocks at L2.  VA = PA everywhere,
 * so turning the MMU on is a behaviour change (caching, reordering,
 * unaligned-access semantics) but not an address change — every kernel
 * symbol stays at the same numeric address.
 *
 * The kernel RAM mapping is currently RWX at EL1 with UXN=1 (no EL0
 * execution).  Split .text / .rodata / .data / .bss permissions land
 * with slice 5.2 once mm_buddy owns the page table.
 *
 * Call exactly once, early in boot (after uart_init, before anything
 * that would benefit from the D-cache).
 */
void mmu_init(void);

/* True iff SCTLR_EL1.M is set.  Test helper; production code should
 * never need to query this. */
int  mmu_is_enabled(void);

/* Slice 5.5 — user window exposed so EL0 test code can find the VA/size
 * of the one EL0-accessible 2 MiB block.  Identity-mapped (VA == PA),
 * Normal memory, AP=EL0+EL1 RW, UXN=0.  Values are constants — safe to
 * call before the MMU is on. */
uint64_t mmu_user_window_base(void);
uint64_t mmu_user_window_size(void);

#endif /* NONUX_MMU_H */
