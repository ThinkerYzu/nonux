/*
 * Kernel tests for slice 7.8a — wait-queue primitive.
 *
 * Five universal cases that cover the contract of `nx_waitq`:
 *
 *   1. waitq_wake_one_releases_one      — wake_one wakes exactly one waiter.
 *   2. waitq_wake_all_releases_all      — wake_all wakes every waiter.
 *   3. waitq_wait_returns_on_deadline   — finite budget expires without a wake.
 *   4. waitq_wake_after_wait_no_lost_wakeup
 *                                       — wake delivered after wait begins
 *                                         is observed by the waiter.
 *   5. waitq_multi_waiter_fifo_order    — wake_one releases waiters in
 *                                         FIFO order across multiple calls.
 *
 * These tests run in the idle-task context (ktest_main is reached after
 * sched_start has promoted the boot CPU into the idle task).  Each test
 * spawns one or more kthreads that call nx_waitq_wait_with_deadline,
 * yields from idle to let those kthreads run far enough to block on the
 * waitq, then issues wakes and yields again to let the woken kthreads
 * record their observed return value before being reaped.
 */

#include "ktest.h"

#include "core/sched/sched.h"
#include "core/sched/task.h"
#include "core/sched/waitq.h"
#include "framework/registry.h"
#include "interfaces/scheduler.h"

/* --- shared waiter fixture ------------------------------------------ */

#define WAITQ_KTEST_MAX_WAITERS 4

struct waiter_ctx {
    struct nx_waitq *wq;
    uint64_t         budget_ns;
    int              order;        /* set when this waiter completes */
    int              rc;           /* return value of wait */
    int              done;         /* 0 → still running, 1 → completed */
};

static volatile int g_completion_seq;   /* monotonic order counter */

static void waiter_entry(void *arg)
{
    struct waiter_ctx *ctx = arg;
    ctx->rc    = nx_waitq_wait_with_deadline(ctx->wq, ctx->budget_ns);
    ctx->order = ++g_completion_seq;
    ctx->done  = 1;
    /* Park — the test reaps us before returning. */
    for (;;) nx_task_yield();
}

/* Yield until either `cond` becomes true or `budget` yields elapse,
 * whichever comes first.  Returns 1 if the condition was met.  Used
 * to give a kthread time to make progress without hanging the test
 * if scheduling is broken. */
#define WAIT_FOR(cond, budget)                                   \
    do {                                                          \
        int _b = (budget);                                        \
        while (_b-- > 0 && !(cond)) nx_task_yield();              \
    } while (0)

/* Reap a parked waiter kthread.  The waiter is sitting in the
 * `for(;;) nx_task_yield()` loop after waking; dequeue + destroy. */
static void reap_waiter(struct nx_task *t)
{
    if (!t) return;
    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    if (ops && self) ops->dequeue(self, t);
    nx_task_destroy(t);
}

/* --- 1. wake_one releases exactly one waiter ------------------------ */

KTEST(waitq_wake_one_releases_one)
{
    struct nx_waitq wq;
    nx_waitq_init(&wq);
    g_completion_seq = 0;

    struct waiter_ctx c1 = { .wq = &wq, .budget_ns = 0 };
    struct waiter_ctx c2 = { .wq = &wq, .budget_ns = 0 };
    struct nx_task *t1 = sched_spawn_kthread("wq1a", waiter_entry, &c1, NULL);
    struct nx_task *t2 = sched_spawn_kthread("wq1b", waiter_entry, &c2, NULL);
    KASSERT_NOT_NULL(t1);
    KASSERT_NOT_NULL(t2);

    /* Let both waiters reach the wait. */
    WAIT_FOR(t1->state == NX_TASK_BLOCKED && t2->state == NX_TASK_BLOCKED, 64);
    KASSERT_EQ_U(t1->state, NX_TASK_BLOCKED);
    KASSERT_EQ_U(t2->state, NX_TASK_BLOCKED);

    nx_waitq_wake_one(&wq);

    /* Exactly one waiter should complete; the other stays blocked. */
    WAIT_FOR(c1.done || c2.done, 64);
    KASSERT_EQ_U(c1.done + c2.done, 1);

    /* Drain the second waiter so we can reap cleanly. */
    nx_waitq_wake_one(&wq);
    WAIT_FOR(c1.done && c2.done, 64);
    KASSERT_EQ_U(c1.done, 1);
    KASSERT_EQ_U(c2.done, 1);
    KASSERT_EQ_U((unsigned)c1.rc, (unsigned)NX_OK);
    KASSERT_EQ_U((unsigned)c2.rc, (unsigned)NX_OK);

    reap_waiter(t1);
    reap_waiter(t2);
}

/* --- 2. wake_all releases every waiter ------------------------------ */

