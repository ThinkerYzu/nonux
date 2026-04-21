/*
 * Scheduler conformance suite — implementation (slice 4.2).
 *
 * See test/host/conformance/conformance_scheduler.h for the usage
 * contract.  Each helper exercises one invariant of
 * `struct nx_scheduler_ops`; callers wrap them in TEST()s.
 *
 * Task allocation.  Conformance cases use minimal stack/static
 * `struct nx_task` instances rather than `nx_task_create` — the
 * scheduler only touches `sched_node`, `id`, and `state`, so we skip
 * the kstack + ID-seq machinery and stay focused on the interface
 * contract.  Each case local-inits its tasks via `init_test_task`.
 */

#include "conformance_scheduler.h"

#include "test/host/test_runner.h"
#include "framework/registry.h"   /* NX_OK / NX_E* */
#include "core/sched/task.h"

#include <string.h>

static void init_test_task(struct nx_task *t, uint32_t id)
{
    memset(t, 0, sizeof *t);
    t->id = id;
    t->state = NX_TASK_READY;
    t->sched_node.next = &t->sched_node;
    t->sched_node.prev = &t->sched_node;
}

/* --- case 1: empty runqueue --- */

void nx_conformance_scheduler_pick_on_empty_returns_null(
    const struct nx_scheduler_fixture *f)
{
    ASSERT_NOT_NULL(f);
    ASSERT_NOT_NULL(f->ops);
    ASSERT_NOT_NULL(f->ops->pick_next);

    void *self = f->create();
    ASSERT_NOT_NULL(self);

    ASSERT_NULL(f->ops->pick_next(self));

    f->destroy(self);
}

/* --- case 2: enqueue → pick returns that task --- */

void nx_conformance_scheduler_enqueue_then_pick_returns_task(
    const struct nx_scheduler_fixture *f)
{
    ASSERT_NOT_NULL(f);
    ASSERT_NOT_NULL(f->ops);
    ASSERT_NOT_NULL(f->ops->enqueue);
    ASSERT_NOT_NULL(f->ops->pick_next);

    void *self = f->create();
    ASSERT_NOT_NULL(self);

    struct nx_task t;
    init_test_task(&t, 101);

    ASSERT_EQ_U(f->ops->enqueue(self, &t), NX_OK);
    ASSERT_EQ_PTR(f->ops->pick_next(self), &t);

    /* Dequeue before destroy so the policy is left clean for the
     * residue check case — not strictly required for this case, but
     * cheap and a reasonable habit. */
    if (f->ops->dequeue)
        f->ops->dequeue(self, &t);

    f->destroy(self);
}

/* --- case 3: dequeue actually removes --- */

void nx_conformance_scheduler_dequeue_removes(
    const struct nx_scheduler_fixture *f)
{
    ASSERT_NOT_NULL(f);
    ASSERT_NOT_NULL(f->ops->enqueue);
    ASSERT_NOT_NULL(f->ops->dequeue);
    ASSERT_NOT_NULL(f->ops->pick_next);

    void *self = f->create();
    ASSERT_NOT_NULL(self);

    struct nx_task t;
    init_test_task(&t, 201);

    ASSERT_EQ_U(f->ops->enqueue(self, &t), NX_OK);
    ASSERT_EQ_PTR(f->ops->pick_next(self), &t);

    ASSERT_EQ_U(f->ops->dequeue(self, &t), NX_OK);
    ASSERT_NULL(f->ops->pick_next(self));

    f->destroy(self);
}

/* --- case 4: dequeue of never-queued task returns NX_ENOENT --- */

void nx_conformance_scheduler_dequeue_nonexistent_returns_enoent(
    const struct nx_scheduler_fixture *f)
{
    ASSERT_NOT_NULL(f);
    ASSERT_NOT_NULL(f->ops->enqueue);
    ASSERT_NOT_NULL(f->ops->dequeue);

    void *self = f->create();
    ASSERT_NOT_NULL(self);

    struct nx_task queued, never_queued;
    init_test_task(&queued, 301);
    init_test_task(&never_queued, 302);

    ASSERT_EQ_U(f->ops->enqueue(self, &queued), NX_OK);

    /* Dequeue a task that was never enqueued — must surface ENOENT,
     * not NX_OK and not a crash.  Exact code is part of the
     * interface contract (scheduler.h). */
    int rc = f->ops->dequeue(self, &never_queued);
    ASSERT_EQ_U((unsigned)rc, (unsigned)NX_ENOENT);

    /* And dequeuing the queued task still works (policy didn't
     * wedge itself). */
    ASSERT_EQ_U(f->ops->dequeue(self, &queued), NX_OK);

    f->destroy(self);
}

