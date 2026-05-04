/*
 * Host tests for components/sched_priority/ (slice 8.4).
 *
 * Three groups:
 *
 *   1. Conformance — seven TEST()s wrapping the universal helpers from
 *      test/host/conformance/conformance_scheduler.{h,c}.
 *
 *   2. Priority-specific behaviour — set_priority works (unlike sched_rr
 *      which returns NX_EINVAL uniformly); pick_next honours priority
 *      order; out-of-range priority is rejected.
 *
 *   3. Lifecycle cycling — 100× init→enable→disable→destroy with zero
 *      residue.
 */

#include "test_runner.h"

#include "conformance/conformance_scheduler.h"
#include "framework/component.h"
#include "framework/registry.h"
#include "interfaces/scheduler.h"

#include <stdlib.h>
#include <string.h>

extern const struct nx_scheduler_ops        sched_priority_scheduler_ops;
extern const struct nx_component_ops        sched_priority_component_ops;
extern const struct nx_component_descriptor sched_priority_descriptor;

/* ------------------------------------------------------------------ */

static void *sched_priority_fixture_create(void)
{
    void *state = calloc(1, sched_priority_descriptor.state_size);
    if (!state) return NULL;
    if (sched_priority_component_ops.init(state) != NX_OK) {
        free(state);
        return NULL;
    }
    return state;
}

static void sched_priority_fixture_destroy(void *self)
{
    sched_priority_component_ops.destroy(self);
    free(self);
}

static const struct nx_scheduler_fixture sched_priority_fixture = {
    .ops     = &sched_priority_scheduler_ops,
    .create  = sched_priority_fixture_create,
    .destroy = sched_priority_fixture_destroy,
};

/* --- 1. Conformance --------------------------------------------------- */

TEST(sched_priority_conformance_pick_on_empty_returns_null)
{
    nx_conformance_scheduler_pick_on_empty_returns_null(&sched_priority_fixture);
}

TEST(sched_priority_conformance_enqueue_then_pick_returns_task)
{
    nx_conformance_scheduler_enqueue_then_pick_returns_task(&sched_priority_fixture);
}

TEST(sched_priority_conformance_dequeue_removes)
{
    nx_conformance_scheduler_dequeue_removes(&sched_priority_fixture);
}

TEST(sched_priority_conformance_dequeue_nonexistent_returns_enoent)
{
    nx_conformance_scheduler_dequeue_nonexistent_returns_enoent(&sched_priority_fixture);
}

TEST(sched_priority_conformance_set_priority_returns_consistent_status)
{
    nx_conformance_scheduler_set_priority_returns_consistent_status(&sched_priority_fixture);
}

TEST(sched_priority_conformance_roundtrip_100_residue_free)
{
    nx_conformance_scheduler_roundtrip_100_residue_free(&sched_priority_fixture);
}

TEST(sched_priority_conformance_borrow_preserves_task_pointer)
{
    nx_conformance_scheduler_borrow_preserves_task_pointer(&sched_priority_fixture);
}

/* --- 2. Priority-specific behaviour ---------------------------------- */

static void init_task(struct nx_task *t, uint32_t id)
{
    memset(t, 0, sizeof *t);
    t->id    = id;
    t->state = NX_TASK_READY;
    t->sched_node.next = &t->sched_node;
    t->sched_node.prev = &t->sched_node;
}

TEST(sched_priority_set_priority_accepts_valid_range)
{
    void *s = sched_priority_fixture_create();
    ASSERT_NOT_NULL(s);

    struct nx_task t;
    init_task(&t, 1);
    ASSERT_EQ_U(sched_priority_scheduler_ops.enqueue(s, &t), NX_OK);

    /* All levels 0..7 must be accepted. */
    for (int p = 0; p < 8; p++)
        ASSERT_EQ_U(sched_priority_scheduler_ops.set_priority(s, &t, p), NX_OK);

    sched_priority_scheduler_ops.dequeue(s, &t);
    sched_priority_fixture_destroy(s);
}

