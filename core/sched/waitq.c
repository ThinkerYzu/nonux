/*
 * Wait-queue primitive — slice 7.8a.
 *
 * See core/sched/waitq.h for the contract.  Implementation notes:
 *
 *   - Mutual exclusion against ISR wakers + concurrent kthread
 *     waiters is achieved with a tiny IRQ-save critical section
 *     around every list mutation.  Single-CPU only; SMP will need
 *     per-waitq locks.
 *
 *   - `wait_with_deadline` dequeues `current` from the scheduler
 *     runqueue, links it onto `wq->waiters` via the same
 *     `sched_node`, flips state to NX_TASK_BLOCKED, then calls
 *     `nx_task_yield()`.  When `current` resumes (after wake or
 *     deadline expiry), it returns NX_OK or NX_EDEADLINE based on
 *     `task->wait_woken`.
 *
 *   - The deadline mechanism: a task that asked for a finite
 *     budget is also linked into a singleton g_deadline_list via
 *     `task->deadline_node`.  `sched_tick` calls
 *     `nx_waitq_tick_deadlines`, which walks that list once per
 *     tick and re-enqueues any task whose deadline has elapsed.
 *     Tasks waiting indefinitely (budget_ns == 0) are NOT on the
 *     list, so the per-tick cost scales with the number of
 *     deadline-bearing waiters, not with the total blocked count.
 */

#include "core/sched/waitq.h"

#include "core/sched/task.h"
#include "core/sched/sched.h"
#include "framework/registry.h"
#include "interfaces/scheduler.h"

/* --- IRQ save/restore: save daif into a uint64_t, restore from it.
 *     Kernel-only — host build never preempts in tests, and the
 *     deferred-tool-style save/restore on x86 would be different. */

#if !__STDC_HOSTED__
static inline uint64_t waitq_irq_save(void)
{
    uint64_t v;
    asm volatile("mrs %0, daif" : "=r"(v));
    asm volatile("msr daifset, #2" ::: "memory");
    return v;
}
static inline void waitq_irq_restore(uint64_t v)
{
    asm volatile("msr daif, %0" :: "r"(v) : "memory");
}
#else
static inline uint64_t waitq_irq_save(void)        { return 0; }
static inline void     waitq_irq_restore(uint64_t v) { (void)v; }
#endif

/* --- stashed scheduler ops (read once via accessors) ---------------- */

static const struct nx_scheduler_ops *waitq_sched_ops(void)
{
    return sched_ops_for_test();
}
static void *waitq_sched_self(void)
{
    return sched_self_for_test();
}

/* --- global deadline list ------------------------------------------- */

static struct nx_list_head g_deadline_list = {
    { &g_deadline_list.n, &g_deadline_list.n }
};

/* --- public API ----------------------------------------------------- */

void nx_waitq_init(struct nx_waitq *wq)
{
    if (!wq) return;
    nx_list_init(&wq->waiters);
}

int nx_waitq_wait_with_deadline(struct nx_waitq *wq, uint64_t budget_ns)
{
    if (!wq) return NX_EINVAL;
    struct nx_task *t = nx_task_current();
    if (!t) return NX_EINVAL;

    uint64_t flags = waitq_irq_save();
    nx_preempt_disable();

    /* Pull current off the runqueue.  If the policy doesn't have
     * us linked (e.g. a hand-built test fixture), dequeue is a
     * no-op (NX_ENOENT) — that's fine. */
    const struct nx_scheduler_ops *ops = waitq_sched_ops();
    void *self = waitq_sched_self();
    if (ops && self) {
        ops->dequeue(self, t);
    }

    /* Link onto the waitq via the same sched_node.  FIFO: append
     * at tail so wake_one releases oldest waiter first. */
    nx_list_add_tail(&wq->waiters, &t->sched_node);
    t->wait_q     = wq;
    t->wait_woken = 0;

    if (budget_ns > 0) {
        nx_deadline_start(&t->wait_deadline, budget_ns);
        t->wait_has_deadline = 1;
        nx_list_add_tail(&g_deadline_list, &t->deadline_node);
    } else {
        t->wait_has_deadline = 0;
    }

    t->state = NX_TASK_BLOCKED;

    nx_preempt_enable();
    waitq_irq_restore(flags);

    /*
     * Voluntarily switch out.  We're not on the runqueue any more,
     * so sched_check_resched picks somebody else.  Control returns
     * here once a wake (or deadline expiry) has put us back on the
     * runqueue and the scheduler picks us up.
     */
    nx_task_yield();

    return t->wait_woken ? NX_OK : NX_EDEADLINE;
}

/* Caller must hold preempt-disabled + IRQ-disabled. */
static void waitq_release_locked(struct nx_task *t)
{
    /* Unlink from waitq (sched_node currently points into wq->waiters). */
    nx_list_remove(&t->sched_node);

    /* Unlink from global deadline list if present. */
    if (t->wait_has_deadline) {
        nx_list_remove(&t->deadline_node);
        t->wait_has_deadline = 0;
    }

    t->wait_q     = NULL;
    t->wait_woken = 1;
    t->state      = NX_TASK_READY;

    const struct nx_scheduler_ops *ops = waitq_sched_ops();
    void *self = waitq_sched_self();
    if (ops && self) {
        ops->enqueue(self, t);
    }
}

void nx_waitq_wake_one(struct nx_waitq *wq)
{
    if (!wq) return;
    uint64_t flags = waitq_irq_save();
    nx_preempt_disable();

    if (!nx_list_empty(&wq->waiters)) {
        struct nx_list_node *n = wq->waiters.n.next;
        struct nx_task *t = nx_list_entry(n, struct nx_task, sched_node);
        waitq_release_locked(t);
    }

    nx_preempt_enable();
    waitq_irq_restore(flags);
}

void nx_waitq_wake_all(struct nx_waitq *wq)
{
    if (!wq) return;
    uint64_t flags = waitq_irq_save();
    nx_preempt_disable();

    while (!nx_list_empty(&wq->waiters)) {
        struct nx_list_node *n = wq->waiters.n.next;
        struct nx_task *t = nx_list_entry(n, struct nx_task, sched_node);
        waitq_release_locked(t);
    }

    nx_preempt_enable();
    waitq_irq_restore(flags);
}

void nx_waitq_tick_deadlines(void)
{
    /* Called from sched_tick (timer ISR).  IRQs are already masked
     * on the ISR-entry path, but save/restore is cheap and keeps
     * the function safe if some future caller invokes it from
     * thread context. */
    uint64_t flags = waitq_irq_save();
    nx_preempt_disable();

    struct nx_list_node *n, *tmp;
    nx_list_for_each_safe(n, tmp, &g_deadline_list) {
        struct nx_task *t = nx_list_entry(n, struct nx_task, deadline_node);
        if (!nx_deadline_exceeded(&t->wait_deadline)) continue;

        /* Expire: unlink from waitq + deadline list, leave
         * wait_woken==0 so wait_with_deadline returns NX_EDEADLINE. */
        nx_list_remove(&t->sched_node);
        nx_list_remove(&t->deadline_node);
        t->wait_has_deadline = 0;
        t->wait_q = NULL;
        t->state  = NX_TASK_READY;

        const struct nx_scheduler_ops *ops = waitq_sched_ops();
        void *self = waitq_sched_self();
        if (ops && self) {
            ops->enqueue(self, t);
        }
    }

    nx_preempt_enable();
    waitq_irq_restore(flags);
}
