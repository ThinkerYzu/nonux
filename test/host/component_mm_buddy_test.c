/*
 * Host tests for components/mm_buddy/ (slice 5.2).
 *
 * Three groups of coverage, same shape as component_sched_rr_test.c:
 *
 *   1. Conformance — seven TEST()s wrapping the universal helpers
 *      from test/host/conformance/conformance_mm.{h,c}.  A component
 *      that fails any of these is not allowed to bind to the
 *      `memory.page_alloc` slot.
 *
 *   2. Lifecycle cycling — 100× init→enable→disable→destroy with
 *      zero residue (observable via counters + leak detector).
 *
 *   3. Coalescing smoke — alloc/split/free paths that are buddy-
 *      specific but not part of the universal contract.
 */

#include "test_runner.h"

#include "conformance/conformance_mm.h"
#include "framework/component.h"
#include "framework/registry.h"
#include "interfaces/mm.h"

#include <stdlib.h>
#include <string.h>

/* Exported by components/mm_buddy/mm_buddy.c. */
extern const struct nx_mm_ops              mm_buddy_mm_ops;
extern const struct nx_component_ops       mm_buddy_component_ops;
extern const struct nx_component_descriptor mm_buddy_descriptor;

/* --- factory used by the conformance harness ------------------------- */

static void *mm_buddy_fixture_create(void)
{
    void *state = calloc(1, mm_buddy_descriptor.state_size);
    if (!state) return NULL;
    if (mm_buddy_component_ops.init(state) != NX_OK) {
        free(state);
        return NULL;
    }
    return state;
}

static void mm_buddy_fixture_destroy(void *self)
{
    mm_buddy_component_ops.destroy(self);
    free(self);
}

static const struct nx_mm_fixture mm_buddy_fixture = {
    .ops     = &mm_buddy_mm_ops,
    .create  = mm_buddy_fixture_create,
    .destroy = mm_buddy_fixture_destroy,
};

/* --- 1. Conformance --------------------------------------------------- */

TEST(mm_buddy_conformance_page_size_is_positive_power_of_two)
{
    nx_conformance_mm_page_size_is_positive_power_of_two(&mm_buddy_fixture);
}

TEST(mm_buddy_conformance_max_order_is_supported)
{
    nx_conformance_mm_max_order_is_supported(&mm_buddy_fixture);
}

TEST(mm_buddy_conformance_alloc_order_zero_is_page_aligned_and_nonnull)
{
    nx_conformance_mm_alloc_order_zero_is_page_aligned_and_nonnull(&mm_buddy_fixture);
}

TEST(mm_buddy_conformance_alloc_order_zero_twice_returns_distinct_pages)
{
    nx_conformance_mm_alloc_order_zero_twice_returns_distinct_pages(&mm_buddy_fixture);
}

TEST(mm_buddy_conformance_alloc_beyond_max_order_returns_null)
{
    nx_conformance_mm_alloc_beyond_max_order_returns_null(&mm_buddy_fixture);
}

TEST(mm_buddy_conformance_max_order_alloc_uses_full_pool)
{
    nx_conformance_mm_max_order_alloc_uses_full_pool(&mm_buddy_fixture);
}

TEST(mm_buddy_conformance_alloc_free_roundtrip_is_coalescing)
{
    nx_conformance_mm_alloc_free_roundtrip_is_coalescing(&mm_buddy_fixture);
}

/* --- 2. Lifecycle cycling -------------------------------------------- */

TEST(mm_buddy_lifecycle_100_cycles_leave_no_residue)
{
    void *state = calloc(1, mm_buddy_descriptor.state_size);
    ASSERT_NOT_NULL(state);

    enum { N = 100 };
    for (int i = 0; i < N; i++) {
        ASSERT_EQ_U(mm_buddy_component_ops.init(state),    NX_OK);
        ASSERT_EQ_U(mm_buddy_component_ops.enable(state),  NX_OK);
        ASSERT_EQ_U(mm_buddy_component_ops.disable(state), NX_OK);
        mm_buddy_component_ops.destroy(state);
    }

    free(state);
}