TEST(sched_priority_set_priority_rejects_out_of_range)
{
    void *s = sched_priority_fixture_create();
    ASSERT_NOT_NULL(s);

    struct nx_task t;
    init_task(&t, 2);
    ASSERT_EQ_U(sched_priority_scheduler_ops.enqueue(s, &t), NX_OK);

    ASSERT_EQ_U((unsigned)sched_priority_scheduler_ops.set_priority(s, &t, -1),
                (unsigned)NX_EINVAL);
    ASSERT_EQ_U((unsigned)sched_priority_scheduler_ops.set_priority(s, &t, 8),
                (unsigned)NX_EINVAL);
    ASSERT_EQ_U((unsigned)sched_priority_scheduler_ops.set_priority(s, &t, 100),
                (unsigned)NX_EINVAL);

    sched_priority_scheduler_ops.dequeue(s, &t);
    sched_priority_fixture_destroy(s);
}

TEST(sched_priority_set_priority_on_non_queued_returns_enoent)
{
    void *s = sched_priority_fixture_create();
    ASSERT_NOT_NULL(s);

    struct nx_task t;
    init_task(&t, 3);
    /* Not enqueued — set_priority must return ENOENT. */
    ASSERT_EQ_U((unsigned)sched_priority_scheduler_ops.set_priority(s, &t, 1),
                (unsigned)NX_ENOENT);

    sched_priority_fixture_destroy(s);
}

TEST(sched_priority_pick_honours_priority_order)
{
    /* Enqueue three tasks at different priorities; pick_next must
     * return the highest-priority one. */
    void *s = sched_priority_fixture_create();
    ASSERT_NOT_NULL(s);

    struct nx_task lo, mid, hi;
    init_task(&lo,  10);
    init_task(&mid, 11);
    init_task(&hi,  12);

    ASSERT_EQ_U(sched_priority_scheduler_ops.enqueue(s, &lo),  NX_OK);
    ASSERT_EQ_U(sched_priority_scheduler_ops.enqueue(s, &mid), NX_OK);
    ASSERT_EQ_U(sched_priority_scheduler_ops.enqueue(s, &hi),  NX_OK);

    /* lo stays at 0, mid → 3, hi → 7 */
    ASSERT_EQ_U(sched_priority_scheduler_ops.set_priority(s, &mid, 3), NX_OK);
    ASSERT_EQ_U(sched_priority_scheduler_ops.set_priority(s, &hi,  7), NX_OK);

    ASSERT_EQ_PTR(sched_priority_scheduler_ops.pick_next(s), &hi);

    /* Remove hi; now mid is highest. */
    ASSERT_EQ_U(sched_priority_scheduler_ops.dequeue(s, &hi), NX_OK);
    ASSERT_EQ_PTR(sched_priority_scheduler_ops.pick_next(s), &mid);

    /* Remove mid; lo is all that remains. */
    ASSERT_EQ_U(sched_priority_scheduler_ops.dequeue(s, &mid), NX_OK);
    ASSERT_EQ_PTR(sched_priority_scheduler_ops.pick_next(s), &lo);

    sched_priority_scheduler_ops.dequeue(s, &lo);
    sched_priority_fixture_destroy(s);
}

TEST(sched_priority_same_priority_fifo_order)
{
    /* Two tasks at the same priority must be picked in FIFO order. */
    void *s = sched_priority_fixture_create();
    ASSERT_NOT_NULL(s);

    struct nx_task a, b;
    init_task(&a, 20);
    init_task(&b, 21);

    ASSERT_EQ_U(sched_priority_scheduler_ops.enqueue(s, &a), NX_OK);
    ASSERT_EQ_U(sched_priority_scheduler_ops.enqueue(s, &b), NX_OK);

    /* Both at default priority (0); a was enqueued first. */
    ASSERT_EQ_PTR(sched_priority_scheduler_ops.pick_next(s), &a);

    sched_priority_scheduler_ops.dequeue(s, &a);
    sched_priority_scheduler_ops.dequeue(s, &b);
    sched_priority_fixture_destroy(s);
}

