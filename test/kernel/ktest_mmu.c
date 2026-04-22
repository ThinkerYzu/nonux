#include "ktest.h"
#include "core/mmu/mmu.h"

/*
 * Kernel-side coverage for slice 5.1.
 *
 * The MMU is brought up by `boot_main` before `ktest_main` runs, so these
 * tests observe the post-enable steady state rather than toggling it
 * themselves.  If any of these fail, something is wrong upstream (the
 * kernel either isn't reaching ktest at all because the MMU enable
 * faulted, or SCTLR_EL1 is somehow not what mmu_init set it to).
 */

KTEST(mmu_sctlr_has_m_c_i_bits_set)
{
    uint64_t sctlr;
    asm volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));
    /* M (0), C (2), I (12) — all set by mmu_init. */
    KASSERT_EQ_U(sctlr & (1UL << 0), 1UL << 0);
    KASSERT_EQ_U(sctlr & (1UL << 2), 1UL << 2);
    KASSERT_EQ_U(sctlr & (1UL << 12), 1UL << 12);
}

KTEST(mmu_is_enabled_helper_returns_true)
{
    KASSERT_EQ_U(mmu_is_enabled(), 1);
}

KTEST(mmu_ttbr0_points_into_ram)
{
    /* The L1 table lives in BSS, which is in the 0x40000000..0x80000000
     * RAM range.  Rule out the case where TTBR0_EL1 ended up 0 or
     * clobbered. */
    uint64_t ttbr0;
    asm volatile ("mrs %0, ttbr0_el1" : "=r"(ttbr0));
    uint64_t base = ttbr0 & ~0x3FUL;
    KASSERT(base >= 0x40000000UL);
    KASSERT(base <  0x80000000UL);
}

/* Deliberately unaligned load/store on Normal memory.  Pre-slice-5.1 the
 * kernel ran with `-mstrict-align` because memory without the MMU is
 * treated as Device-nGnRnE where unaligned access faults synchronously.
 * With the MMU on and RAM mapped Normal (SCTLR.A stays 0), unaligned
 * access is legal. */
KTEST(mmu_unaligned_access_on_normal_memory_succeeds)
{
    /* Static so we have a known-aligned base in the .bss range. */
    static volatile uint8_t buf[32];
    for (int i = 0; i < 32; i++) buf[i] = (uint8_t)i;

    /* Pointer casts via uintptr arithmetic bypass `-mstrict-align`
     * hints the compiler might still hold; the actual load is what
     * matters at runtime. */
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(buf + 1);
    uint32_t v = *p;

    /* Little-endian: bytes at offsets 1,2,3,4 = 0x01,0x02,0x03,0x04. */
    KASSERT_EQ_U(v, 0x04030201UL);

    *p = 0xCAFEBABEUL;
    KASSERT_EQ_U(buf[1], 0xBE);
    KASSERT_EQ_U(buf[2], 0xBA);
    KASSERT_EQ_U(buf[3], 0xFE);
    KASSERT_EQ_U(buf[4], 0xCA);
}
