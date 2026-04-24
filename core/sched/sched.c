/*
 * Core scheduler driver — slice 4.4.
 *
 * Holds the stashed policy pointer (g_sched / g_sched_self), the
 * reschedule shim that runs at IRQ-return and after yield, and the
 * transition from boot context to idle task.  See DESIGN.md
 * §Scheduler: Core Driver + Component.
 *
 * R8 escape-hatch: the timer ISR never dereferences slot->active; it
 * sets current->need_resched and the IRQ-return path calls
 * sched_check_resched() on the interrupted task's kernel stack.  The
 * stashed g_sched pointer is set once at boot by sched_init() and
 * read from this file only.
 */

#include "core/sched/sched.h"
#include "core/sched/task.h"
#include "framework/hook.h"
#include "framework/process.h"
#if !__STDC_HOSTED__
#include "core/mmu/mmu.h"
#endif

#if !__STDC_HOSTED__
#include "core/lib/lib.h"
#endif

/* --- preempt counters (unchanged from slice 4.1) --------------------- */

void nx_preempt_disable(void)
{
    struct nx_task *t = nx_task_current();
    if (!t) return;
    t->preempt_count++;
}

void nx_preempt_enable(void)
{
    struct nx_task *t = nx_task_current();
    if (!t) return;
    if (t->preempt_count > 0)
        t->preempt_count--;
}

int nx_preempt_count(void)
{
    struct nx_task *t = nx_task_current();
    return t ? t->preempt_count : 0;
}

/* --- stashed policy pair + sched_init -------------------------------- */

static const struct nx_scheduler_ops *g_sched_ops;
static void                          *g_sched_self;

void sched_init(const struct nx_scheduler_ops *ops, void *self)
{
    /* Publish ops first then self, so a concurrent reader that sees a
     * non-NULL ops never sees a stale self.  On single-core Phase 4
     * the ordering is enforced by the compiler's sequencing; SMP will
     * need explicit release semantics. */
    g_sched_ops  = ops;
    g_sched_self = self;
}

bool sched_is_initialized(void)
{
    return g_sched_ops != NULL;
}

const struct nx_scheduler_ops *sched_ops_for_test(void) { return g_sched_ops; }
void                          *sched_self_for_test(void) { return g_sched_self; }

/* --- idle task + sched_start ----------------------------------------- */

struct nx_task  g_idle_task;   /* non-static — debug prints want it */
static bool     g_sched_started;

static void idle_task_init(void)
{
    const char *name = "idle";
    int i = 0;
    for (; i < NX_TASK_NAME_MAX - 1 && name[i]; i++)
        g_idle_task.name[i] = name[i];
    g_idle_task.name[i] = '\0';

    g_idle_task.id             = 0;        /* 0 is reserved for idle */
    g_idle_task.state          = NX_TASK_RUNNING;
    g_idle_task.preempt_count  = 0;
    g_idle_task.need_resched   = 0;
    g_idle_task.kstack_base    = NULL;     /* boot stack — not tracked here */
    g_idle_task.kstack_size    = 0;
    g_idle_task.sched_node.next = &g_idle_task.sched_node;
    g_idle_task.sched_node.prev = &g_idle_task.sched_node;
    /* Slice 7.1: idle belongs to the kernel process (pid 0).  Any
     * kthread spawned before someone explicitly reassigns its
     * `process` pointer inherits this default (see nx_task_create). */
    g_idle_task.process        = &g_kernel_process;
    /* cpu_ctx is undefined — the very first cpu_switch_to saves into
     * it and every subsequent one uses it.  We'll only ever switch
     * AWAY from idle (the boot CPU is already running in idle's
     * context), so the initial cpu_ctx values don't matter. */
}

void sched_start(void)
{
    if (g_sched_started) return;
    g_sched_started = true;

    idle_task_init();

#if !__STDC_HOSTED__
    /* Publish current = idle via TPIDR_EL1 so nx_task_current() returns
     * sane values from here on, including from the timer ISR. */
    asm volatile("msr tpidr_el1, %0" :: "r"(&g_idle_task));
#endif

    /* If the scheduler is up, enqueue idle as a permanent fallback. */
    if (g_sched_ops)
        g_sched_ops->enqueue(g_sched_self, &g_idle_task);
}

/* --- tick + reschedule shim ------------------------------------------ */

void sched_tick(void)
{
    if (!g_sched_ops) return;
    g_sched_ops->tick(g_sched_self);
}

void sched_check_resched(void)
{
    struct nx_task *curr = nx_task_current();
    if (!curr) return;
    if (!g_sched_ops) return;
    if (!curr->need_resched) return;
    if (curr->preempt_count > 0) return;

    curr->need_resched = 0;

    /* Let the policy rotate its runqueue — for round-robin this moves
     * the head (current) to the tail so the next pick_next returns a
     * different task if one exists. */
    g_sched_ops->yield(g_sched_self);

    struct nx_task *next = g_sched_ops->pick_next(g_sched_self);
    if (!next || next == curr) return;

    /* Fire NX_HOOK_CONTEXT_SWITCH (slice 4.4) — hooks run with
     * preempt-disabled on the outgoing task's kernel stack. */
    nx_preempt_disable();
    struct nx_hook_context hctx = {
        .point   = NX_HOOK_CONTEXT_SWITCH,
        .u.csw   = { .prev = curr, .next = next },
    };
    nx_hook_dispatch(&hctx);

    /* Slice 7.2: flip TTBR0 if the incoming task lives in a different
     * process.  Every address-space root shares the kernel's identity
     * map for the code page currently executing, so the instruction
     * fetch after the `isb` inside `mmu_switch_address_space` resolves
     * unchanged.  The TLB flush makes stale user-region entries from
     * the outgoing process invisible to the incoming one. */
#if !__STDC_HOSTED__
    if (next->process && curr->process &&
        next->process != curr->process &&
        next->process->ttbr0_root != curr->process->ttbr0_root &&
        next->process->ttbr0_root != 0) {
        mmu_switch_address_space(next->process->ttbr0_root);
    }
#endif

    cpu_switch_to(curr, next);
    /* Resumes here whenever some future switch selects `curr` again.
     * preempt_enable runs in the resumed task's context. */
    nx_preempt_enable();
}

void nx_task_yield(void)
{
    struct nx_task *curr = nx_task_current();
    if (!curr) return;
    curr->need_resched = 1;
    sched_check_resched();
}

/* --- kthread spawn --------------------------------------------------- */

struct nx_task *sched_spawn_kthread(const char *name,
                                    void (*entry)(void *),
                                    void *arg)
{
    struct nx_task *t = nx_task_create(name, entry, arg, 1);
    if (!t) return NULL;
    if (g_sched_ops) {
        int rc = g_sched_ops->enqueue(g_sched_self, t);
        if (rc != 0) {
            nx_task_destroy(t);
            return NULL;
        }
    }
    return t;
}
