/*
 * Core scheduler driver — preempt counters.
 *
 * Slice 4.1 only: increment / decrement a per-task counter.  When the
 * counter reaches zero and `need_resched` is set, slice 4.4 will kick the
 * reschedule shim; today the field is just observed.
 */

#include "core/sched/sched.h"
#include "core/sched/task.h"

void nx_preempt_disable(void)
{
    struct nx_task *t = nx_task_current();
    if (!t) return;
    t->preempt_count++;
    /* Single-core, non-reentrant: no barrier needed between the counter
     * bump and subsequent memory access. */
}

void nx_preempt_enable(void)
{
    struct nx_task *t = nx_task_current();
    if (!t) return;
    if (t->preempt_count > 0)
        t->preempt_count--;
    /* Slice 4.4: if preempt_count == 0 && need_resched: call the
     * reschedule shim here too.  Stub for now. */
}

int nx_preempt_count(void)
{
    struct nx_task *t = nx_task_current();
    return t ? t->preempt_count : 0;
}
