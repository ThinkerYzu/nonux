/*
 * sched_rr — round-robin scheduler policy (slice 4.3).
 *
 * First real policy component for nonux.  Implements the universal
 * interface contract from `interfaces/scheduler.h`; passes the
 * conformance suite from slice 4.2.  No dependencies, no worker
 * threads — `spawns_threads: false`, `pause_hook: false`.
 *
 * Slice 4.3 is cooperative-only.  `tick()` accounts for quantum expiry
 * (sets `need_resched` on the current task when the counter hits
 * zero), but the IRQ-return reschedule shim that acts on
 * `need_resched` lives in slice 4.4.  `sched_init` is also a 4.4 job —
 * it will stash this component's `scheduler_ops`/`self` pair in the
 * core driver's `g_sched` escape-hatch.
 *
 * Runqueue is an intrusive `struct nx_list_head` over every task's
 * `sched_node`.  Enqueue → tail, pick_next → head (no dequeue on
 * pick so the operation is idempotent against the same queue state).
 * `yield` rotates: head to tail.  Classic round-robin.
 *
 * Task storage is owned by the caller (`nx_task_create` allocates,
 * `nx_task_destroy` frees).  The scheduler only links/unlinks via
 * the intrusive node — borrow semantics per scheduler.h.
 */

#include "framework/component.h"
#include "framework/registry.h"
#include "interfaces/scheduler.h"
#include "core/sched/task.h"
#include "core/lib/list.h"

#if !__STDC_HOSTED__
#include "core/lib/lib.h"
#endif

/*
 * Default time quantum in ticks.  At the 10 Hz kernel timer this is
 * 1 second per task — conservative for interactive workloads, lets
 * the slice-4.4 preemption demo visibly advance both tasks without
 * the test runner timing out.  A future kernel.json config knob can
 * override this once gen-config emits per-component config macros.
 */
#define SCHED_RR_DEFAULT_QUANTUM_TICKS 10

struct sched_rr_state {
    struct nx_list_head runqueue;
    unsigned            quantum_ticks;
    unsigned            remaining;

    /* Counters let kernel / host tests peek at the bound instance
     * and verify that each lifecycle verb fired exactly once. */
    unsigned            init_called;
    unsigned            enable_called;
    unsigned            disable_called;
    unsigned            destroy_called;
};

/* --------------------------------------------------------------------
 *  struct nx_scheduler_ops — the interface surface consumed by the
 *  core scheduler driver (slice 4.4 reads via `g_sched`).  Public
 *  symbol so tests can reference it without reaching into the
 *  descriptor.
 * ------------------------------------------------------------------ */

static int on_queue(const struct sched_rr_state *s, const struct nx_task *t)
{
    const struct nx_list_node *n;
    nx_list_for_each(n, &s->runqueue) {
        if (n == &t->sched_node) return 1;
    }
    return 0;
}

static struct nx_task *sched_rr_pick_next(void *self)
{
    struct sched_rr_state *s = self;
    if (nx_list_empty(&s->runqueue)) return NULL;
    return nx_list_entry(s->runqueue.n.next, struct nx_task, sched_node);
}

static int sched_rr_enqueue(void *self, struct nx_task *task)
{
    if (!task) return NX_EINVAL;
    struct sched_rr_state *s = self;
    if (on_queue(s, task)) return NX_EEXIST;
    nx_list_add_tail(&s->runqueue, &task->sched_node);
    return NX_OK;
}

static int sched_rr_dequeue(void *self, struct nx_task *task)
{
    if (!task) return NX_EINVAL;
    struct sched_rr_state *s = self;
    if (!on_queue(s, task)) return NX_ENOENT;
    nx_list_remove(&task->sched_node);
    return NX_OK;
}

