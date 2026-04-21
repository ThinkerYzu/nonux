#ifndef NONUX_SCHED_SCHED_H
#define NONUX_SCHED_SCHED_H

#include "core/sched/task.h"

/*
 * Core scheduler driver — policy-agnostic glue over the ARM64 context
 * switch.  See DESIGN.md §Scheduler: Core Driver + Component.
 *
 * Slice 4.1: just the context-switch primitive + preempt counters.
 * Slice 4.4 adds `sched_init` / `sched_start` / `sched_tick` /
 * `sched_check_resched` and the stashed `g_sched` policy pointer.
 */

/*
 * Save callee-saved state + SP from `prev`, load same from `next`, and
 * update TPIDR_EL1 to `next`.  Implemented in core/cpu/context.S.
 *
 * Caller must hold preempt-disabled (via nx_preempt_disable) so the
 * currently-running task doesn't switch mid-save.  `prev` and `next` must
 * be non-NULL; `prev == next` is a no-op-on-paper but still legal.
 */
void cpu_switch_to(struct nx_task *prev, struct nx_task *next);

/* Increment / decrement the current task's preempt-count.  When the count
 * is > 0 the core scheduler will not preempt the current task; when it
 * returns to zero the reschedule shim checks `current->need_resched` and
 * may invoke pick_next + cpu_switch_to.  (Slice 4.4 wires the shim; slice
 * 4.1 only provides the counter so host tests can exercise nesting.) */
void nx_preempt_disable(void);
void nx_preempt_enable(void);

/* Observability: current preempt count of the running task, or 0 if no
 * current task is set (boot-time, or host tests before they set one). */
int  nx_preempt_count(void);

#endif /* NONUX_SCHED_SCHED_H */
