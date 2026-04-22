#include "mmu.h"

#include <stdint.h>

/*
 * Kernel MMU bring-up — slice 5.1.
 *
 * Page-table shape (4 KiB granule, 39-bit VA, T0SZ=25 → 3 levels):
 *
 *       +---- L1 (512 entries, 1 GiB each) ----+
 *       |  [0]  → L2_mmio   (0.0 GiB .. 1.0 GiB)
 *       |  [1]  → L2_ram    (1.0 GiB .. 2.0 GiB)
 *       |  [2..511]  invalid
 *       +--------------------------------------+
 *
 *       L2_mmio: 512 × 2 MiB blocks, Device-nGnRnE, RW at EL1, PXN+UXN.
 *       L2_ram:  512 × 2 MiB blocks, Normal WB-WA Inner Shareable, RW EL1,
 *                executable at EL1 (PXN=0), non-executable at EL0 (UXN=1).
 *
 * All three tables are in BSS (zero-initialised by start.S).  VA = PA
 * everywhere, so turning the MMU on does not change what any kernel
 * symbol resolves to — only its cacheability, ordering semantics, and
 * alignment rules change.
 */

/* ---------- Descriptor bit layout (AArch64 stage 1, ARMv8.0-A) ---------- */

#define DESC_VALID        (1UL << 0)
#define DESC_TABLE        (1UL << 1)   /* at L1/L2: table vs block */
#define DESC_ATTR_IDX(n)  ((uint64_t)((n) & 0x7) << 2)
#define DESC_AP_EL1_RW    (0UL << 6)
#define DESC_SH_INNER     (3UL << 8)
#define DESC_SH_NONE      (0UL << 8)
#define DESC_AF           (1UL << 10)
#define DESC_PXN          (1UL << 53)  /* EL1 no-execute */
#define DESC_UXN          (1UL << 54)  /* EL0 no-execute */

/* ---------- Memory attribute indirection (MAIR_EL1) --------------------- */

#define MAIR_ATTR_DEVICE_nGnRnE  0x00UL
#define MAIR_ATTR_NORMAL_WBWA    0xFFUL   /* Inner WB-WA + Outer WB-WA */

#define ATTR_IDX_DEVICE  0
#define ATTR_IDX_NORMAL  1

#define MAIR_VALUE  ((MAIR_ATTR_DEVICE_nGnRnE << (ATTR_IDX_DEVICE * 8)) | \
                     (MAIR_ATTR_NORMAL_WBWA   << (ATTR_IDX_NORMAL * 8)))

/* ---------- Geometry ---------------------------------------------------- */

#define BLOCK2_SHIFT  21UL           /* 2 MiB */
#define PAGE_SHIFT    12UL

#define RAM_BASE      0x40000000UL
#define MMIO_BASE     0x00000000UL
#define GIB(n)        ((uint64_t)(n) * 0x40000000UL)

static uint64_t l1_table[512]      __attribute__((aligned(4096)));
static uint64_t l2_mmio_table[512] __attribute__((aligned(4096)));
static uint64_t l2_ram_table[512]  __attribute__((aligned(4096)));

/* ---------- Descriptor builders ---------------------------------------- */

/* Device block: RW at EL1, no execution at either EL, no cache. */
static inline uint64_t device_block(uint64_t pa)
{
    return pa | DESC_VALID |
           DESC_ATTR_IDX(ATTR_IDX_DEVICE) |
           DESC_AP_EL1_RW | DESC_SH_NONE |
           DESC_AF | DESC_PXN | DESC_UXN;
}

/* Normal-memory block: RW at EL1, executable at EL1 (PXN=0), no EL0
 * execution (UXN=1), Inner Shareable, Write-Back Write-Allocate. */
static inline uint64_t normal_block(uint64_t pa)
{
    return pa | DESC_VALID |
           DESC_ATTR_IDX(ATTR_IDX_NORMAL) |
           DESC_AP_EL1_RW | DESC_SH_INNER |
           DESC_AF | DESC_UXN;
}

/* ---------- TCR_EL1 ----------------------------------------------------- *
 *
 *   T0SZ=25       → 39-bit VA (start at L1)
 *   IRGN0=01      → table walks Normal Inner WB-WA
 *   ORGN0=01      → table walks Normal Outer WB-WA
 *   SH0=11        → inner shareable
 *   TG0=00        → 4 KiB granule
 *   EPD1=1        → disable TTBR1 walks (we don't use the high half yet)
 *   IPS=001       → 36-bit PA (plenty for QEMU virt)
 */
#define TCR_VALUE   ((25UL << 0)  | \
                     (1UL  << 8)  | \
                     (1UL  << 10) | \
                     (3UL  << 12) | \
                     (0UL  << 14) | \
                     (1UL  << 23) | \
                     (1UL  << 32))

/* ---------- Bring-up --------------------------------------------------- */

void mmu_init(void)
{
    /* Populate L2 tables.  2 MiB blocks cover the full 1 GiB each. */
    for (uint64_t i = 0; i < 512; i++) {
        l2_mmio_table[i] = device_block(MMIO_BASE + (i << BLOCK2_SHIFT));
        l2_ram_table[i]  = normal_block(RAM_BASE  + (i << BLOCK2_SHIFT));
    }

    /* L1: wire the two L2 tables.  Everything else stays invalid. */
    l1_table[0] = (uint64_t)l2_mmio_table | DESC_VALID | DESC_TABLE;
    l1_table[1] = (uint64_t)l2_ram_table  | DESC_VALID | DESC_TABLE;

    /* Make the table writes visible to the table walker before it
     * starts walking.  The walker is in the inner shareable domain
     * once TCR.SH0=11, so `dsb ish` suffices. */
    asm volatile ("dsb ish" ::: "memory");

    asm volatile ("msr mair_el1, %0"  :: "r"(MAIR_VALUE));
    asm volatile ("msr tcr_el1,  %0"  :: "r"((uint64_t)TCR_VALUE));
    asm volatile ("msr ttbr0_el1, %0" :: "r"((uint64_t)(uintptr_t)l1_table));
    asm volatile ("isb");

    /* Flush any stale EL1 translations and I-cache contents before
     * turning the MMU on. */
    asm volatile ("tlbi vmalle1"  ::: "memory");
    asm volatile ("dsb ish"       ::: "memory");
    asm volatile ("ic iallu"      ::: "memory");
    asm volatile ("dsb ish"       ::: "memory");
    asm volatile ("isb");

    /* Enable MMU + D-cache + I-cache atomically.  The `isb` after the
     * write is what makes the next instruction fetch go through the
     * translation table.  Because VA == PA everywhere we mapped,
     * execution continues uninterrupted at the next `isb`-following
     * instruction. */
    uint64_t sctlr;
    asm volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0);   /* M  — MMU enable              */
    sctlr |= (1UL << 2);   /* C  — D-cache enable          */
    sctlr |= (1UL << 12);  /* I  — I-cache enable          */
    asm volatile ("msr sctlr_el1, %0" :: "r"(sctlr) : "memory");
    asm volatile ("isb");
}

int mmu_is_enabled(void)
{
    uint64_t sctlr;
    asm volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));
    return (sctlr & 1UL) ? 1 : 0;
}