static void sched_rr_yield(void *self)
{
    struct sched_rr_state *s = self;
    if (nx_list_empty(&s->runqueue)) return;
    /* Rotate: move the head to the tail.  Cooperative-only in
     * slice 4.3; the reschedule shim (4.4) calls pick_next after. */
    struct nx_list_node *head = s->runqueue.n.next;
    nx_list_remove(head);
    nx_list_add_tail(&s->runqueue, head);
    /* Reset quantum so the freshly-rotated head starts its slice
     * with a full window. */
    s->remaining = s->quantum_ticks;
}

static int sched_rr_set_priority(void *self, struct nx_task *task, int priority)
{
    (void)self; (void)priority;
    if (!task) return NX_EINVAL;
    /* Round-robin ignores priorities — the contract says policies
     * that don't support them must return NX_EINVAL uniformly. */
    return NX_EINVAL;
}

static void sched_rr_tick(void *self)
{
    struct sched_rr_state *s = self;
    /* Read the current task from the core driver's TPIDR_EL1 stash.
     * sched_rr doesn't track current separately — the core keeps the
     * one source of truth, and the policy only cares about quantum
     * accounting for whoever is actually running right now. */
    struct nx_task *curr = nx_task_current();
    if (!curr) return;
    if (s->remaining > 0) s->remaining--;
    if (s->remaining == 0) {
        /* Quantum expired.  Flag the current task for reschedule —
         * the IRQ-return shim in core/sched/sched.c consumes
         * need_resched and invokes pick_next on this task's kernel
         * stack before ERET. */
        curr->need_resched = 1;
        s->remaining = s->quantum_ticks;
    }
}

const struct nx_scheduler_ops sched_rr_scheduler_ops = {
    .pick_next    = sched_rr_pick_next,
    .enqueue      = sched_rr_enqueue,
    .dequeue      = sched_rr_dequeue,
    .yield        = sched_rr_yield,
    .set_priority = sched_rr_set_priority,
    .tick         = sched_rr_tick,
};

/* --------------------------------------------------------------------
 *  struct nx_component_ops — framework-facing lifecycle table.
 *  `handle_msg` stays NULL: the scheduler is driven by the core
 *  driver through `scheduler_ops`, not by IPC messages.  Slice 3.9b
 *  may add a thin handle_msg wrapper for runtime introspection.
 * ------------------------------------------------------------------ */

static int sched_rr_init(void *self)
{
    struct sched_rr_state *s = self;
    nx_list_init(&s->runqueue);
    s->quantum_ticks  = SCHED_RR_DEFAULT_QUANTUM_TICKS;
    s->remaining      = s->quantum_ticks;
    s->init_called++;
    return NX_OK;
}

static int sched_rr_enable(void *self)
{
    struct sched_rr_state *s = self;
    s->enable_called++;
    return NX_OK;
}

static int sched_rr_disable(void *self)
{
    struct sched_rr_state *s = self;
    s->disable_called++;
    /* Runqueue entries are borrowed task pointers — the scheduler
     * doesn't own them, so we leave the queue as-is.  A subsequent
     * enable() resumes with the same tasks queued. */
    return NX_OK;
}

static void sched_rr_destroy(void *self)
{
    struct sched_rr_state *s = self;
    s->destroy_called++;
    /* The framework owns state_buf allocation (nx_framework_
     * bootstrap), so there's nothing to free here — just note that
     * destroy fired so tests can observe lifecycle symmetry. */
}

const struct nx_component_ops sched_rr_component_ops = {
    .init    = sched_rr_init,
    .enable  = sched_rr_enable,
    .disable = sched_rr_disable,
    .destroy = sched_rr_destroy,
    /* No pause_hook: spawns_threads is false in the manifest.
     * No handle_msg: the scheduler is not IPC-driven in Phase 4. */
};

NX_COMPONENT_REGISTER_NO_DEPS_IFACE(sched_rr,
                                    struct sched_rr_state,
                                    &sched_rr_component_ops,
                                    &sched_rr_scheduler_ops);
