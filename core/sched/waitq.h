#ifndef NONUX_SCHED_WAITQ_H
#define NONUX_SCHED_WAITQ_H

#include <stdint.h>

#include "core/lib/list.h"

/*
 * Wait-queue primitive — slice 7.8a.
 *
 * Replaces the v1 yield-loop pattern (slice 7.6d.N.6b `sys_read`
 * CHANNEL arm; slice 7.4b `sys_wait`; slice 7.6d.N.final.a
 * `nx_console_read`) with a real kernel blocking primitive.
 *
 * Semantics.
 *   `nx_waitq_wait_with_deadline(wq, budget_ns)` parks the calling
 *   task on `wq` and yields the CPU.  budget_ns == 0 → block
 *   indefinitely (returns when somebody calls wake_one/wake_all
 *   targeting `wq`).  budget_ns > 0 → block for at most that many
 *   nanoseconds; if no wake arrives the timer-tick path
 *   (`nx_waitq_tick_deadlines`, called from `sched_tick`) expires
 *   the wait and the function returns NX_EDEADLINE.
 *
 *   `nx_waitq_wake_one(wq)` releases one waiter, FIFO order
 *   (oldest waiter first).  `nx_waitq_wake_all(wq)` releases every
 *   waiter on `wq` in FIFO order.  Both are safe to call from ISR
 *   context — they save/restore DAIF.I and use preempt-disable
 *   around the runqueue mutation.
 *
 * Storage.
 *   The waitq's intrusive list reuses each task's `sched_node` —
 *   when a task is on a waitq it is NOT on the scheduler runqueue.
 *   Wake or deadline-expire dequeues from `wq->waiters`, sets state
 *   READY, and re-enqueues on the scheduler runqueue.  The task
 *   resumes execution back inside `nx_waitq_wait_with_deadline`,
 *   which inspects `task->wait_woken` to decide NX_OK vs
 *   NX_EDEADLINE.
 *
 *   Tasks with a deadline also link into a global
 *   `g_deadline_list` via `task->deadline_node` so the timer tick
 *   can expire deadlines without walking every waitq in the
 *   system.  Tasks blocked indefinitely (budget_ns == 0) are NOT
 *   on the deadline list.
 */

struct nx_waitq {
    struct nx_list_head waiters;
};

void nx_waitq_init(struct nx_waitq *wq);

/*
 * Block the calling task on `wq`.  Returns:
 *   NX_OK         — woken by `wake_one` / `wake_all`.
 *   NX_EDEADLINE  — deadline elapsed first (only with budget_ns > 0).
 *   NX_EINVAL     — `wq` is NULL or there is no current task.
 *
 * MUST NOT be called from ISR context — the function yields.
 */
int  nx_waitq_wait_with_deadline(struct nx_waitq *wq, uint64_t budget_ns);

/*
 * Predicate-checked wait.  Re-evaluates `pred(ctx)` inside the
 * critical section that registers the caller on `wq` — if it
 * already returns non-zero, the caller is NOT enqueued and the
 * function returns NX_OK immediately (treating the predicate as
 * an already-fired wake).
 *
 * This is the lost-wakeup-safe wait pattern: callers do
 *
 *     while (!cond) {
 *         nx_waitq_wait_unless(&wq, budget, cond_pred, ctx);
 *     }
 *
 * and a wake that fires after the caller's outer `cond` check but
 * before the inner predicate check is caught by the predicate.
 *
 * Same return contract as `wait_with_deadline`.  `pred` may be NULL
 * (degrades to plain `wait_with_deadline` semantics).
 */
int  nx_waitq_wait_unless(struct nx_waitq *wq, uint64_t budget_ns,
                          int (*pred)(void *), void *ctx);

/*
 * Release one / all waiter(s) of `wq`.  No-op if the queue is
 * empty.  ISR-safe.
 */
void nx_waitq_wake_one(struct nx_waitq *wq);
void nx_waitq_wake_all(struct nx_waitq *wq);

/*
 * Walk the global deadline list and expire any waiter whose
 * `wait_deadline` has passed.  Called once per timer tick from
 * `sched_tick`.  ISR-context-safe (sched_tick runs from the timer
 * ISR with IRQs masked).
 */
void nx_waitq_tick_deadlines(void);

#endif /* NONUX_SCHED_WAITQ_H */
