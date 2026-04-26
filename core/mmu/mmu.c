#include "mmu.h"

#include <stddef.h>
#include <stdint.h>

#if __STDC_HOSTED__
#include <stdlib.h>
#include <string.h>
#else
#include "core/lib/kheap.h"
#include "core/lib/lib.h"
#include "framework/process.h"          /* NX_PROCESS_TLS_OFFSET, _SIZE */
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

/* User window: a contiguous run of 2 MiB blocks carved out of L2_ram
 * for EL0 code + stack.  Chosen so it's deep enough into RAM to be
 * clear of the PMM's near-term allocations (kernel image ends around
 * 0x40096000; PMM hands out contiguously from 0x400d6000; 128 MiB in
 * is far past any reasonable consumer).  Slice 5.6+ moved user pages
 * onto per-task TTBR0 tables.  Slice 7.6d.2b grew the window from
 * one block (2 MiB) to four blocks (8 MiB) so a static-linked
 * busybox image (~1.91 MiB text+data) leaves room for stack + heap. */
#define USER_WINDOW_INDEX   64u                        /* L2_ram entry # */
#define USER_WINDOW_BLOCKS  4u                         /* count of 2 MiB blocks */
#define USER_WINDOW_VA      (RAM_BASE + \
                             ((uint64_t)USER_WINDOW_INDEX << BLOCK2_SHIFT))
#define USER_WINDOW_SIZE    ((uint64_t)USER_WINDOW_BLOCKS << BLOCK2_SHIFT)
#define USER_BLOCK_SIZE     ((uint64_t)1 << BLOCK2_SHIFT)  /* 2 MiB block alignment */

uint64_t mmu_user_window_base(void) { return USER_WINDOW_VA; }
uint64_t mmu_user_window_size(void) { return USER_WINDOW_SIZE; }