/* --- case 5: set_priority is consistent — NX_OK or uniformly NX_EINVAL --- */

void nx_conformance_scheduler_set_priority_returns_consistent_status(
    const struct nx_scheduler_fixture *f)
{
    ASSERT_NOT_NULL(f);
    ASSERT_NOT_NULL(f->ops->enqueue);
    ASSERT_NOT_NULL(f->ops->set_priority);

    void *self = f->create();
    ASSERT_NOT_NULL(self);

    struct nx_task a, b;
    init_test_task(&a, 401);
    init_test_task(&b, 402);

    ASSERT_EQ_U(f->ops->enqueue(self, &a), NX_OK);
    ASSERT_EQ_U(f->ops->enqueue(self, &b), NX_OK);

    int ra = f->ops->set_priority(self, &a, 1);
    int rb = f->ops->set_priority(self, &b, 1);

    /* Contract from scheduler.h: policies that don't support
     * priorities must return NX_EINVAL uniformly.  Policies that do
     * support them must accept at least priority == 1 (a middling
     * value all policies should accept), so returning NX_OK is also
     * acceptable — but ra and rb must agree, the policy must not be
     * "priorities for some tasks but not others." */
    ASSERT_EQ_U((unsigned)ra, (unsigned)rb);
    ASSERT(ra == NX_OK || ra == NX_EINVAL);

    if (f->ops->dequeue) {
        f->ops->dequeue(self, &a);
        f->ops->dequeue(self, &b);
    }
    f->destroy(self);
}

/* --- case 6: 100-task round-trip leaves no residue --- */

void nx_conformance_scheduler_roundtrip_100_residue_free(
    const struct nx_scheduler_fixture *f)
{
    ASSERT_NOT_NULL(f);
    ASSERT_NOT_NULL(f->ops->enqueue);
    ASSERT_NOT_NULL(f->ops->dequeue);
    ASSERT_NOT_NULL(f->ops->pick_next);

    void *self = f->create();
    ASSERT_NOT_NULL(self);

    enum { N = 100 };
    static struct nx_task tasks[N];
    for (int i = 0; i < N; i++) {
        init_test_task(&tasks[i], (uint32_t)(1000 + i));
        ASSERT_EQ_U(f->ops->enqueue(self, &tasks[i]), NX_OK);
    }

    /* Dequeue every task — order is policy-defined so we can't check
     * pick_next's return order here, but we can check that after N
     * dequeues the queue is empty. */
    for (int i = 0; i < N; i++)
        ASSERT_EQ_U(f->ops->dequeue(self, &tasks[i]), NX_OK);

    ASSERT_NULL(f->ops->pick_next(self));

    /* destroy must succeed cleanly — the mem_track layer will fail
     * the calling TEST() if anything leaked. */
    f->destroy(self);
}

/* --- case 7: borrow — enqueue must not mutate task storage --- */

void nx_conformance_scheduler_borrow_preserves_task_pointer(
    const struct nx_scheduler_fixture *f)
{
    ASSERT_NOT_NULL(f);
    ASSERT_NOT_NULL(f->ops->enqueue);
    ASSERT_NOT_NULL(f->ops->pick_next);

    void *self = f->create();
    ASSERT_NOT_NULL(self);

    struct nx_task t;
    init_test_task(&t, 501);
    t.id = 0xDEADBEEF;
    memcpy(t.name, "borrowme", sizeof "borrowme");
    t.preempt_count = 7;

    ASSERT_EQ_U(f->ops->enqueue(self, &t), NX_OK);

    /* pick_next must return the same pointer, not a copy. */
    struct nx_task *got = f->ops->pick_next(self);
    ASSERT_EQ_PTR(got, &t);

    /* Caller-visible fields the scheduler promises not to touch
     * (borrow — scheduler may only read, and may only mutate
     * sched_node for its own queue bookkeeping). */
    ASSERT_EQ_U(t.id, 0xDEADBEEF);
    ASSERT_EQ_U(t.preempt_count, 7);
    ASSERT_EQ_U((unsigned)t.state, (unsigned)NX_TASK_READY);
    ASSERT(memcmp(t.name, "borrowme", sizeof "borrowme") == 0);

    if (f->ops->dequeue)
        f->ops->dequeue(self, &t);
    f->destroy(self);
}
