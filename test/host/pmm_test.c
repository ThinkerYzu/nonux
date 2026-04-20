#include "test_runner.h"
#include "mem_track.h"
#include "core/pmm/pmm.h"

#include <stdint.h>
#include <stdlib.h>

/*
 * Host-side tests for the bitmap PMM.  A posix_memalign'd buffer stands in
 * for the physical memory region.  Each test sets up its own region, so
 * global PMM state is reinitialized per test.
 */

#define REGION_PAGES 256
#define REGION_BYTES (REGION_PAGES * PMM_PAGE_SIZE)

static void *region;

static void setup_region(void)
{
    region = NULL;
    int rc = posix_memalign(&region, PMM_PAGE_SIZE, REGION_BYTES);
    if (rc != 0 || !region) {
        fprintf(stderr, "posix_memalign failed\n");
        abort();
    }
    pmm_init((uintptr_t)region, REGION_BYTES);
}

static void teardown_region(void)
{
    free(region);
    region = NULL;
}

/* --- PMM behavior --- */

TEST(pmm_init_counts_pages_and_reserves_bitmap)
{
    setup_region();
    size_t total = pmm_total_count();
    size_t freec = pmm_free_count();
    /* At init, every non-reserved page is free. */
    ASSERT_EQ_U(total, freec);
    /* 256 pages minus 1 reserved for the bitmap (32 B fits in one page). */
    ASSERT_EQ_U(total, REGION_PAGES - 1);
    teardown_region();
}

TEST(pmm_alloc_page_returns_nonnull_and_aligned)
{
    setup_region();
    void *p = pmm_alloc_page();
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U((uintptr_t)p & (PMM_PAGE_SIZE - 1), 0);
    ASSERT(pmm_free_count() == pmm_total_count() - 1);
    teardown_region();
}

TEST(pmm_alloc_distinct_pages)
{
    setup_region();
    void *a = pmm_alloc_page();
    void *b = pmm_alloc_page();
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT(a != b);
    teardown_region();
}

TEST(pmm_alloc_until_exhausted_then_null)
{
    setup_region();
    size_t total = pmm_total_count();
    void **pages = malloc(total * sizeof(void *));
    for (size_t i = 0; i < total; i++) {
        pages[i] = pmm_alloc_page();
        ASSERT_NOT_NULL(pages[i]);
    }
    /* Pool is drained — next alloc must fail. */
    ASSERT_NULL(pmm_alloc_page());
    ASSERT_EQ_U(pmm_free_count(), 0);
    free(pages);
    teardown_region();
}

TEST(pmm_free_then_realloc)
{
    setup_region();
    void *a = pmm_alloc_page();
    ASSERT_NOT_NULL(a);
    pmm_free_page(a);
    void *b = pmm_alloc_page();
    /* With the hint biased toward freed pages, this should be the same page. */
    ASSERT_EQ_PTR(a, b);
    teardown_region();
}

TEST(pmm_full_cycle_no_leak)
{
    setup_region();
    size_t total = pmm_total_count();
    void **pages = malloc(total * sizeof(void *));
    for (size_t i = 0; i < total; i++)
        pages[i] = pmm_alloc_page();
    for (size_t i = 0; i < total; i++)
        pmm_free_page(pages[i]);
    ASSERT_EQ_U(pmm_free_count(), total);
    free(pages);
    teardown_region();
}

TEST(pmm_alloc_pages_contiguous_two)
{
    setup_region();
    uint8_t *p = pmm_alloc_pages(2);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U((uintptr_t)p & (PMM_PAGE_SIZE - 1), 0);
    /* Second page is adjacent and can be distinguished by address. */
    uint8_t *second = p + PMM_PAGE_SIZE;
    /* Allocate another single page — must differ from both. */
    void *third = pmm_alloc_page();
    ASSERT(third != p);
    ASSERT(third != second);
    pmm_free_pages(p, 2);
    teardown_region();
}

TEST(pmm_alloc_pages_too_big_returns_null)
{
    setup_region();
    ASSERT_NULL(pmm_alloc_pages(REGION_PAGES * 2));
    teardown_region();
}

TEST(pmm_alloc_pages_finds_contiguous_range_after_fragmentation)
{
    setup_region();
    /* Allocate 5 pages singly, free the middle 3 — creates a 3-page gap that
     * pmm_alloc_pages(3) must find. */
    void *p0 = pmm_alloc_page();
    void *p1 = pmm_alloc_page();
    void *p2 = pmm_alloc_page();
    void *p3 = pmm_alloc_page();
    void *p4 = pmm_alloc_page();
    ASSERT_NOT_NULL(p0); ASSERT_NOT_NULL(p1); ASSERT_NOT_NULL(p2);
    ASSERT_NOT_NULL(p3); ASSERT_NOT_NULL(p4);
    pmm_free_page(p1);
    pmm_free_page(p2);
    pmm_free_page(p3);
    void *block = pmm_alloc_pages(3);
    ASSERT_NOT_NULL(block);
    teardown_region();
}

/* --- mem_track self-tests --- */

TEST(mem_track_tracks_live_and_released)
{
    /* mt_reset was called by the runner; start from zero. */
    ASSERT_EQ_U(mt_live_count(), 0);

    void *a = TRACKED_ALLOC(64);
    void *b = TRACKED_ALLOC(128);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT_EQ_U(mt_live_count(), 2);

    TRACKED_FREE(a);
    ASSERT_EQ_U(mt_live_count(), 1);

    TRACKED_FREE(b);
    ASSERT_EQ_U(mt_live_count(), 0);
}

TEST(mem_track_zero_initializes_payload)
{
    unsigned char *p = TRACKED_ALLOC(32);
    ASSERT_NOT_NULL(p);
    for (int i = 0; i < 32; i++)
        ASSERT_EQ_U(p[i], 0);
    TRACKED_FREE(p);
}