TEST(sched_priority_set_priority_moves_to_correct_bucket)
{
    /* After set_priority, pick_next must reflect the new position. */
    void *s = sched_priority_fixture_create();
    ASSERT_NOT_NULL(s);

    struct nx_task a, b;
    init_task(&a, 30);
    init_task(&b, 31);

    ASSERT_EQ_U(sched_priority_scheduler_ops.enqueue(s, &a), NX_OK);
    ASSERT_EQ_U(sched_priority_scheduler_ops.enqueue(s, &b), NX_OK);

    /* a and b both at 0; pick returns a. */
    ASSERT_EQ_PTR(sched_priority_scheduler_ops.pick_next(s), &a);

    /* Promote b to priority 5; now pick must return b. */
    ASSERT_EQ_U(sched_priority_scheduler_ops.set_priority(s, &b, 5), NX_OK);
    ASSERT_EQ_PTR(sched_priority_scheduler_ops.pick_next(s), &b);

    /* Demote b back to 0; pick returns a again (FIFO: a was there first). */
    ASSERT_EQ_U(sched_priority_scheduler_ops.set_priority(s, &b, 0), NX_OK);
    ASSERT_EQ_PTR(sched_priority_scheduler_ops.pick_next(s), &a);

    sched_priority_scheduler_ops.dequeue(s, &a);
    sched_priority_scheduler_ops.dequeue(s, &b);
    sched_priority_fixture_destroy(s);
}

TEST(sched_priority_blocked_task_skipped_by_pick)
{
    void *s = sched_priority_fixture_create();
    ASSERT_NOT_NULL(s);

    struct nx_task hi, lo;
    init_task(&hi, 40);
    init_task(&lo, 41);

    ASSERT_EQ_U(sched_priority_scheduler_ops.enqueue(s, &hi), NX_OK);
    ASSERT_EQ_U(sched_priority_scheduler_ops.enqueue(s, &lo), NX_OK);
    ASSERT_EQ_U(sched_priority_scheduler_ops.set_priority(s, &hi, 7), NX_OK);

    /* Mark hi BLOCKED — pick_next must skip it and return lo. */
    hi.state = NX_TASK_BLOCKED;
    ASSERT_EQ_PTR(sched_priority_scheduler_ops.pick_next(s), &lo);

    /* Unblock hi — pick_next returns hi again. */
    hi.state = NX_TASK_READY;
    ASSERT_EQ_PTR(sched_priority_scheduler_ops.pick_next(s), &hi);

    sched_priority_scheduler_ops.dequeue(s, &hi);
    sched_priority_scheduler_ops.dequeue(s, &lo);
    sched_priority_fixture_destroy(s);
}

/* --- 3. Lifecycle cycling -------------------------------------------- */

TEST(sched_priority_lifecycle_100_cycles_leave_no_residue)
{
    void *state = calloc(1, sched_priority_descriptor.state_size);
    ASSERT_NOT_NULL(state);

    enum { N = 100 };
    for (int i = 0; i < N; i++) {
        ASSERT_EQ_U(sched_priority_component_ops.init(state),    NX_OK);
        ASSERT_EQ_U(sched_priority_component_ops.enable(state),  NX_OK);
        ASSERT_EQ_U(sched_priority_component_ops.disable(state), NX_OK);
        sched_priority_component_ops.destroy(state);
    }

    free(state);
}

TEST(sched_priority_init_yields_pickable_empty_queue)
{
    void *state = calloc(1, sched_priority_descriptor.state_size);
    ASSERT_NOT_NULL(state);
    ASSERT_EQ_U(sched_priority_component_ops.init(state), NX_OK);
    ASSERT_NULL(sched_priority_scheduler_ops.pick_next(state));
    sched_priority_component_ops.destroy(state);
    free(state);
}
