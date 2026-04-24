#include "mmu.h"

#include <stddef.h>
#include <stdint.h>

#if __STDC_HOSTED__
#include <stdlib.h>
#include <string.h>
#else
#include "core/lib/kheap.h"
#include "core/lib/lib.h"
#endif

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

/* User-accessible Normal block — slice 5.5.
 *
 * Same attributes as a plain Normal block except AP permits EL0 access
 * (AP=0b01: RW at both EL1 and EL0) and UXN=0 (EL0 may execute).  PXN
 * stays 0 so kernel code in this region also runs fine if needed.
 *
 * Used only for the user window at USER_WINDOW_VA; all other RAM blocks
 * stay kernel-only via `normal_block()`.  Per-task TTBR0 tables that
 * would split this into per-process mappings arrive in a follow-up
 * slice.
 */
#define DESC_AP_USER_RW  (1UL << 6)   /* AP[2:1] = 0b01 — EL0+EL1 RW */

static inline uint64_t user_block(uint64_t pa)
{
    return pa | DESC_VALID |
           DESC_ATTR_IDX(ATTR_IDX_NORMAL) |
           DESC_AP_USER_RW | DESC_SH_INNER |
           DESC_AF;
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

/* User window: a single 2 MiB block carved out of L2_ram for EL0 code +
 * stack.  Chosen so it's deep enough into RAM to be clear of the PMM's
 * near-term allocations (kernel image ends around 0x40096000; PMM hands
 * out contiguously from 0x400d6000; 128 MiB in is far past any
 * reasonable slice-5.x consumer).  Slice 5.6+ will move user pages
 * onto per-task TTBR0 tables. */
#define USER_WINDOW_INDEX   64u                        /* L2_ram entry # */
#define USER_WINDOW_VA      (RAM_BASE + \
                             ((uint64_t)USER_WINDOW_INDEX << BLOCK2_SHIFT))
#define USER_WINDOW_SIZE    ((uint64_t)1 << BLOCK2_SHIFT)

uint64_t mmu_user_window_base(void) { return USER_WINDOW_VA; }
uint64_t mmu_user_window_size(void) { return USER_WINDOW_SIZE; }

