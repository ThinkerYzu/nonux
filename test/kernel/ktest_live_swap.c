/*
 * Slice 8.5 — live scheduler swap with running tasks.
 *
 * Headline: three background kthreads count iterations while the test
 * thread performs a live sched_priority → sched_rr swap via the config
 * API.  Tasks survive the swap with no leaks; observable behaviour
 * change (set_priority semantics differ) is verified before and after.
 *
 * Two tests (defined in REVERSE execution order — the ktest section
 * places entries in reverse source order):
 *   source-1: live_swap_snapshot_reflects_new_scheduler  → runs SECOND
 *   source-2: live_swap_tasks_survive_and_behavior_changes → runs FIRST
 *
 * Test 2 (runs first) performs the swap.  Test 1 (runs second) verifies
 * the post-swap state.
 *
 * Note: these tests are ordered last in KTEST_C.  Test 2 swaps
 * sched_priority → sched_rr (sched_priority is destroyed by the swap);
 * test 1 confirms the new state.  No subsequent ktest file runs after
 * ktest_live_swap.c.
 */

#include "ktest.h"

#include "framework/config.h"
#include "framework/component.h"
#include "framework/dispatcher.h"
#include "framework/registry.h"
#include "core/sched/sched.h"
#include "core/sched/task.h"
#include "core/lib/lib.h"
#include <stdatomic.h>

/* Scheduler ops tables and test helpers from both implementations. */
extern const struct nx_scheduler_ops sched_priority_scheduler_ops;
extern const struct nx_scheduler_ops sched_rr_scheduler_ops;
extern void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

/* ---- background counting tasks ------------------------------------- */

static volatile int g_lsw_a;
static volatile int g_lsw_b;
static volatile int g_lsw_c;

static void lsw_count_entry(void *arg)
{
    volatile int *cnt = (volatile int *)arg;
    for (;;) {
        (*cnt)++;
        nx_task_yield();
    }
}

/* ====================================================================
 * Test 1 (runs SECOND) — registry and driver stay coherent after swap
 *
 * test 2 (below) left the kernel running sched_rr; sched_priority was
 * destroyed.  Verify both the registry slot and the global scheduler
 * driver reflect the new state.
 *
 * NOTE: nx_config_query_snapshot is intentionally not used here.  The
 * snapshot is filled LIFO and is capped at 32 entries; after many EL0
 * tests the window is saturated by caller_slots, so the "scheduler"
 * boot slot may fall outside the window.  nx_slot_lookup is the
 * authoritative API (see ktest_config.c test 1 for the same rationale).
 * ==================================================================== */

KTEST(live_swap_snapshot_reflects_new_scheduler)
{
    /* Registry: "scheduler" slot must be bound to sched_rr. */
    struct nx_slot *s = nx_slot_lookup("scheduler");
    KASSERT_NOT_NULL(s);
    KASSERT_NOT_NULL(s->active);
    KASSERT_EQ_U((uint64_t)s->active->state, (uint64_t)NX_LC_ACTIVE);
    KASSERT(strcmp(s->active->manifest_id, "sched_rr") == 0);

    /* Global driver must route through sched_rr. */
    KASSERT(sched_ops_for_test() == &sched_rr_scheduler_ops);
}

/* ====================================================================
 * Test 2 (runs FIRST) — tasks survive the swap; behaviour changes
 * ==================================================================== */

