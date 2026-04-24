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

/* ---------- Per-process address spaces (slice 7.2) -------------------- */
/*
 * `nx_process` owns an "address space" — a TTBR0 root plus enough
 * page-table pages to back the user window.  Layout per process:
 *
 *     L1 (one 4 KiB page) — slot 0 reuses the static kernel L2_mmio,
 *     slot 1 points at the per-process L2_ram.
 *
 *     L2_ram (one 4 KiB page) — a copy of the kernel's static
 *     L2_ram, except slot USER_WINDOW_INDEX (=64) is a user_block()
 *     pointing at this process's own 2 MiB user-window physical
 *     backing.
 *
 *     user window backing (one 2 MiB chunk) — Normal memory,
 *     exclusive to this process.  Allocated via malloc() so the
 *     PMM/kheap delivers page-aligned (and in practice 2 MiB-
 *     aligned since the large path pulls contiguous PMM pages).
 *
 * All kernel mappings (MMIO + non-window RAM) stay identical across
 * every process's TTBR0 — the kernel code therefore remains
 * reachable no matter which address space is current, so switching
 * TTBR0 from kernel-mode code is safe.  The isolation property is
 * that the 2 MiB user window at USER_WINDOW_VA resolves to
 * different physical pages in different processes.
 *
 * Address-space identifiers are `uint64_t` in units of "physical
 * address of the L1 root" — VA == PA under our identity map, so the
 * caller can treat the value as an opaque token plus or as a TTBR0
 * write-through if they want to.
 */

/*
 * Allocate a fresh per-process address space.  Returns the physical
 * address of the L1 root on success (always non-zero for a real
 * allocation), or 0 on allocation failure.  Host builds can't do
 * real MMU work and return 0 unconditionally — callers should treat
 * that as "no address space available" rather than an error.
 */
uint64_t mmu_create_address_space(void);

/*
 * Free a per-process address space created by
 * `mmu_create_address_space`.  Idempotent against 0 and against the
 * kernel's static address space (calling destroy on the kernel
 * address space is a no-op — freeing BSS storage isn't meaningful).
 */
void mmu_destroy_address_space(uint64_t l1_root);

/*
 * Switch TTBR0_EL1 to `l1_root` and flush the EL1/EL0 TLBs.  Safe
 * to call with the current root (becomes a no-op modulo the barrier
 * cost).  Safe to call from kernel mode because every L1 root built
 * by `mmu_create_address_space` shares the kernel's MMIO + RAM
 * identity map — the kernel code page executing this call stays
 * mapped across the switch.
 *
 * On host the call is a no-op: there's no MMU, so "switching" is
 * just remembering the root for test introspection.  Host tests
 * that want to observe which root is "current" can use
 * `mmu_current_address_space_for_test()`.
 */
void mmu_switch_address_space(uint64_t l1_root);

/*
 * Physical address of the kernel's static L1 root — the always-
 * present "kernel address space".  This is the root installed by
 * `mmu_init()` and used by every kthread not yet reassigned to a
 * per-process address space.  Returns 0 if `mmu_init` hasn't run
 * (host builds always return 0).
 */
uint64_t mmu_kernel_address_space(void);

/*
 * Return a kernel-visible pointer to the 2 MiB user-window backing
 * of the address space identified by `root`.  Slice 7.3: the ELF
 * loader writes segment bytes here from kernel context — since VA
 * == PA under the kernel's identity map, this pointer can be
 * dereferenced without switching TTBR0.
 *
 * Returns NULL if `root` is 0 or the kernel root (neither has a
 * per-process backing chunk — the kernel process uses the shared
 * 2 MiB window carved in `mmu_init`).  Host builds always return
 * NULL.
 */
void *mmu_address_space_user_backing(uint64_t root);

#if __STDC_HOSTED__
/*
 * Host-test helper: inspect the value last passed to
 * `mmu_switch_address_space`.  The switch itself is a no-op on
 * host; this helper lets tests prove the call site tried to move
 * to the expected root.  Production code has no reason to call.
 */
uint64_t mmu_current_address_space_for_test(void);
#endif

#endif /* NONUX_MMU_H */
