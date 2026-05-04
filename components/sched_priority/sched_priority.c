/*
 * sched_priority — fixed-priority scheduler policy (slice 8.4).
 *
 * Second real policy component for nonux.  Implements the universal
 * interface contract from `interfaces/scheduler.h`; passes the
 * conformance suite from slice 4.2 and the priority-specific cases
 * in test/host/component_sched_priority_test.c.
 *
 * SCHED_PRIORITY_LEVELS priority buckets (0 = default/lowest,
 * SCHED_PRIORITY_LEVELS-1 = highest).  Each bucket is a FIFO sub-queue.
 * `pick_next` scans from the highest bucket down, returning the first
 * non-BLOCKED task.  `yield` rotates the current task within its own
 * bucket so same-priority tasks share the CPU fairly.
 *
 * Like sched_rr, task storage is borrowed: the scheduler only
 * links/unlinks via the intrusive sched_node — borrow semantics per
 * scheduler.h.
 */

#include "framework/component.h"
#include "framework/process.h"
#include "framework/registry.h"
#include "framework/scheduler_dispatch.h"
#include "interfaces/scheduler.h"
#include "core/sched/task.h"
#include "core/lib/list.h"

#if !__STDC_HOSTED__
#include "core/lib/lib.h"
#endif

#define SCHED_PRIORITY_LEVELS         8   /* 0 = default, 7 = highest */
#define SCHED_PRIORITY_DEFAULT        0
#define SCHED_PRIORITY_DEFAULT_QUANTUM_TICKS 2

struct sched_priority_state {
    struct nx_list_head queues[SCHED_PRIORITY_LEVELS];
    unsigned            quantum_ticks;
    unsigned            remaining;

    unsigned            init_called;
    unsigned            enable_called;
    unsigned            disable_called;
    unsigned            destroy_called;
};

/* Return the priority level of a queued task, or -1 if not found. */
static int task_priority(const struct sched_priority_state *s,
                         const struct nx_task *t)
{
    for (int p = 0; p < SCHED_PRIORITY_LEVELS; p++) {
        const struct nx_list_node *n;
        nx_list_for_each(n, &s->queues[p]) {
            if (n == &t->sched_node) return p;
        }
    }
    return -1;
}

static int on_queue(const struct sched_priority_state *s,
                    const struct nx_task *t)
{
    return task_priority(s, t) >= 0;
}

static struct nx_task *sched_priority_pick_next(void *self)
{
    struct sched_priority_state *s = self;
    /* Scan from highest to lowest; within each bucket return the head.
     * Skip BLOCKED tasks (defensive — same rationale as sched_rr). */
    for (int p = SCHED_PRIORITY_LEVELS - 1; p >= 0; p--) {
        struct nx_list_node *n;
        nx_list_for_each(n, &s->queues[p]) {
            struct nx_task *t = nx_list_entry(n, struct nx_task, sched_node);
            if (t->state != NX_TASK_BLOCKED) return t;
        }
    }
    return NULL;
}

static int sched_priority_enqueue(void *self, struct nx_task *task)
{
    if (!task) return NX_EINVAL;
    struct sched_priority_state *s = self;
    if (on_queue(s, task)) return NX_EEXIST;
    nx_list_add_tail(&s->queues[SCHED_PRIORITY_DEFAULT], &task->sched_node);
    return NX_OK;
}

static int sched_priority_dequeue(void *self, struct nx_task *task)
{
    if (!task) return NX_EINVAL;
    struct sched_priority_state *s = self;
    if (!on_queue(s, task)) return NX_ENOENT;
    nx_list_remove(&task->sched_node);
    return NX_OK;
}

/*
 * Rotate the current task within its priority bucket so that other
 * tasks at the same level get a turn.  On the host build (no current
 * task), fall back to rotating the head of the highest non-empty
 * bucket — the conformance suite doesn't test yield so this path is
 * exercised only by component-specific tests.
 */
static void sched_priority_yield(void *self)
{
    struct sched_priority_state *s = self;
    struct nx_task *curr = nx_task_current();
    if (curr) {
        int p = task_priority(s, curr);
        if (p >= 0) {
            struct nx_list_node *node = &curr->sched_node;
            nx_list_remove(node);
            nx_list_add_tail(&s->queues[p], node);
            s->remaining = s->quantum_ticks;
            return;
        }
    }
    /* Fallback: rotate head of highest non-empty bucket. */
    for (int p = SCHED_PRIORITY_LEVELS - 1; p >= 0; p--) {
        if (!nx_list_empty(&s->queues[p])) {
            struct nx_list_node *head = s->queues[p].n.next;
            nx_list_remove(head);
            nx_list_add_tail(&s->queues[p], head);
            s->remaining = s->quantum_ticks;
            return;
        }
    }
}

