/*
 * Kernel tests for slice 4.4 — preemption, idle, kthread primitive.
 *
 * These tests run AFTER `boot_main`'s sched_start() has turned the
 * boot CPU's context into the idle task and sched_init has stashed
 * sched_rr's scheduler_ops.  ktest_main runs in the idle-task
 * context; when a test spawns a kthread and yields, the scheduler
 * picks it up, runs it, and the kthread's entry executes on its
 * own kernel stack.
 *
 * Each test cleans up any kthreads it spawned before returning so
 * subsequent tests don't inherit a stale runqueue.
 */

#include "ktest.h"

#include "core/sched/task.h"
#include "core/sched/sched.h"
#include "framework/registry.h"
#include "interfaces/scheduler.h"

/* Exported by components/sched_rr/sched_rr.c. */
extern const struct nx_scheduler_ops sched_rr_scheduler_ops;

/* --- 1. sched_init plumbing --- */

KTEST(sched_init_stashed_scheduler_ops_are_populated)
{
    KASSERT(sched_is_initialized());
    /* Bootstrap picks sched_rr from kernel.json — its scheduler_ops
     * table must be the one stashed. */
    KASSERT_EQ_U((uint64_t)(uintptr_t)sched_ops_for_test(),
                 (uint64_t)(uintptr_t)&sched_rr_scheduler_ops);
    /* `self` must be the bound component's state buffer, not NULL. */
    KASSERT_NOT_NULL(sched_self_for_test());
}

/* --- 2. sched_start transitioned boot context into idle --- */

KTEST(sched_start_promoted_boot_context_into_idle_task)
{
    struct nx_task *curr = nx_task_current();
    KASSERT_NOT_NULL(curr);
    /* Idle is id 0 (reserved); any other task would have a nonzero id
     * from the atomic sequence. */
    KASSERT_EQ_U(curr->id, 0);
    KASSERT(curr->name[0] == 'i' && curr->name[1] == 'd' &&
            curr->name[2] == 'l' && curr->name[3] == 'e');
    KASSERT_EQ_U(curr->state, NX_TASK_RUNNING);
    KASSERT_EQ_U(curr->preempt_count, 0);
}

/* --- 3. sched_tick drives the quantum --- */

KTEST(sched_tick_flips_need_resched_after_quantum)
{
    struct nx_task *curr = nx_task_current();
    KASSERT_NOT_NULL(curr);

    /* Pre-clear need_resched so we can observe the flip cleanly.  The
     * idle task may already have need_resched set from a prior tick;
     * the test needs the flag to transition 0 → 1. */
    curr->need_resched = 0;

    /* Bounded loop: the default quantum is SCHED_RR_DEFAULT_QUANTUM
     * _TICKS (10), so 32 calls is comfortably more than enough.  If
     * need_resched doesn't flip within the bound, the policy's tick
     * is broken. */
    int flipped = 0;
    for (int i = 0; i < 32; i++) {
        sched_tick();
        if (curr->need_resched) { flipped = 1; break; }
    }
    KASSERT(flipped);

    /* Reset so the post-test idle loop doesn't immediately trigger a
     * gratuitous reschedule check. */
    curr->need_resched = 0;
}

/* --- 4. sched_spawn_kthread actually runs the entry --- */

static volatile int g_spawn_sentinel;

static void sentinel_entry(void *arg)
{
    g_spawn_sentinel = (int)(uintptr_t)arg;
    /* Park — the test dequeues + destroys us before returning, so the
     * scheduler never picks us up from here anyway.  Yielding in a
     * loop keeps us well-behaved in the interim if it does. */
    for (;;) nx_task_yield();
}

KTEST(sched_spawn_kthread_runs_entry_after_yield)
{
    g_spawn_sentinel = 0;

    struct nx_task *k = sched_spawn_kthread("ktest_spawn",
                                             sentinel_entry,
                                             (void *)(uintptr_t)0xABCD);
    KASSERT_NOT_NULL(k);

    /* Yield from idle so the scheduler picks the new kthread.  After
     * a bounded number of yields the sentinel must be written. */
    for (int i = 0; i < 32 && g_spawn_sentinel != 0xABCD; i++)
        nx_task_yield();

    KASSERT_EQ_U(g_spawn_sentinel, 0xABCD);

    /* Cleanup: dequeue then destroy.  We hold the sched_self pointer
     * via the stashed getter; the scheduler never sees k again after
     * dequeue, so the destroy-free is safe. */
    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, k);
    nx_task_destroy(k);
}

/* --- 5. Two kthreads interleave via cooperative yields --- */

#define TWO_TASKS_ITERS 5

static volatile int g_ta_count;
static volatile int g_tb_count;

static void task_a_entry(void *arg)
{
    (void)arg;
    for (int i = 0; i < TWO_TASKS_ITERS; i++) {
        g_ta_count++;
        nx_task_yield();
    }
    for (;;) nx_task_yield();
}

static void task_b_entry(void *arg)
{
    (void)arg;
    for (int i = 0; i < TWO_TASKS_ITERS; i++) {
        g_tb_count++;
        nx_task_yield();
    }
    for (;;) nx_task_yield();
}

KTEST(sched_two_tasks_cooperate_via_yield)
{
    g_ta_count = 0;
    g_tb_count = 0;

    struct nx_task *a = sched_spawn_kthread("ta", task_a_entry, NULL);
    struct nx_task *b = sched_spawn_kthread("tb", task_b_entry, NULL);
    KASSERT_NOT_NULL(a);
    KASSERT_NOT_NULL(b);

    /* Yield from the idle (ktest) task until both counters reach the
     * expected value.  With cooperative round-robin, each yield picks
     * one of A/B/idle; after a few rotations each of A and B has had
     * TWO_TASKS_ITERS turns.  Bounded upper iteration count keeps
     * the test terminating even if scheduling is broken. */
    const int max_iters = 200;
    int done = 0;
    for (int i = 0; i < max_iters; i++) {
        if (g_ta_count >= TWO_TASKS_ITERS &&
            g_tb_count >= TWO_TASKS_ITERS) { done = 1; break; }
        nx_task_yield();
    }

    KASSERT(done);
    KASSERT_EQ_U(g_ta_count, TWO_TASKS_ITERS);
    KASSERT_EQ_U(g_tb_count, TWO_TASKS_ITERS);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, a);
    ops->dequeue(self, b);
    nx_task_destroy(a);
    nx_task_destroy(b);
}
