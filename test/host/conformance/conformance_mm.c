/*
 * Memory-allocator conformance suite — implementation (slice 5.2).
 *
 * See conformance_mm.h for the usage contract.  Each helper exercises
 * one invariant of `struct nx_mm_ops`; callers wrap them in TEST()s.
 */

#include "conformance_mm.h"

#include "test/host/test_runner.h"

#include <stdint.h>

/* --- case 1: page_size is a positive power of two --------------------- */

void nx_conformance_mm_page_size_is_positive_power_of_two(
    const struct nx_mm_fixture *f)
{
    ASSERT_NOT_NULL(f);
    ASSERT_NOT_NULL(f->ops);
    ASSERT_NOT_NULL(f->ops->page_size);

    void *self = f->create();
    ASSERT_NOT_NULL(self);

    size_t ps = f->ops->page_size(self);
    ASSERT(ps > 0);
    /* Power of two: ps & (ps - 1) == 0. */
    ASSERT_EQ_U(ps & (ps - 1), 0);

    f->destroy(self);
}

/* --- case 2: max_order > 0, and order-max alloc succeeds -------------- */

void nx_conformance_mm_max_order_is_supported(
    const struct nx_mm_fixture *f)
{
    ASSERT_NOT_NULL(f);
    ASSERT_NOT_NULL(f->ops->max_order);
    ASSERT_NOT_NULL(f->ops->alloc_pages);
    ASSERT_NOT_NULL(f->ops->free_pages);

    void *self = f->create();
    ASSERT_NOT_NULL(self);

    unsigned max_order = f->ops->max_order(self);
    ASSERT(max_order > 0);

    void *p = f->ops->alloc_pages(self, max_order);
    ASSERT_NOT_NULL(p);
    f->ops->free_pages(self, p, max_order);

    f->destroy(self);
}

/* --- case 3: alloc(0) returns a page-aligned non-NULL pointer --------- */

void nx_conformance_mm_alloc_order_zero_is_page_aligned_and_nonnull(
    const struct nx_mm_fixture *f)
{
    void *self = f->create();
    ASSERT_NOT_NULL(self);

    size_t ps = f->ops->page_size(self);
    void *p = f->ops->alloc_pages(self, 0);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_U((uintptr_t)p & (ps - 1), 0);

    f->ops->free_pages(self, p, 0);
    f->destroy(self);
}

/* --- case 4: two alloc(0) calls return distinct, non-overlapping pages */

void nx_conformance_mm_alloc_order_zero_twice_returns_distinct_pages(
    const struct nx_mm_fixture *f)
{
    void *self = f->create();
    ASSERT_NOT_NULL(self);

    size_t ps = f->ops->page_size(self);
    void *a = f->ops->alloc_pages(self, 0);
    void *b = f->ops->alloc_pages(self, 0);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT(a != b);

    /* Non-overlap: the pointers are at least page_size apart. */
    uintptr_t ua = (uintptr_t)a;
    uintptr_t ub = (uintptr_t)b;
    uintptr_t diff = ua < ub ? (ub - ua) : (ua - ub);
    ASSERT(diff >= ps);

    f->ops->free_pages(self, a, 0);
    f->ops->free_pages(self, b, 0);
    f->destroy(self);
}

/* --- case 5: alloc at order > max_order returns NULL ------------------ */

void nx_conformance_mm_alloc_beyond_max_order_returns_null(
    const struct nx_mm_fixture *f)
{
    void *self = f->create();
    ASSERT_NOT_NULL(self);

    unsigned max_order = f->ops->max_order(self);
    ASSERT_NULL(f->ops->alloc_pages(self, max_order + 1));
    ASSERT_NULL(f->ops->alloc_pages(self, max_order + 5));

    f->destroy(self);
}

/* --- case 6: max-order alloc consumes the whole pool ------------------ */

void nx_conformance_mm_max_order_alloc_uses_full_pool(
    const struct nx_mm_fixture *f)
{
    void *self = f->create();
    ASSERT_NOT_NULL(self);

    unsigned max_order = f->ops->max_order(self);

    /* First max-order alloc succeeds. */
    void *p = f->ops->alloc_pages(self, max_order);
    ASSERT_NOT_NULL(p);

    /* Pool is now fully committed — even an order-0 alloc must fail. */
    ASSERT_NULL(f->ops->alloc_pages(self, 0));

    /* Free, then confirm we can alloc max-order again (proves coalescing
     * and destroy-path don't leak the pool). */
    f->ops->free_pages(self, p, max_order);
    void *q = f->ops->alloc_pages(self, max_order);
    ASSERT_NOT_NULL(q);
    f->ops->free_pages(self, q, max_order);

    f->destroy(self);
}

/* --- case 7: alloc/free round-trip coalesces so the pool is whole again */

void nx_conformance_mm_alloc_free_roundtrip_is_coalescing(
    const struct nx_mm_fixture *f)
{
    void *self = f->create();
    ASSERT_NOT_NULL(self);

    unsigned max_order = f->ops->max_order(self);
    size_t page_count = (size_t)1 << max_order;

    /* Allocate every page at order 0, then free them all. */
    enum { MAX_PAGES = 256 };  /* cap for stack array; plenty for max_order ≤ 8 */
    ASSERT(page_count <= MAX_PAGES);
    void *pages[MAX_PAGES];
    for (size_t i = 0; i < page_count; i++) {
        pages[i] = f->ops->alloc_pages(self, 0);
        ASSERT_NOT_NULL(pages[i]);
    }

    /* Pool fully committed — pattern-match case 6. */
    ASSERT_NULL(f->ops->alloc_pages(self, 0));

    for (size_t i = 0; i < page_count; i++)
        f->ops->free_pages(self, pages[i], 0);

    /* After the round trip the allocator must be able to hand out the
     * full pool as a single max-order block again.  If coalescing
     * missed a merge, this returns NULL. */
    void *full = f->ops->alloc_pages(self, max_order);
    ASSERT_NOT_NULL(full);
    f->ops->free_pages(self, full, max_order);

    f->destroy(self);
}