KTEST(live_swap_tasks_survive_and_behavior_changes)
{
    /* Pre-condition: sched_priority must be the active scheduler. */
    KASSERT(sched_ops_for_test() == &sched_priority_scheduler_ops);

    /* Spawn three background counting tasks (enqueued into sched_priority). */
    g_lsw_a = 0;
    g_lsw_b = 0;
    g_lsw_c = 0;
    struct nx_task *ta = sched_spawn_kthread("lsw_a", lsw_count_entry,
                                             (void *)&g_lsw_a, NULL);
    struct nx_task *tb = sched_spawn_kthread("lsw_b", lsw_count_entry,
                                             (void *)&g_lsw_b, NULL);
    struct nx_task *tc = sched_spawn_kthread("lsw_c", lsw_count_entry,
                                             (void *)&g_lsw_c, NULL);
    KASSERT_NOT_NULL(ta);
    KASSERT_NOT_NULL(tb);
    KASSERT_NOT_NULL(tc);

    /* Let them run for several quanta under sched_priority. */
    for (int i = 0; i < 15; i++) nx_task_yield();

    int before_a = g_lsw_a;
    int before_b = g_lsw_b;
    int before_c = g_lsw_c;
    KASSERT(before_a > 0);
    KASSERT(before_b > 0);
    KASSERT(before_c > 0);

    /* --- Pre-swap behaviour: set_priority returns NX_OK (task is queued) */
    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    int rc = ops->set_priority(self, ta, 3);
    KASSERT_EQ_U((uint64_t)rc, (uint64_t)NX_OK);

    /*
     * Dequeue all three tasks (plus the dispatcher kthread) before the
     * swap.  The pause phase completes cleanly when the old runqueue is
     * emptied of entries the new scheduler won't inherit.  Task structs
     * remain valid; re-enqueue into sched_rr after the swap below.
     */
    struct nx_task *disp = nx_dispatcher_task_for_test();
    ops->dequeue(self, ta);
    ops->dequeue(self, tb);
    ops->dequeue(self, tc);
    if (disp) ops->dequeue(self, disp);

    /*
     * Pre-swap safety: purge any stale EL0 user tasks from the scheduler
     * runqueue (blocking tasks that woke up mid-test from a previous test's
     * wait-queue, e.g. an expired ppoll deadline) and wait for their
     * in-flight IPC messages to the scheduler slot to drain.
     *
     * The pre-drain happens with the timer STILL active so the idle task's
     * wfi can be interrupted — preventing the deadlock that would occur
     * inside nx_recompose when timer_pause() + drain + idle-wfi interact.
     */
    sched_rr_purge_user_tasks(sched_self_for_test(), nx_task_current());
    {
        struct nx_slot *ss = nx_slot_lookup("scheduler");
        if (ss) {
            while (atomic_load_explicit(&ss->in_flight_calls,
                                        memory_order_acquire) > 0)
                nx_task_yield();
        }
    }

    /* --- Live swap: sched_priority → sched_rr ------------------------ */
    rc = nx_config_swap_component("scheduler", "sched_rr");
    KASSERT_EQ_U((uint64_t)rc, (uint64_t)NX_OK);

    /*
     * sched_rr's enable() hook called sched_init(); verify the global
     * scheduler driver now routes through the new implementation.
     */
    KASSERT(sched_ops_for_test() == &sched_rr_scheduler_ops);
    KASSERT_NOT_NULL(sched_self_for_test());

    /* --- Post-swap behaviour: set_priority returns EINVAL on sched_rr */
    ops = sched_ops_for_test();
    self = sched_self_for_test();
    rc = ops->set_priority(self, ta, 3);
    KASSERT_EQ_U((uint64_t)rc, (uint64_t)NX_EINVAL); /* behaviour changed */

    /* Re-enqueue all three tasks and the dispatcher into the new scheduler.
     * Without this, the dispatcher kthread is stranded (it was in sched_priority's
     * runqueue, which is now destroyed) and subsequent IPC processing stalls. */
    if (disp) ops->enqueue(self, disp);
    ops->enqueue(self, ta);
    ops->enqueue(self, tb);
    ops->enqueue(self, tc);

    /* Let them run for several quanta under sched_rr. */
    for (int i = 0; i < 15; i++) nx_task_yield();

    /* Tasks survived: each counter advanced past its pre-swap snapshot. */
    KASSERT(g_lsw_a > before_a);
    KASSERT(g_lsw_b > before_b);
    KASSERT(g_lsw_c > before_c);

    /* Cleanup. */
    ops->dequeue(self, ta);
    ops->dequeue(self, tb);
    ops->dequeue(self, tc);
    nx_task_destroy(ta);
    nx_task_destroy(tb);
    nx_task_destroy(tc);
}
