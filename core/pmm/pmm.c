#include "pmm.h"
#include "core/lib/lib.h"

/*
 * Bitmap-backed physical allocator.
 *
 * Layout: the bitmap sits at the very start of the managed region.  Bit N
 * refers to the page whose physical base is `g_base + N * PMM_PAGE_SIZE`.
 * 0 = free, 1 = allocated.  Pages covered by the bitmap itself are marked
 * allocated at init.
 *
 * Allocation is first-fit.  We start each scan from `g_hint` so freed pages
 * tend to be reused quickly; the hint is advisory, not authoritative.
 */

static uintptr_t g_base;
static uint8_t  *g_bitmap;          /* bytes, atomic bit flips */
static size_t    g_total;           /* total pages in the pool */
static size_t    g_reserved;        /* pages reserved for the bitmap */
static size_t    g_hint;            /* search hint — advisory */

/* _Atomic so counter reads/writes don't tear across interrupt preemption.
 * Not declared _Atomic in the bitmap because GCC __atomic_fetch_* builtins
 * work on plain bytes; the bitmap is a contiguous byte array. */
static _Atomic size_t g_free;

static inline size_t bit_byte(size_t index) { return index >> 3; }
static inline uint8_t bit_mask(size_t index) { return (uint8_t)(1U << (index & 7U)); }

/* Non-atomic read — used during init, and as a cheap prefilter in alloc.
 * Racing readers get stale bits but the subsequent CAS via try_claim makes
 * that safe. */
static int bit_test(size_t index)
{
    return (__atomic_load_n(&g_bitmap[bit_byte(index)], __ATOMIC_RELAXED)
            & bit_mask(index)) != 0;
}

static int try_claim(size_t index)
{
    uint8_t mask = bit_mask(index);
    uint8_t old  = __atomic_fetch_or(&g_bitmap[bit_byte(index)], mask,
                                     __ATOMIC_ACQUIRE);
    return (old & mask) == 0;
}

static void release(size_t index)
{
    uint8_t mask = bit_mask(index);
    __atomic_fetch_and(&g_bitmap[bit_byte(index)], (uint8_t)~mask,
                       __ATOMIC_RELEASE);
}

static void *page_addr(size_t index)
{
    return (void *)(g_base + index * (uintptr_t)PMM_PAGE_SIZE);
}

static size_t index_of(void *page)
{
    return ((uintptr_t)page - g_base) >> PMM_PAGE_SHIFT;
}

void pmm_init(uintptr_t base, size_t size)
{
    /* Round base up to page boundary, size down to page multiple. */
    uintptr_t aligned = (base + PMM_PAGE_SIZE - 1) & ~(uintptr_t)(PMM_PAGE_SIZE - 1);
    size_t    adjust  = aligned - base;
    if (size < adjust)
        size = 0;
    else
        size -= adjust;
    size &= ~(size_t)(PMM_PAGE_SIZE - 1);

    g_base  = aligned;
    g_total = size / PMM_PAGE_SIZE;

    /* Bitmap: one bit per page, living at the start of the region. */
    g_bitmap = (uint8_t *)aligned;
    size_t bitmap_bytes = (g_total + 7) / 8;
    size_t bitmap_pages = (bitmap_bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;

    /* Zero everything first, then mark the bitmap's own pages as allocated. */
    memset(g_bitmap, 0, bitmap_bytes);
    for (size_t i = 0; i < bitmap_pages; i++) {
        uint8_t mask = bit_mask(i);
        g_bitmap[bit_byte(i)] |= mask;
    }

    g_reserved = bitmap_pages;
    g_hint     = bitmap_pages;
    __atomic_store_n(&g_free, g_total - bitmap_pages, __ATOMIC_RELEASE);
}

void *pmm_alloc_page(void)
{
    size_t start = __atomic_load_n(&g_hint, __ATOMIC_RELAXED);
    if (start >= g_total) start = g_reserved;

    for (size_t step = 0; step < g_total; step++) {
        size_t i = start + step;
        if (i >= g_total) i -= g_total;
        if (i < g_reserved) continue;

        if (!bit_test(i) && try_claim(i)) {
            __atomic_store_n(&g_hint, i + 1, __ATOMIC_RELAXED);
            __atomic_fetch_sub(&g_free, 1, __ATOMIC_RELAXED);
            return page_addr(i);
        }
    }
    return NULL;
}

void *pmm_alloc_pages(size_t count)
{
    if (count == 0) return NULL;
    if (count == 1) return pmm_alloc_page();
    if (count > g_total - g_reserved) return NULL;

    for (size_t i = g_reserved; i + count <= g_total; ) {
        /* Find a run of `count` apparently-free pages. */
        size_t run = 0;
        while (run < count && !bit_test(i + run)) run++;
        if (run < count) {
            /* page i+run is set — skip past it */
            i += run + 1;
            continue;
        }

        /* Try to claim all of them. */
        size_t claimed = 0;
        for (; claimed < count; claimed++) {
            if (!try_claim(i + claimed))
                break;
        }
        if (claimed == count) {
            __atomic_store_n(&g_hint, i + count, __ATOMIC_RELAXED);
            __atomic_fetch_sub(&g_free, count, __ATOMIC_RELAXED);
            return page_addr(i);
        }

        /* Roll back and retry after the failure. */
        for (size_t j = 0; j < claimed; j++)
            release(i + j);
        i += claimed + 1;
    }
    return NULL;
}

void pmm_free_page(void *page)
{
    pmm_free_pages(page, 1);
}

void pmm_free_pages(void *page, size_t count)
{
    size_t idx = index_of(page);
    for (size_t i = 0; i < count; i++)
        release(idx + i);
    __atomic_fetch_add(&g_free, count, __ATOMIC_RELAXED);
    /* Encourage quick reuse by lowering the hint. */
    __atomic_store_n(&g_hint, idx, __ATOMIC_RELAXED);
}

size_t pmm_free_count(void)
{
    return __atomic_load_n(&g_free, __ATOMIC_RELAXED);
}

size_t pmm_total_count(void)
{
    return g_total - g_reserved;
}
