#ifndef NONUX_CONFORMANCE_SCHEDULER_H
#define NONUX_CONFORMANCE_SCHEDULER_H

#include "interfaces/scheduler.h"

/*
 * Scheduler conformance suite (slice 4.2).
 *
 * Every scheduler policy component (sched_rr, future sched_priority, …)
 * must pass every case below before it is allowed to bind to the
 * `scheduler` slot in a production kernel.com.  The cases encode the
 * universal contract of `struct nx_scheduler_ops`: anything
 * policy-specific (FIFO order for round-robin, priority inversion
 * handling for priority schedulers) is tested separately in the
 * component's own test file.
 *
 * Usage.
 *   A component's test file defines one TEST() per case, each of
 *   which calls the matching helper below against a fixture that
 *   knows how to create / destroy a fresh policy instance.  Seven
 *   helpers → seven tests per policy.  Per-case isolation gives
 *   clean pass/fail reporting through the host test framework.
 *
 *   Example:
 *     static const struct nx_scheduler_fixture rr_fixture = {
 *         .ops = &sched_rr_ops,
 *         .create = sched_rr_fixture_create,
 *         .destroy = sched_rr_fixture_destroy,
 *     };
 *     TEST(sched_rr_pick_on_empty_returns_null) {
 *         nx_conformance_scheduler_pick_on_empty_returns_null(&rr_fixture);
 *     }
 *     ...
 */

struct nx_scheduler_fixture {
    const struct nx_scheduler_ops *ops;
    void *(*create)(void);           /* fresh policy self; may return NULL on OOM */
    void  (*destroy)(void *self);    /* must release everything `create` allocated */
};

/*
 * Universal cases — must hold for every scheduler policy.
 *
 * Each helper calls `fixture->create`, exercises the op table, then
 * `fixture->destroy`.  Failures are reported through ASSERT() from
 * the host test framework, so the calling TEST() short-circuits on
 * the first bad assertion.
 */
void nx_conformance_scheduler_pick_on_empty_returns_null(
    const struct nx_scheduler_fixture *f);

void nx_conformance_scheduler_enqueue_then_pick_returns_task(
    const struct nx_scheduler_fixture *f);

void nx_conformance_scheduler_dequeue_removes(
    const struct nx_scheduler_fixture *f);

void nx_conformance_scheduler_dequeue_nonexistent_returns_enoent(
    const struct nx_scheduler_fixture *f);

void nx_conformance_scheduler_set_priority_returns_consistent_status(
    const struct nx_scheduler_fixture *f);

void nx_conformance_scheduler_roundtrip_100_residue_free(
    const struct nx_scheduler_fixture *f);

void nx_conformance_scheduler_borrow_preserves_task_pointer(
    const struct nx_scheduler_fixture *f);

#endif /* NONUX_CONFORMANCE_SCHEDULER_H */
