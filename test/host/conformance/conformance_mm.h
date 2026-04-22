#ifndef NONUX_CONFORMANCE_MM_H
#define NONUX_CONFORMANCE_MM_H

#include "interfaces/mm.h"

/*
 * Memory-allocator conformance suite (slice 5.2).
 *
 * Every `memory.page_alloc` component (mm_buddy, future mm_slab, ...)
 * must pass every case below before it is allowed to bind to the
 * `memory.page_alloc` slot in a production kernel.  Cases encode the
 * universal contract of `struct nx_mm_ops`; anything policy-specific
 * (exact free-list layout, coalescing-order, NUMA locality) is tested
 * separately in the component's own test file.
 *
 * Usage.
 *   Define one TEST() per case wrapping each helper against a fixture
 *   that knows how to create / destroy a fresh allocator instance.
 *   Seven helpers → seven tests per component.
 */

struct nx_mm_fixture {
    const struct nx_mm_ops *ops;
    void *(*create)(void);           /* fresh `self`; may return NULL on OOM */
    void  (*destroy)(void *self);    /* must release everything create allocated */
};

/*
 * Universal cases — must hold for every memory allocator policy.
 *
 * Each helper calls `fixture->create`, exercises the op table, then
 * `fixture->destroy`.  Failures surface through ASSERT() from the host
 * test framework so the calling TEST() short-circuits on the first
 * bad assertion.
 */

void nx_conformance_mm_page_size_is_positive_power_of_two(
    const struct nx_mm_fixture *f);

void nx_conformance_mm_max_order_is_supported(
    const struct nx_mm_fixture *f);

void nx_conformance_mm_alloc_order_zero_is_page_aligned_and_nonnull(
    const struct nx_mm_fixture *f);

void nx_conformance_mm_alloc_order_zero_twice_returns_distinct_pages(
    const struct nx_mm_fixture *f);

void nx_conformance_mm_alloc_beyond_max_order_returns_null(
    const struct nx_mm_fixture *f);

void nx_conformance_mm_max_order_alloc_uses_full_pool(
    const struct nx_mm_fixture *f);

void nx_conformance_mm_alloc_free_roundtrip_is_coalescing(
    const struct nx_mm_fixture *f);

#endif /* NONUX_CONFORMANCE_MM_H */
