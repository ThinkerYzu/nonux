#ifndef NONUX_SCHED_TASK_H
#define NONUX_SCHED_TASK_H

#include <stddef.h>
#include <stdint.h>

#include "core/lib/list.h"

/*
 * Task + saved CPU context.  This is the frozen core-side view of a
 * schedulable entity.  Policy (runqueue layout, quantum accounting) lives
 * in components/sched_rr/; this header only commits to the fields that
 * cpu_switch_to touches and that every policy needs.
 *
 * See DESIGN.md §Scheduler: Core Driver + Component.
 */

/*
 * Saved callee-saved state for an ARM64 task.
 *
 *   offset 0x00:  x19 / x20     (stp pair)
 *   offset 0x10:  x21 / x22
 *   offset 0x20:  x23 / x24
 *   offset 0x30:  x25 / x26
 *   offset 0x40:  x27 / x28
 *   offset 0x50:  x29 / x30     (FP / LR)
 *   offset 0x60:  sp
 *   offset 0x68:  daif (exception mask — I/F/A/D bits)
 *
 * The caller-saved regs (x0..x18) are not saved here: `cpu_switch_to` is a
 * plain C call from the scheduler driver, so AAPCS lets the compiler treat
 * them as clobbered across the call.  No FP state because the kernel builds
 * with `-mgeneral-regs-only`.
 *
 * DAIF (the IRQ/FIQ/SError/debug mask bits) is saved and restored per-task
 * so that a task interrupted mid-IRQ-handler (DAIF-I masked) can coexist
 * with a task that yielded voluntarily (DAIF-I enabled).  Without this the
 * first involuntary switch at IRQ-return would carry the ISR's masked state
 * into a yielding task, and the CPU would never take another tick.
 *
 * `__attribute__((aligned(16)))` keeps every stp/ldp pair on a 16-byte
 * boundary so `-mstrict-align` stays happy while MMU is off.
 */
struct nx_cpu_ctx {
    uint64_t x19, x20;
    uint64_t x21, x22;
    uint64_t x23, x24;
    uint64_t x25, x26;
    uint64_t x27, x28;
    uint64_t x29, x30;   /* fp, lr */
    uint64_t sp;
    uint64_t daif;
} __attribute__((aligned(16)));

enum nx_task_state {
    NX_TASK_READY    = 0,
    NX_TASK_RUNNING  = 1,
    NX_TASK_BLOCKED  = 2,
    NX_TASK_ZOMBIE   = 3,
};

#define NX_TASK_NAME_MAX 16

/*
 * Forward declaration — `struct nx_process` is defined in
 * framework/process.h.  Tasks link back to their owning process so
 * the syscall dispatcher can resolve the current handle table
 * without a separate global; inherited by `nx_task_create` from the
 * caller's current process (falling back to `&g_kernel_process`
 * during bootstrap).  Slice 7.1 added this field.
 */
struct nx_process;

/*
 * `cpu_ctx` is first so `cpu_switch_to(struct task *)` can treat the task
 * pointer as a `struct nx_cpu_ctx *` without indexing.  `_Static_assert`
 * below pins this.
 */
struct nx_task {
    struct nx_cpu_ctx   cpu_ctx;
    uint32_t            id;
    char                name[NX_TASK_NAME_MAX];
    enum nx_task_state  state;
    int                 preempt_count;
    int                 need_resched;
    void               *kstack_base;
    size_t              kstack_size;
    struct nx_list_node sched_node;
    struct nx_process  *process;
};

_Static_assert(offsetof(struct nx_task, cpu_ctx) == 0,
               "nx_task.cpu_ctx must be at offset 0 — cpu_switch_to depends on it");

/*
 * Task primitives.
 *
 *   nx_task_create:  allocates a kernel stack of `kstack_pages` 4 KiB pages,
 *                    preps `cpu_ctx` so the first cpu_switch_to lands at
 *                    entry(arg), returns the task.
 *   nx_task_destroy: releases kernel stack, returns struct to PMM slab heap.
 *   nx_task_current: returns the task pointer cached in TPIDR_EL1 on the
 *                    kernel build, or a host-visible `g_current` on the
 *                    host build.  Slice 4.4 populates TPIDR_EL1 during the
 *                    first context switch.
 */

struct nx_task *nx_task_create(const char *name,
                               void (*entry)(void *),
                               void *arg,
                               size_t kstack_pages);

/*
 * Slice 7.4: create a child task whose first resume replays a
 * parent's trap frame.  `parent_tf` is the trap frame the parent
 * was running under at its fork SVC; this helper allocates a
 * fresh kernel stack, byte-copies the 272-byte trap frame onto
 * its top, rewrites the copy's `x[0]` to 0 (the child's fork
 * return value), and sets `cpu_ctx` so the first `cpu_switch_to`
 * into this task lands at `nx_task_fork_child_entry` (a thunk in
 * `vectors.S` that does RESTORE_TRAPFRAME + eret).
 *
 * Caller sets `task->process` to the already-forked child
 * process before enqueueing.  Kernel-only — host build has no
 * trap frame concept and stubs this out.
 *
 * Returns NULL on kstack/struct allocation failure.
 */
struct trap_frame;  /* forward decl; full def in core/cpu/exception.h */
struct nx_task *nx_task_create_forked(const char *name,
                                      const struct trap_frame *parent_tf);

void            nx_task_destroy(struct nx_task *t);
struct nx_task *nx_task_current(void);

/* Host-test-only: override the current-task pointer (used by preempt-count
 * tests that have no real scheduling context). */
#if __STDC_HOSTED__
void            nx_task_set_current_for_test(struct nx_task *t);
#endif

#endif /* NONUX_SCHED_TASK_H */
