#ifndef NONUX_INTERFACE_SCHEDULER_H
#define NONUX_INTERFACE_SCHEDULER_H

#include "core/sched/task.h"

/*
 * Scheduler interface.
 *
 * Every scheduler policy component (e.g. components/sched_rr/) implements
 * a `const struct nx_scheduler_ops` table and a matching `void *self`
 * instance.  The core scheduler driver (core/sched/) holds both in a
 * stashed pointer pair after `sched_init(ops, self)` runs at the end of
 * boot bring-up — see DESIGN.md §Scheduler: Core Driver + Component.
 *
 * Ownership.
 *   Every `struct nx_task *` parameter is `borrow`: the scheduler keeps
 *   the pointer valid while the task sits on its runqueue, but never
 *   frees the storage.  Tasks are created / destroyed through the core
 *   driver (`nx_task_create` / `nx_task_destroy`); policy impls only
 *   link/unlink them on their internal queues.
 *
 * Concurrency.
 *   Phase 4 is single-CPU, so every op is called by the per-CPU
 *   dispatcher (slice 3.9b) or the IRQ-return reschedule shim.
 *   Implementations don't need internal locks in v1 but must not
 *   block — every op is bounded, non-preemptible, and fast.
 *
 * Error convention.
 *   `int` returns use NX_OK / NX_E* from framework/registry.h.  See
 *   the individual op docs below for the per-op status set.
 */

struct nx_scheduler_ops {
    /*
     * Return the task that should run next on this CPU, or NULL if the
     * runqueue is empty.  Borrow.  The scheduler neither advances its
     * internal "current" marker nor dequeues the returned task — the
     * driver decides whether to switch to it and may call pick_next
     * repeatedly without side effects (idempotent against the same
     * queue state).
     *
     * Implementations must be bounded O(n) in runqueue length and
     * preserve FIFO-equivalence within the same priority level when a
     * policy exposes priorities.
     */
    struct nx_task *(*pick_next)(void *self);

    /*
     * Add `task` to the runqueue.  Borrow.
     *
     * Returns:
     *   NX_OK       — task queued.
     *   NX_EEXIST   — task already on the runqueue (double-enqueue).
     *   NX_EINVAL   — NULL task.
     */
    int (*enqueue)(void *self, struct nx_task *task);

    /*
     * Remove `task` from the runqueue.  Borrow.
     *
     * Returns:
     *   NX_OK      — removed.
     *   NX_ENOENT  — task is not on the runqueue.
     *   NX_EINVAL  — NULL task.
     */
    int (*dequeue)(void *self, struct nx_task *task);

    /*
     * The current task voluntarily gives up the rest of its quantum.
     * Slice 4.4 wires this into the reschedule shim.  In Phase 4 it
     * may no-op if the runqueue has no other runnable tasks (caller
     * loop back through pick_next handles that case).
     */
    void (*yield)(void *self);

    /*
     * Set `task`'s priority.  `priority` is policy-defined; the only
     * universal contract is "higher number = higher priority, zero is
     * the default."
     *
     * Returns:
     *   NX_OK       — policy implements priorities and accepted the value.
     *   NX_EINVAL   — policy does not support priorities (round-robin),
     *                 or the value is out of range.  Callers can treat
     *                 NX_EINVAL as "priorities not honoured by this
     *                 policy" without distinguishing the two sub-cases.
     *   NX_ENOENT   — task is not on the runqueue.
     */
    int (*set_priority)(void *self, struct nx_task *task, int priority);

    /*
     * Called once per timer tick from the core driver's `sched_tick`
     * (slice 4.4).  The policy typically decrements the current task's
     * remaining quantum and sets `need_resched` when it reaches zero —
     * the core driver reads `current->need_resched` at IRQ-return time
     * and reschedules there.
     *
     * Must be bounded and non-blocking; runs with preemption disabled
     * on the interrupted task's kernel stack.
     */
    void (*tick)(void *self);
};

#endif /* NONUX_INTERFACE_SCHEDULER_H */