void mmu_init(void)
{
    /* Populate L2 tables.  2 MiB blocks cover the full 1 GiB each. */
    for (uint64_t i = 0; i < 512; i++) {
        l2_mmio_table[i] = device_block(MMIO_BASE + (i << BLOCK2_SHIFT));
        l2_ram_table[i]  = normal_block(RAM_BASE  + (i << BLOCK2_SHIFT));
    }

    /* Upgrade the user-window slot's permissions so EL0 can read /
     * write / execute within it.  Still identity-mapped to the same
     * physical 2 MiB (VA == PA). */
    l2_ram_table[USER_WINDOW_INDEX] =
        user_block(RAM_BASE + ((uint64_t)USER_WINDOW_INDEX << BLOCK2_SHIFT));

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

/* ---------- Per-process address spaces (slice 7.2) -------------------- */
/*
 * Layout review (see mmu.h for the full description):
 *
 *   L1_priv[512]           fresh 4 KiB page, aligned to 4 KiB
 *     [0] → l2_mmio_table  kernel-static device map, shared across all procs
 *     [1] → L2_ram_priv    fresh 4 KiB, copy of kernel L2_ram except slot 64
 *     [2..511] invalid     (no mapping)
 *
 *   L2_ram_priv[512]       fresh 4 KiB page, aligned to 4 KiB
 *     [0..63]  = kernel's l2_ram_table[0..63]  (RAM, kernel-only, shared PAs)
 *     [64]     = user_block(user_backing)      (this process's private 2 MiB)
 *     [65..511] = kernel's l2_ram_table[65..511]
 *
 *   user_backing           fresh 2 MiB chunk, aligned to 2 MiB (via malloc
 *                          which routes through PMM for large allocs;
 *                          PMM hands back page-aligned PA-contiguous runs,
 *                          and for ≥ 2 MiB the alignment happens to
 *                          coincide because PMM advances by pages only —
 *                          we align the result manually below to be safe).
 */

#if !__STDC_HOSTED__
/*
 * Bookkeeping for destroy.  An L1 root is an opaque `uint64_t l1_pa`; to
 * free it we need to know which physical pages hold L1_priv, L2_ram_priv,
 * and the 2 MiB user backing.  We stash them in a small header placed at
 * the FRONT of the L1 page — the page is 4 KiB, the header is 24 bytes,
 * and the actual page-table descriptors start at a 4 KiB alignment.
 * Using the same page for the header keeps per-process state inline and
 * avoids another allocation.  The L1 page-table proper starts at
 * offset 0, so we can't actually put the header there.
 *
 * Simpler: keep a separate per-process `struct mmu_address_space` on
 * the heap that carries the bookkeeping.  The caller holds an L1 root
 * (uint64_t PA); we find the bookkeeping via a tiny lookup table.
 */
#define MMU_MAX_ADDRESS_SPACES  16u

struct mmu_address_space {
    uint64_t  l1_root_pa;     /* == (uintptr_t)l1_page */
    void     *l1_page;        /* 4 KiB L1 table */
    void     *l2_ram_page;    /* 4 KiB L2_ram */
    void     *user_backing;   /* 2 MiB user window */
    void     *user_backing_raw;  /* the unaligned malloc() return — for free */
};

static struct mmu_address_space g_mmu_spaces[MMU_MAX_ADDRESS_SPACES];

static struct mmu_address_space *find_space_by_root(uint64_t root)
{
    for (unsigned i = 0; i < MMU_MAX_ADDRESS_SPACES; i++)
        if (g_mmu_spaces[i].l1_root_pa == root)
            return &g_mmu_spaces[i];
    return NULL;
}

static struct mmu_address_space *alloc_space_slot(void)
{
    for (unsigned i = 0; i < MMU_MAX_ADDRESS_SPACES; i++)
        if (g_mmu_spaces[i].l1_root_pa == 0)
            return &g_mmu_spaces[i];
    return NULL;
}
#endif /* !__STDC_HOSTED__ */

uint64_t mmu_create_address_space(void)
{
#if __STDC_HOSTED__
    /* Host has no MMU — there's nothing meaningful to build.  Return 0
     * so the caller can treat this as "no address space was allocated"
     * and still proceed with a valid `nx_process` (most of which is
     * architecture-independent). */
    return 0;
#else
    struct mmu_address_space *s = alloc_space_slot();
    if (!s) return 0;

    /* Page-table pages are 4 KiB and need to be 4 KiB-aligned.  Kheap's
     * malloc for sizes > KHEAP_SLAB_MAX routes through PMM which
     * returns page-aligned chunks, so a plain 4 KiB allocation is fine. */
    uint64_t *l1  = malloc(4096);
    uint64_t *l2r = malloc(4096);
    if (!l1 || !l2r) {
        if (l1)  free(l1);
        if (l2r) free(l2r);
        return 0;
    }

    /* 2 MiB user backing — malloc an extra 2 MiB to align to a 2 MiB
     * boundary manually.  PMM's pages are 4 KiB, so without this
     * we'd get a 4 KiB-aligned chunk that may straddle a 2 MiB
     * boundary, which makes it unusable as an L2 block. */
    void *raw = malloc(USER_WINDOW_SIZE * 2);
    if (!raw) {
        free(l1); free(l2r);
        return 0;
    }
    uint64_t user_pa = ((uint64_t)(uintptr_t)raw + USER_WINDOW_SIZE - 1) &
                       ~(USER_WINDOW_SIZE - 1);

    /* Populate L1: [0] shares kernel l2_mmio, [1] points at new L2_ram. */
    for (int i = 0; i < 512; i++) l1[i] = 0;
    l1[0] = (uint64_t)(uintptr_t)l2_mmio_table | DESC_VALID | DESC_TABLE;
    l1[1] = (uint64_t)(uintptr_t)l2r           | DESC_VALID | DESC_TABLE;

    /* Populate L2_ram: copy kernel's RAM map, then overwrite slot 64. */
    for (int i = 0; i < 512; i++) l2r[i] = l2_ram_table[i];
    l2r[USER_WINDOW_INDEX] = user_block(user_pa);

    s->l1_root_pa       = (uint64_t)(uintptr_t)l1;
    s->l1_page          = l1;
    s->l2_ram_page      = l2r;
    s->user_backing     = (void *)(uintptr_t)user_pa;
    s->user_backing_raw = raw;

    /* Make the table writes visible before anyone could walk them. */
    asm volatile ("dsb ish" ::: "memory");
    return s->l1_root_pa;
#endif
}

void mmu_destroy_address_space(uint64_t l1_root)
{
    if (l1_root == 0) return;
#if !__STDC_HOSTED__
    if (l1_root == (uint64_t)(uintptr_t)l1_table) return; /* kernel root */

    struct mmu_address_space *s = find_space_by_root(l1_root);
    if (!s) return;

    free(s->l1_page);
    free(s->l2_ram_page);
    free(s->user_backing_raw);
    s->l1_root_pa       = 0;
    s->l1_page          = NULL;
    s->l2_ram_page      = NULL;
    s->user_backing     = NULL;
    s->user_backing_raw = NULL;
#else
    (void)l1_root;
#endif
}

#if __STDC_HOSTED__
static uint64_t g_host_current_root;

uint64_t mmu_current_address_space_for_test(void)
{
    return g_host_current_root;
}
#endif

void mmu_switch_address_space(uint64_t l1_root)
{
#if __STDC_HOSTED__
    g_host_current_root = l1_root;
#else
    /* Write TTBR0_EL1, then invalidate TLB + pipeline so the next
     * instruction fetch uses the new tables.  Because every root we
     * build shares the kernel's identity map for the currently-
     * executing kernel code page, the instruction fetch immediately
     * after the isb resolves to the same PA via the new root. */
    asm volatile ("msr ttbr0_el1, %0" :: "r"(l1_root) : "memory");
    asm volatile ("isb");
    asm volatile ("tlbi vmalle1" ::: "memory");
    asm volatile ("dsb ish"      ::: "memory");
    asm volatile ("isb");
#endif
}

uint64_t mmu_kernel_address_space(void)
{
#if __STDC_HOSTED__
    return 0;
#else
    return (uint64_t)(uintptr_t)l1_table;
#endif
}

void *mmu_address_space_user_backing(uint64_t root)
{
#if __STDC_HOSTED__
    (void)root;
    return NULL;
#else
    if (root == 0) return NULL;
    if (root == (uint64_t)(uintptr_t)l1_table) return NULL;
    struct mmu_address_space *s = find_space_by_root(root);
    return s ? s->user_backing : NULL;
#endif
}

void mmu_copy_user_backing(uint64_t src_root, uint64_t dst_root)
{
#if __STDC_HOSTED__
    (void)src_root; (void)dst_root;
#else
    void *src = mmu_address_space_user_backing(src_root);
    void *dst = mmu_address_space_user_backing(dst_root);
    if (!src || !dst || src == dst) return;
    memcpy(dst, src, USER_WINDOW_SIZE);
    /* Same cache-coherence sequence as the ELF loader's post-copy
     * barrier: freshly-copied bytes may hold executable code. */
    asm volatile ("dsb ish"  ::: "memory");
    asm volatile ("ic iallu" ::: "memory");
    asm volatile ("dsb ish"  ::: "memory");
    asm volatile ("isb");
#endif
}
