/*
 * Host test that exercises the scheduler conformance harness (slice 4.2).
 *
 * Two purposes:
 *   1. Smoke-test the harness itself by running every universal case
 *      against a trivially-correct in-file FIFO fixture.  If the harness
 *      is buggy it fails here rather than later against sched_rr.
 *   2. Serve as a worked example / README-by-code for the slice-4.3
 *      component test: `components/sched_rr/` will have the same
 *      shape — one TEST() per conformance helper, delegating to the
 *      fixture.
 *
 * The fixture (sched_fifo) is the world's dumbest correct scheduler:
 * FIFO enqueue/pick/dequeue via an intrusive list, no priorities.
 * Not wired into any component and not linked into the kernel build —
 * pure test-only code.
 */

#include "test_runner.h"
#include "conformance/conformance_scheduler.h"

#include "interfaces/scheduler.h"
#include "framework/registry.h"
#include "core/sched/task.h"
#include "core/lib/list.h"

#include <stdlib.h>

/* --- sched_fifo: test-only FIFO scheduler fixture ------------------- */

struct sched_fifo {
    struct nx_list_head runqueue;
};

static struct nx_task *fifo_pick_next(void *self)
{
    struct sched_fifo *s = self;
    if (nx_list_empty(&s->runqueue)) return NULL;
    struct nx_list_node *n = s->runqueue.n.next;
    return nx_list_entry(n, struct nx_task, sched_node);
}

/* Linear scan to detect a duplicate enqueue (needed for NX_EEXIST in
 * the interface contract — even though the conformance suite doesn't
 * exercise duplicate-enqueue today, a correct fixture implements it). */
static int fifo_is_on_queue(struct sched_fifo *s, struct nx_task *t)
{
    struct nx_list_node *n;
    nx_list_for_each(n, &s->runqueue) {
        if (n == &t->sched_node) return 1;
    }
    return 0;
}

static int fifo_enqueue(void *self, struct nx_task *task)
{
    if (!task) return NX_EINVAL;
    struct sched_fifo *s = self;
    if (fifo_is_on_queue(s, task)) return NX_EEXIST;
    nx_list_add_tail(&s->runqueue, &task->sched_node);
    return NX_OK;
}

static int fifo_dequeue(void *self, struct nx_task *task)
{
    if (!task) return NX_EINVAL;
    struct sched_fifo *s = self;
    if (!fifo_is_on_queue(s, task)) return NX_ENOENT;
    nx_list_remove(&task->sched_node);
    return NX_OK;
}

static void fifo_yield(void *self) { (void)self; }

static int fifo_set_priority(void *self, struct nx_task *task, int prio)
{
    (void)self; (void)task; (void)prio;
    /* Round-robin / FIFO: priorities not supported → NX_EINVAL per
     * the scheduler.h contract. */
    return NX_EINVAL;
}

static void fifo_tick(void *self) { (void)self; }

static const struct nx_scheduler_ops sched_fifo_ops = {
    .pick_next    = fifo_pick_next,
    .enqueue      = fifo_enqueue,
    .dequeue      = fifo_dequeue,
    .yield        = fifo_yield,
    .set_priority = fifo_set_priority,
    .tick         = fifo_tick,
};

static void *sched_fifo_create(void)
{
    struct sched_fifo *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    nx_list_init(&s->runqueue);
    return s;
}

static void sched_fifo_destroy(void *self)
{
    free(self);
}

static const struct nx_scheduler_fixture sched_fifo_fixture = {
    .ops     = &sched_fifo_ops,
    .create  = sched_fifo_create,
    .destroy = sched_fifo_destroy,
};

/* --- conformance TEST wrappers -------------------------------------- */

TEST(sched_fifo_conformance_pick_on_empty_returns_null)
{
    nx_conformance_scheduler_pick_on_empty_returns_null(&sched_fifo_fixture);
}

TEST(sched_fifo_conformance_enqueue_then_pick_returns_task)
{
    nx_conformance_scheduler_enqueue_then_pick_returns_task(&sched_fifo_fixture);
}

TEST(sched_fifo_conformance_dequeue_removes)
{
    nx_conformance_scheduler_dequeue_removes(&sched_fifo_fixture);
}

TEST(sched_fifo_conformance_dequeue_nonexistent_returns_enoent)
{
    nx_conformance_scheduler_dequeue_nonexistent_returns_enoent(&sched_fifo_fixture);
}

TEST(sched_fifo_conformance_set_priority_returns_consistent_status)
{
    nx_conformance_scheduler_set_priority_returns_consistent_status(&sched_fifo_fixture);
}

TEST(sched_fifo_conformance_roundtrip_100_residue_free)
{
    nx_conformance_scheduler_roundtrip_100_residue_free(&sched_fifo_fixture);
}

TEST(sched_fifo_conformance_borrow_preserves_task_pointer)
{
    nx_conformance_scheduler_borrow_preserves_task_pointer(&sched_fifo_fixture);
}

/* --- local FIFO unit tests (sanity-check the fixture, independent of
 * the harness — if these pass but the conformance cases fail, the bug
 * is in the harness; if these fail the fixture is broken) */

TEST(sched_fifo_internal_empty_queue_is_detected)
{
    struct sched_fifo *s = sched_fifo_create();
    ASSERT_NOT_NULL(s);
    ASSERT_NULL(sched_fifo_ops.pick_next(s));
    sched_fifo_destroy(s);
}

TEST(sched_fifo_internal_duplicate_enqueue_is_eexist)
{
    struct sched_fifo *s = sched_fifo_create();
    ASSERT_NOT_NULL(s);
    struct nx_task t = {0};
    t.sched_node.next = &t.sched_node;
    t.sched_node.prev = &t.sched_node;
    ASSERT_EQ_U(sched_fifo_ops.enqueue(s, &t), NX_OK);
    ASSERT_EQ_U((unsigned)sched_fifo_ops.enqueue(s, &t), (unsigned)NX_EEXIST);
    sched_fifo_ops.dequeue(s, &t);
    sched_fifo_destroy(s);
}
