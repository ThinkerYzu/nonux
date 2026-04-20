#include "ktest.h"
#include "core/pmm/pmm.h"

/*
 * In-kernel PMM tests.  Unlike test/host/pmm_test.c these exercise the
 * real PMM initialized over physical RAM above __free_mem_start, so a
 * "write then read back" actually proves the page is usable memory.
 */

extern char __free_mem_start[];

KTEST(pmm_alloc_page_is_aligned_and_above_kernel)
{
    void *p = pmm_alloc_page();
    KASSERT_NOT_NULL(p);
    KASSERT_EQ_U((uintptr_t)p & (PMM_PAGE_SIZE - 1), 0);
    KASSERT((uintptr_t)p >= (uintptr_t)__free_mem_start);
    pmm_free_page(p);
}

KTEST(pmm_alloc_page_is_writable_and_readable)
{
    volatile uint64_t *p = pmm_alloc_page();
    KASSERT_NOT_NULL((void *)p);
    p[0] = 0xDEADBEEFCAFEBABEUL;
    p[1] = 0x0123456789ABCDEFUL;
    p[PMM_PAGE_SIZE / sizeof(uint64_t) - 1] = 0x1111222233334444UL;
    KASSERT_EQ_U(p[0], 0xDEADBEEFCAFEBABEUL);
    KASSERT_EQ_U(p[1], 0x0123456789ABCDEFUL);
    KASSERT_EQ_U(p[PMM_PAGE_SIZE / sizeof(uint64_t) - 1],
                 0x1111222233334444UL);
    pmm_free_page((void *)p);
}

KTEST(pmm_full_cycle_restores_free_count)
{
    enum { N = 128 };
    size_t before = pmm_free_count();
    KASSERT(before >= N);

    void *pages[N];
    for (int i = 0; i < N; i++) {
        pages[i] = pmm_alloc_page();
        KASSERT_NOT_NULL(pages[i]);
    }
    KASSERT_EQ_U(pmm_free_count(), before - N);

    for (int i = 0; i < N; i++)
        pmm_free_page(pages[i]);
    KASSERT_EQ_U(pmm_free_count(), before);
}

KTEST(pmm_contiguous_alloc_is_contiguous)
{
    uint8_t *block = pmm_alloc_pages(4);
    KASSERT_NOT_NULL(block);
    KASSERT_EQ_U((uintptr_t)block & (PMM_PAGE_SIZE - 1), 0);

    /* Touch the first and last byte of the full range to prove the
     * whole block is backed. */
    block[0] = 0xAA;
    block[4 * PMM_PAGE_SIZE - 1] = 0x55;
    KASSERT_EQ_U(block[0], 0xAA);
    KASSERT_EQ_U(block[4 * PMM_PAGE_SIZE - 1], 0x55);

    pmm_free_pages(block, 4);
}