KTEST(waitq_wake_all_releases_all)
{
    struct nx_waitq wq;
    nx_waitq_init(&wq);
    g_completion_seq = 0;

    struct waiter_ctx c[3] = {
        { .wq = &wq, .budget_ns = 0 },
        { .wq = &wq, .budget_ns = 0 },
        { .wq = &wq, .budget_ns = 0 },
    };
    struct nx_task *t[3];
    for (int i = 0; i < 3; i++) {
        t[i] = sched_spawn_kthread("wq2", waiter_entry, &c[i], NULL);
        KASSERT_NOT_NULL(t[i]);
    }
    WAIT_FOR(t[0]->state == NX_TASK_BLOCKED &&
             t[1]->state == NX_TASK_BLOCKED &&
             t[2]->state == NX_TASK_BLOCKED, 96);

    nx_waitq_wake_all(&wq);

    WAIT_FOR(c[0].done && c[1].done && c[2].done, 96);
    for (int i = 0; i < 3; i++) {
        KASSERT_EQ_U(c[i].done, 1);
        KASSERT_EQ_U((unsigned)c[i].rc, (unsigned)NX_OK);
    }

    for (int i = 0; i < 3; i++) reap_waiter(t[i]);
}

/* --- 3. wait returns NX_EDEADLINE when budget elapses without a wake */

KTEST(waitq_wait_returns_on_deadline)
{
    struct nx_waitq wq;
    nx_waitq_init(&wq);
    g_completion_seq = 0;

    /* 50 ms budget — comfortably more than one 100 ms timer tick at
     * 10 Hz could ever round to, but small enough to keep the
     * 15-second QEMU test budget safe.  We spin yielding for up to
     * 256 yields after the budget; with idle in the runqueue and a
     * timer tick every 100 ms, the deadline check fires within a
     * tick or two. */
    struct waiter_ctx c = { .wq = &wq, .budget_ns = 50ULL * 1000 * 1000 };
    struct nx_task *t = sched_spawn_kthread("wq3", waiter_entry, &c, NULL);
    KASSERT_NOT_NULL(t);

    WAIT_FOR(c.done, 1024);
    KASSERT_EQ_U(c.done, 1);
    KASSERT_EQ_U((unsigned)c.rc, (unsigned)NX_EDEADLINE);

    reap_waiter(t);
}

/* --- 4. wake delivered after wait begins is observed --------------- */

KTEST(waitq_wake_after_wait_no_lost_wakeup)
{
    struct nx_waitq wq;
    nx_waitq_init(&wq);
    g_completion_seq = 0;

    /* Indefinite wait; if the wakeup were lost the test would hang. */
    struct waiter_ctx c = { .wq = &wq, .budget_ns = 0 };
    struct nx_task *t = sched_spawn_kthread("wq4", waiter_entry, &c, NULL);
    KASSERT_NOT_NULL(t);

    /* Let the waiter reach the wait — only then issue wake_one. */
    WAIT_FOR(t->state == NX_TASK_BLOCKED, 64);
    KASSERT_EQ_U(t->state, NX_TASK_BLOCKED);

    nx_waitq_wake_one(&wq);

    WAIT_FOR(c.done, 64);
    KASSERT_EQ_U(c.done, 1);
    KASSERT_EQ_U((unsigned)c.rc, (unsigned)NX_OK);

    reap_waiter(t);
}

/* --- 5. wake_one releases waiters in FIFO order -------------------- */

KTEST(waitq_multi_waiter_fifo_order)
{
    struct nx_waitq wq;
    nx_waitq_init(&wq);
    g_completion_seq = 0;

    /* Spawn 3 waiters in order A, B, C.  All block before any wake.
     * Then issue wake_one three times — A must complete first, then
     * B, then C. */
    struct waiter_ctx ca = { .wq = &wq, .budget_ns = 0 };
    struct waiter_ctx cb = { .wq = &wq, .budget_ns = 0 };
    struct waiter_ctx cc = { .wq = &wq, .budget_ns = 0 };
    struct nx_task *ta = sched_spawn_kthread("wqA", waiter_entry, &ca, NULL);
    /* Yield so A actually parks before B/C arrive — the FIFO test is
     * only meaningful if waiters enter the queue in spawn order. */
    WAIT_FOR(ta->state == NX_TASK_BLOCKED, 64);
    struct nx_task *tb = sched_spawn_kthread("wqB", waiter_entry, &cb, NULL);
    WAIT_FOR(tb->state == NX_TASK_BLOCKED, 64);
    struct nx_task *tc = sched_spawn_kthread("wqC", waiter_entry, &cc, NULL);
    WAIT_FOR(tc->state == NX_TASK_BLOCKED, 64);
    KASSERT_NOT_NULL(ta);
    KASSERT_NOT_NULL(tb);
    KASSERT_NOT_NULL(tc);

    nx_waitq_wake_one(&wq);
    WAIT_FOR(ca.done, 64);
    KASSERT_EQ_U(ca.done, 1);
    KASSERT_EQ_U(cb.done, 0);
    KASSERT_EQ_U(cc.done, 0);

    nx_waitq_wake_one(&wq);
    WAIT_FOR(cb.done, 64);
    KASSERT_EQ_U(cb.done, 1);
    KASSERT_EQ_U(cc.done, 0);

    nx_waitq_wake_one(&wq);
    WAIT_FOR(cc.done, 64);
    KASSERT_EQ_U(cc.done, 1);

    /* Order assigned by g_completion_seq — A=1, B=2, C=3. */
    KASSERT_EQ_U(ca.order, 1);
    KASSERT_EQ_U(cb.order, 2);
    KASSERT_EQ_U(cc.order, 3);

    reap_waiter(ta);
    reap_waiter(tb);
    reap_waiter(tc);
}