void mmu_init(void)
{
    /* Populate L2 tables.  2 MiB blocks cover the full 1 GiB each. */
    for (uint64_t i = 0; i < 512; i++) {
        l2_mmio_table[i] = device_block(MMIO_BASE + (i << BLOCK2_SHIFT));
        l2_ram_table[i]  = normal_block(RAM_BASE  + (i << BLOCK2_SHIFT));
    }

    /* Upgrade slot USER_WINDOW_INDEX's permissions so EL0 can access
     * the first user-window block in the kernel's address space.
     * Slice-5.5 era artifact for drop_to_el0 with kernel TTBR0 (no
     * per-process address space yet).  Per-process L2_ram tables
     * overwrite USER_WINDOW_INDEX..USER_WINDOW_INDEX+USER_WINDOW_BLOCKS-1
     * with user_block descriptors pointing at the per-process
     * backing — see mmu_create_address_space.  We deliberately leave
     * the kernel's slots USER_WINDOW_INDEX+1.. as normal_block
     * (kernel-only) so a stray EL0 access via the kernel TTBR0
     * faults rather than silently aliasing the kernel's identity
     * map of those PAs (which often coincides with some other
     * process's user backing post-slice-7.6d.2b). */
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

    /* Slice 7.6c.3b: enable EL0 access to FP/SIMD.  CPACR_EL1.FPEN =
     * 0b11 means "no trap from any EL"; the kernel itself is built
     * with -mgeneral-regs-only so it never touches FP/SIMD, but EL0
     * userspace (musl's memset, busybox, etc.) freely uses NEON.  In
     * v1 we don't save/restore FP state on context switch — the
     * single-EL0-task-at-a-time pattern in our ktests means FP
     * registers stay coherent for the running task.  Cross-task FP
     * persistence (e.g. a long-running busybox shell parented over
     * forked children that also use FP) lands with a future
     * "FP context save on schedule" slice. */
    uint64_t cpacr;
    asm volatile ("mrs %0, cpacr_el1" : "=r"(cpacr));
    cpacr |= (3UL << 20);  /* FPEN = 0b11 — no trap on FP/SIMD */
    asm volatile ("msr cpacr_el1, %0" :: "r"(cpacr) : "memory");
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
 *     [0..63]    = kernel's l2_ram_table[0..63]   (RAM, kernel-only, shared PAs)
 *     [64..67]   = user_block(user_backing + i*2 MiB)  (private 8 MiB)
 *     [68..511]  = kernel's l2_ram_table[68..511]
 *
 *   user_backing           fresh USER_WINDOW_SIZE (8 MiB) chunk, aligned
 *                          to USER_BLOCK_SIZE (2 MiB) — each L2 block is
 *                          2 MiB-granular, so the four contiguous slots
 *                          just need the start address to be 2 MiB-aligned;
 *                          subsequent slots follow by stride.  We align
 *                          the malloc result manually (PMM advances by
 *                          pages only, so the raw return is 4 KiB-aligned).
 */

#if !__STDC_HOSTED__
/*
 * Bookkeeping for destroy.  An L1 root is an opaque `uint64_t l1_pa`; to
 * free it we need to know which physical pages hold L1_priv, L2_ram_priv,
 * and the 8 MiB user backing.  We stash them in a small header placed at
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
/* v1 cap.  Bumped from 16 → 32 in slice 7.6c.4 alongside
 * NX_PROCESS_TABLE_CAPACITY (same reason: cumulative test usage of
 * stranded processes hit the previous cap). */
#define MMU_MAX_ADDRESS_SPACES  32u

struct mmu_address_space {
    uint64_t  l1_root_pa;     /* == (uintptr_t)l1_page */
    void     *l1_page;        /* 4 KiB L1 table */
    void     *l2_ram_page;    /* 4 KiB L2_ram */
    void     *user_backing;   /* USER_WINDOW_SIZE (8 MiB) user window */
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

    /* User backing — `USER_WINDOW_SIZE` bytes (8 MiB after slice
     * 7.6d.2b) of contiguous RAM, aligned to a 2 MiB block boundary.
     * Each L2 block is 2 MiB-granular, so the four contiguous slots
     * just need the START address to be 2 MiB-aligned; the rest
     * follow by stride.  Trim alignment slack to one block (was
     * USER_WINDOW_SIZE * 2 in the 2 MiB era — wasteful at 8 MiB). */
    void *raw = malloc(USER_WINDOW_SIZE + USER_BLOCK_SIZE);
    if (!raw) {
        free(l1); free(l2r);
        return 0;
    }
    uint64_t user_pa = ((uint64_t)(uintptr_t)raw + USER_BLOCK_SIZE - 1) &
                       ~(USER_BLOCK_SIZE - 1);

    /* Slice 7.6d.3c: zero the kernel-pre-initialized TLS area at
     * offset NX_PROCESS_TLS_OFFSET into the user backing.  PMM hands
     * out un-zeroed pages; musl's `__init_libc` reads `errno` (at
     * `(struct pthread *)TPIDR_EL0->errno_val`, struct offset 0x20)
     * on basically every syscall return.  Garbage there → musl
     * thinks errno is some bizarre value and tries to use it as a
     * pointer for some internal check, faulting elsewhere.  Zeroing
     * `NX_PROCESS_TLS_SIZE` bytes ensures the first TLS-relative
     * read returns 0 (the documented "no error" / quiescent state)
     * until musl's `__set_thread_area` overwrites TPIDR_EL0 with
     * its own buffer. */
    memset((void *)(uintptr_t)(user_pa + NX_PROCESS_TLS_OFFSET),
           0, NX_PROCESS_TLS_SIZE);

    /* Populate L1: [0] shares kernel l2_mmio, [1] points at new L2_ram. */
    for (int i = 0; i < 512; i++) l1[i] = 0;
    l1[0] = (uint64_t)(uintptr_t)l2_mmio_table | DESC_VALID | DESC_TABLE;
    l1[1] = (uint64_t)(uintptr_t)l2r           | DESC_VALID | DESC_TABLE;

    /* Populate L2_ram: copy kernel's RAM map, then overwrite the
     * USER_WINDOW_BLOCKS slots starting at USER_WINDOW_INDEX with
     * descriptors pointing at consecutive 2 MiB chunks of the
     * per-process backing. */
    for (int i = 0; i < 512; i++) l2r[i] = l2_ram_table[i];
    for (uint64_t i = 0; i < USER_WINDOW_BLOCKS; i++) {
        l2r[USER_WINDOW_INDEX + i] =
            user_block(user_pa + (i << BLOCK2_SHIFT));
    }

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

    /* Slice 7.6d.2b: the memcpy below uses kernel-mode pointers (=
     * physical addresses, since the kernel runs with an identity
     * map for all of RAM).  We arrived here from a syscall while the
     * caller's process TTBR0 is active, and that TTBR0's L2 has slots
     * USER_WINDOW_INDEX..+USER_WINDOW_BLOCKS-1 overridden as
     * user_block descriptors pointing at the caller's own user_pa.
     * If the destination PA range (dst..dst+USER_WINDOW_SIZE) crosses
     * the user-window VA (0x48000000..+USER_WINDOW_SIZE) — increasingly
     * likely now that user_pa chunks are 8 MiB — the memcpy's writes
     * via the user-window VA would translate back to the *caller's*
     * user_pa instead of dst's PA.  The kernel address space's
     * identity map keeps slot USER_WINDOW_INDEX pointing at PA
     * RAM_BASE+(slot<<BLOCK2_SHIFT), so the same VA resolves to the
     * intended PA there.  Switch to kernel TTBR0 for the copy + back
     * before returning so the caller's RESTORE_TRAPFRAME + eret keeps
     * working in the caller's address space.
     *
     * We do the switch via raw `msr` rather than
     * mmu_switch_address_space() because the latter does a tlbi that
     * blows the entire TLB — overkill twice in a hot path (fork is
     * already slow).  The raw msr + isb is enough to make subsequent
     * fetches/loads use the new root; the existing TLB entries for
     * non-user-window VAs (e.g. the kernel's own code/data) stay
     * valid because they're identical across roots. */
    uint64_t saved_ttbr0;
    asm volatile ("mrs %0, ttbr0_el1" : "=r"(saved_ttbr0));
    asm volatile ("msr ttbr0_el1, %0" :: "r"(mmu_kernel_address_space()) : "memory");
    asm volatile ("isb");

    memcpy(dst, src, USER_WINDOW_SIZE);

    asm volatile ("msr ttbr0_el1, %0" :: "r"(saved_ttbr0) : "memory");
    asm volatile ("isb");
    /* Same cache-coherence sequence as the ELF loader's post-copy
     * barrier: freshly-copied bytes may hold executable code. */
    asm volatile ("dsb ish"  ::: "memory");
    asm volatile ("ic iallu" ::: "memory");
    asm volatile ("dsb ish"  ::: "memory");
    asm volatile ("isb");
#endif
}