static int sched_priority_set_priority(void *self, struct nx_task *task,
                                        int priority)
{
    if (!task) return NX_EINVAL;
    if (priority < 0 || priority >= SCHED_PRIORITY_LEVELS) return NX_EINVAL;
    struct sched_priority_state *s = self;
    int cur_p = task_priority(s, task);
    if (cur_p < 0) return NX_ENOENT;
    if (cur_p == priority) return NX_OK;
    nx_list_remove(&task->sched_node);
    nx_list_add_tail(&s->queues[priority], &task->sched_node);
    return NX_OK;
}

static void sched_priority_tick(void *self)
{
    struct sched_priority_state *s = self;
    struct nx_task *curr = nx_task_current();
    if (!curr) return;
    if (s->remaining > 0) s->remaining--;
    if (s->remaining == 0) {
        curr->need_resched = 1;
        s->remaining = s->quantum_ticks;
    }
}

const struct nx_scheduler_ops sched_priority_scheduler_ops = {
    .pick_next    = sched_priority_pick_next,
    .enqueue      = sched_priority_enqueue,
    .dequeue      = sched_priority_dequeue,
    .yield        = sched_priority_yield,
    .set_priority = sched_priority_set_priority,
    .tick         = sched_priority_tick,
};

/* ------------------------------------------------------------------ */

static int sched_priority_init(void *self)
{
    struct sched_priority_state *s = self;
    for (int p = 0; p < SCHED_PRIORITY_LEVELS; p++)
        nx_list_init(&s->queues[p]);
    s->quantum_ticks = SCHED_PRIORITY_DEFAULT_QUANTUM_TICKS;
    s->remaining     = s->quantum_ticks;
    s->init_called++;
    return NX_OK;
}

static int sched_priority_enable(void *self)
{
    struct sched_priority_state *s = self;
    s->enable_called++;
    return NX_OK;
}

static int sched_priority_disable(void *self)
{
    struct sched_priority_state *s = self;
    s->disable_called++;
    return NX_OK;
}

static void sched_priority_destroy(void *self)
{
    struct sched_priority_state *s = self;
    s->destroy_called++;
}

static int sched_priority_handle_msg(void *self, struct nx_ipc_message *msg)
{
    return nx_scheduler_dispatch(self, &sched_priority_scheduler_ops, msg);
}

const struct nx_component_ops sched_priority_component_ops = {
    .init       = sched_priority_init,
    .enable     = sched_priority_enable,
    .disable    = sched_priority_disable,
    .destroy    = sched_priority_destroy,
    .handle_msg = sched_priority_handle_msg,
};

NX_COMPONENT_REGISTER_NO_DEPS_IFACE(sched_priority,
                                    struct sched_priority_state,
                                    &sched_priority_component_ops,
                                    &sched_priority_scheduler_ops);

/* ------------------------------------------------------------------ *
 * Test-only helpers
 * ------------------------------------------------------------------ */

/*
 * Dequeue every runqueue entry whose task belongs to a user process
 * except `keep`.  Mirrors sched_rr_purge_user_tasks; called by the
 * ktest posix-layer teardown between tests to drain stranded EL0
 * children.
 */
void sched_priority_purge_user_tasks(void *self, struct nx_task *keep)
{
    struct sched_priority_state *s = self;
    for (int p = 0; p < SCHED_PRIORITY_LEVELS; p++) {
        struct nx_list_node *node = s->queues[p].n.next;
        while (node != &s->queues[p].n) {
            struct nx_task *t = nx_list_entry(node, struct nx_task, sched_node);
            struct nx_list_node *next = node->next;
            if (t != keep && t->process && t->process != &g_kernel_process)
                nx_list_remove(node);
            node = next;
        }
    }
}

/*
 * Build-compatibility shim: the 20+ ktest teardown helpers declare
 * `sched_rr_purge_user_tasks` as an extern and call it to clean up
 * stranded EL0 tasks after each POSIX-layer test.  When
 * sched_priority is the active scheduler (i.e. in kernel.json),
 * sched_rr.c is NOT compiled in — so we provide the symbol here and
 * delegate to our own purge.
 *
 * Marked __weak so that when both sched_rr.c and sched_priority.c
 * are linked together (host test build), sched_rr.c's strong
 * definition wins and routes to the correct sched_rr state.  In the
 * kernel build only one scheduler impl is compiled in, so there is
 * no collision regardless.
 */
void __attribute__((weak)) sched_rr_purge_user_tasks(void *self,
                                                      struct nx_task *keep)
{
    sched_priority_purge_user_tasks(self, keep);
}
