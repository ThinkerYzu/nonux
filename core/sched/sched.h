#ifndef NONUX_SCHED_SCHED_H
#define NONUX_SCHED_SCHED_H

#include <stdbool.h>

#include "core/sched/task.h"
#include "interfaces/scheduler.h"

/*
 * Core scheduler driver — policy-agnostic glue over the ARM64 context
 * switch.  See DESIGN.md §Scheduler: Core Driver + Component.
 *
 * Layering:
 *
 *   core/sched/sched.{h,c}       — THIS FILE: stashed g_sched/g_sched_self,
 *                                  reschedule shim, tick forwarding, yield,
 *                                  preempt counters, idle-task setup.
 *   components/sched_rr/         — Policy component implementing
 *                                  struct nx_scheduler_ops.  The core
 *                                  driver never dereferences slot->active;
 *                                  framework/bootstrap.c hands the policy's
 *                                  ops + self over via sched_init() at the
 *                                  end of boot bring-up.  R8 escape-hatch.
 *   core/cpu/vectors.S           — IRQ-return shim calls
 *                                  sched_check_resched before eret.
 *   core/timer/timer.c           — on_tick → sched_tick.
 */

/*
 * Save callee-saved state + SP + DAIF from `prev`, load same from `next`,
 * and update TPIDR_EL1 to `next`.  Implemented in core/cpu/context.S.
 *
 * Caller must hold preempt-disabled (via nx_preempt_disable) so the
 * currently-running task doesn't switch mid-save.  `prev` and `next` must
 * be non-NULL; `prev == next` is a no-op-on-paper but still legal.
 */
void cpu_switch_to(struct nx_task *prev, struct nx_task *next);

/* Increment / decrement the current task's preempt-count.  When the count
 * is > 0 the core scheduler will not preempt the current task; when it
 * returns to zero the reschedule shim checks `current->need_resched` and
 * may invoke pick_next + cpu_switch_to.  Nestable. */
void nx_preempt_disable(void);
void nx_preempt_enable(void);

/* Observability: current preempt count of the running task, or 0 if no
 * current task is set (boot-time, or host tests before they set one). */
int  nx_preempt_count(void);

/* ----- Slice 4.4 additions: policy handoff + scheduling verbs -------- */

/*
 * Stash the scheduler policy's ops/self pair.  Called once from
 * framework/bootstrap.c after bootstrap brings the scheduler component
 * to ACTIVE.  After this returns, timer ticks drive through
 * g_sched->tick(), and sched_check_resched dispatches through
 * g_sched->pick_next().
 *
 * Re-calling sched_init with a different (ops, self) is the basis for
 * runtime scheduler swap in Phase 8; Phase 4 calls it once at boot.
 */
void sched_init(const struct nx_scheduler_ops *ops, void *self);

/* True once sched_init has been called at least once. */
bool sched_is_initialized(void);

/* Introspection for the test harness — returns the stashed pair.
 * Intentionally not marked for general consumption; the real callers
 * are the timer ISR and the reschedule shim via the private accessor
 * inside sched.c. */
const struct nx_scheduler_ops *sched_ops_for_test(void);
void                          *sched_self_for_test(void);

/*
 * Transition the current boot-CPU context into the idle task.  Sets
 * TPIDR_EL1 to a statically-allocated idle_task struct, enqueues it on
 * the scheduler's runqueue (so pick_next can return it as a fallback
 * when no other task is ready), and returns.  The caller then enters
 * the idle loop (or, under NX_KTEST, drives into ktest_main which runs
 * in the idle-task context).
 *
 * Idempotent — calling sched_start twice is a no-op after the first.
 */
void sched_start(void);

/* Timer-ISR forwarder.  Calls g_sched->tick(g_sched_self) if the
 * scheduler is initialised; otherwise no-op.  Bounded and non-blocking. */
void sched_tick(void);

/*
 * Reschedule check.  Called from:
 *   - The IRQ-return shim in core/cpu/vectors.S, after on_irq returns.
 *   - nx_task_yield() for voluntary cooperation.
 *
 * Reads the current task's need_resched / preempt_count, and if
 * appropriate:
 *   1. Calls g_sched->yield() to let the policy rotate its runqueue.
 *   2. Calls g_sched->pick_next() to select the next task.
 *   3. Fires NX_HOOK_CONTEXT_SWITCH with { prev, next }.
 *   4. Calls cpu_switch_to(prev, next).
 *
 * Returns to the caller on the newly-switched-in task's stack.  The
 * outgoing task resumes here when some future switch selects it again.
 */
void sched_check_resched(void);

/*
 * Voluntary yield.  Flags need_resched on current and calls
 * sched_check_resched.  Legal to call from kthread bodies and tests;
 * MUST NOT be called from an ISR (which can't preempt itself).
 */
void nx_task_yield(void);

/*
 * Spawn a kernel thread and enqueue it on the scheduler.  `kstack_pages`
 * of PMM memory are allocated.  On success the returned task is ready
 * to run and the scheduler will pick it up on the next reschedule.
 *
 * This is a core primitive, not a component-spawned worker — the
 * slice-3.8 `spawns_threads ⇒ pause_hook` manifest rule does not apply.
 */
struct nx_task *sched_spawn_kthread(const char *name,
                                    void (*entry)(void *),
                                    void *arg);

#endif /* NONUX_SCHED_SCHED_H */