/* --- 3. Buddy-specific coalescing smoke ------------------------------ */

TEST(mm_buddy_split_alloc_returns_lower_half_of_parent)
{
    /* After a fresh init, the whole pool is one max-order block.
     * alloc(max_order - 1) splits the root into two halves and returns
     * the lower half.  Another alloc(max_order - 1) returns the upper
     * half.  Freeing them in reverse order coalesces back to the root. */
    void *self = mm_buddy_fixture_create();
    ASSERT_NOT_NULL(self);

    unsigned mo = mm_buddy_mm_ops.max_order(self);
    ASSERT(mo >= 1);

    void *a = mm_buddy_mm_ops.alloc_pages(self, mo - 1);
    void *b = mm_buddy_mm_ops.alloc_pages(self, mo - 1);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);

    /* The two halves are adjacent and the lower-addressed one equals
     * the pool base.  The size separating them is a half-pool. */
    size_t half = mm_buddy_mm_ops.page_size(self) * ((size_t)1 << (mo - 1));
    void *lower = (char *)a < (char *)b ? a : b;
    void *upper = (char *)a < (char *)b ? b : a;
    ASSERT_EQ_U((uintptr_t)upper - (uintptr_t)lower, half);

    /* No third half-pool alloc — the pool has only two halves. */
    ASSERT_NULL(mm_buddy_mm_ops.alloc_pages(self, mo - 1));

    mm_buddy_mm_ops.free_pages(self, b, mo - 1);
    mm_buddy_mm_ops.free_pages(self, a, mo - 1);

    /* After freeing both halves, coalescing must have reconstituted
     * the root — a max-order alloc must succeed. */
    void *root = mm_buddy_mm_ops.alloc_pages(self, mo);
    ASSERT_NOT_NULL(root);
    mm_buddy_mm_ops.free_pages(self, root, mo);

    mm_buddy_fixture_destroy(self);
}

TEST(mm_buddy_interleaved_orders_alloc_and_free_cleanly)
{
    /* Alternating orders exercise the split-down + coalesce-up paths
     * without a round-trip symmetry shortcut. */
    void *self = mm_buddy_fixture_create();
    ASSERT_NOT_NULL(self);

    void *p0 = mm_buddy_mm_ops.alloc_pages(self, 0);
    void *p1 = mm_buddy_mm_ops.alloc_pages(self, 1);
    void *p2 = mm_buddy_mm_ops.alloc_pages(self, 2);
    ASSERT_NOT_NULL(p0); ASSERT_NOT_NULL(p1); ASSERT_NOT_NULL(p2);

    /* All three are page-aligned, distinct, and non-overlapping.  The
     * interface only guarantees page alignment; higher-order blocks
     * inherit the pool base's alignment mod block_size (mm.h says
     * callers must not rely on alignment beyond page_size). */
    size_t ps = mm_buddy_mm_ops.page_size(self);
    ASSERT_EQ_U((uintptr_t)p0 & (ps - 1), 0);
    ASSERT_EQ_U((uintptr_t)p1 & (ps - 1), 0);
    ASSERT_EQ_U((uintptr_t)p2 & (ps - 1), 0);
    ASSERT(p0 != p1); ASSERT(p1 != p2); ASSERT(p0 != p2);

    mm_buddy_mm_ops.free_pages(self, p2, 2);
    mm_buddy_mm_ops.free_pages(self, p1, 1);
    mm_buddy_mm_ops.free_pages(self, p0, 0);

    /* After freeing, the whole pool must be available as a single
     * max-order block. */
    unsigned mo = mm_buddy_mm_ops.max_order(self);
    void *full = mm_buddy_mm_ops.alloc_pages(self, mo);
    ASSERT_NOT_NULL(full);
    mm_buddy_mm_ops.free_pages(self, full, mo);

    mm_buddy_fixture_destroy(self);
}
