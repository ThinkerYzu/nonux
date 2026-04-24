/*
 * Task primitives for the core scheduler driver.  Slice 4.1.
 *
 * Responsibilities at this slice:
 *   - Allocate a task + its kernel stack.
 *   - Hand-craft `cpu_ctx` so the very first cpu_switch_to into the new
 *     task lands at `entry(arg)`.
 *   - Track "current task".  On the kernel build this lives in TPIDR_EL1;
 *     on the host build it's a plain static variable that host tests flip
 *     with nx_task_set_current_for_test() (host doesn't have TPIDR_EL1 and
 *     doesn't actually context-switch in tests — the save/restore and
 *     TPIDR_EL1 update are kernel-test material).
 *
 * Slice 4.4 adds the idle task, the first-switch ("sched_start"), and the
 * IRQ-return reschedule shim.
 */

#include "core/sched/task.h"
#include "core/sched/sched.h"
#include "framework/process.h"
#if !__STDC_HOSTED__
#include "core/cpu/exception.h"
#endif

#if __STDC_HOSTED__
#include <stdlib.h>
#include <string.h>
#else
#include "core/lib/kheap.h"
#include "core/lib/lib.h"
#include "core/pmm/pmm.h"
#endif

/* context.S first-switch thunk.  On the very first `cpu_switch_to` into a
 * newly-created task the saved LR (x30) is the address of this thunk; the
 * entry function + arg are stashed in x19 and x20 respectively so that
 * when the thunk runs it can restore them and jump to entry(arg). */
extern void nx_task_bootstrap(void);

#if __STDC_HOSTED__
/*
 * Host-build stub for cpu_switch_to.  The real implementation lives in
 * core/cpu/context.S (ARM64 assembly) which cannot be linked into a
 * host x86-64 build.  Host tests never drive the scheduler into an
 * actual context switch — sched_check_resched's precondition
 * (current->need_resched && !preempt_count) is never reached from
 * host tests — but sched.c references the symbol unconditionally so
 * the linker needs a definition.  Abort if anything does reach it.
 */
void cpu_switch_to(struct nx_task *prev, struct nx_task *next)
{
    (void)prev; (void)next;
    abort();
}
#endif

/* 0 is reserved for "no task" so preempt accounting and TPIDR_EL1 can both
 * use NULL-as-none without colliding. */
static _Atomic uint32_t g_task_id_seq = 1;

#if __STDC_HOSTED__
static struct nx_task *g_current;
#endif

/* --- kernel-stack allocation ----------------------------------------- */

static void *alloc_kstack(size_t pages)
{
    if (pages == 0) pages = 1;
#if __STDC_HOSTED__
    /* Host tests don't run on the allocated stack — cpu_switch_to isn't
     * exercised on host.  A plain malloc is enough to prove task_create
     * populates cpu_ctx->sp correctly. */
    return malloc(pages * 4096U);
#else
    return pmm_alloc_pages(pages);
#endif
}

static void free_kstack(void *p, size_t pages)
{
    if (!p) return;
#if __STDC_HOSTED__
    (void)pages;
    free(p);
#else
    pmm_free_pages(p, pages);
#endif
}

/* --- struct nx_task allocation --------------------------------------- */

static struct nx_task *alloc_task_struct(void)
{
#if __STDC_HOSTED__
    struct nx_task *t = calloc(1, sizeof *t);
    return t;
#else
    struct nx_task *t = calloc(1, sizeof *t);
    return t;
#endif
}

static void free_task_struct(struct nx_task *t)
{
    free(t);
}

/* --- public API ------------------------------------------------------ */

struct nx_task *nx_task_create(const char *name,
                               void (*entry)(void *),
                               void *arg,
                               size_t kstack_pages)
{
    if (!entry) return NULL;

    struct nx_task *t = alloc_task_struct();
    if (!t) return NULL;

    void *stack = alloc_kstack(kstack_pages);
    if (!stack) {
        free_task_struct(t);
        return NULL;
    }

    t->id = __atomic_fetch_add(&g_task_id_seq, 1, __ATOMIC_RELAXED);

    /* Copy name with truncation.  We do it byte-wise so the host and
     * freestanding builds match without pulling strncpy in. */
    size_t i = 0;
    if (name) {
        for (; i < NX_TASK_NAME_MAX - 1 && name[i]; i++)
            t->name[i] = name[i];
    }
    t->name[i] = '\0';

    t->state         = NX_TASK_READY;
    t->preempt_count = 0;
    t->need_resched  = 0;
    t->kstack_base   = stack;
    t->kstack_size   = (kstack_pages ? kstack_pages : 1) * 4096U;
    t->sched_node.next = &t->sched_node;
    t->sched_node.prev = &t->sched_node;

    /* SP starts at the top of the kstack, 16-byte aligned.  ARM64 AAPCS
     * requires 16-byte alignment at any public interface boundary, which
     * the thunk counts as. */
    uintptr_t sp_top = (uintptr_t)stack + t->kstack_size;
    sp_top &= ~(uintptr_t)0xf;

    /* Hand-craft cpu_ctx:
     *   - x19 holds entry pointer
     *   - x20 holds arg
     *   - x30 (LR) points at the first-switch thunk, which restores
     *     x19/x20 and jumps to entry(arg).
     *   - SP points at the top of the kstack.
     * The first `ret` inside cpu_switch_to will branch to x30 ==
     * nx_task_bootstrap, which unpacks the entry call.
     *
     * On the host build we never actually run the thunk — host tests
     * just inspect cpu_ctx fields — but we still populate them so the
     * same invariants are testable cross-build. */
    t->cpu_ctx.x19 = (uint64_t)(uintptr_t)entry;
    t->cpu_ctx.x20 = (uint64_t)(uintptr_t)arg;
    t->cpu_ctx.x29 = 0;  /* FP */
#if __STDC_HOSTED__
    /* On host, nx_task_bootstrap is not linked (it's an .S symbol).
     * Put a canary here so assertions can tell "never switched" from
     * "switched and returned". */
    t->cpu_ctx.x30 = (uint64_t)0xDEAD0000BEEF0000ULL;
#else
    t->cpu_ctx.x30 = (uint64_t)(uintptr_t)&nx_task_bootstrap;
#endif
    t->cpu_ctx.sp   = (uint64_t)sp_top;
    t->cpu_ctx.daif = 0;  /* IRQs enabled for a fresh kthread */

    /* Inherit process from the caller (slice 7.1).  Fall back to the
     * kernel process if there's no current task yet — that covers
     * every kthread spawned before sched_start publishes idle as the
     * current task. */
    struct nx_task *caller = nx_task_current();
    t->process = (caller && caller->process) ? caller->process
                                             : &g_kernel_process;

    return t;
}

struct nx_task *nx_task_create_forked(const char *name,
                                      const struct trap_frame *parent_tf)
{
#if __STDC_HOSTED__
    /* Host has no trap frames or EL0 — a fork-forked task is
     * meaningless outside the kernel.  Stub out. */
    (void)name; (void)parent_tf;
    return NULL;
#else
    if (!parent_tf) return NULL;

    struct nx_task *t = alloc_task_struct();
    if (!t) return NULL;

    void *stack = alloc_kstack(1);
    if (!stack) {
        free_task_struct(t);
        return NULL;
    }

    t->id = __atomic_fetch_add(&g_task_id_seq, 1, __ATOMIC_RELAXED);

    size_t i = 0;
    if (name) {
        for (; i < NX_TASK_NAME_MAX - 1 && name[i]; i++)
            t->name[i] = name[i];
    }
    t->name[i] = '\0';

    t->state         = NX_TASK_READY;
    t->preempt_count = 0;
    t->need_resched  = 0;
    t->kstack_base   = stack;
    t->kstack_size   = 4096u;
    t->sched_node.next = &t->sched_node;
    t->sched_node.prev = &t->sched_node;

    /* Top of kstack, 16-byte aligned. */
    uintptr_t sp_top = (uintptr_t)stack + t->kstack_size;
    sp_top &= ~(uintptr_t)0xf;

    /* Reserve sizeof(struct trap_frame) bytes for the child's
     * replay frame.  The trap-frame layout in SAVE_TRAPFRAME uses
     * exactly 272 bytes; `struct trap_frame` in exception.h is
     * shaped to match (31 * 8 + 3 * 8 = 272).  Align to 16 so the
     * restore's stp/ldp stay happy. */
    uintptr_t frame_addr = sp_top - sizeof(struct trap_frame);
    frame_addr &= ~(uintptr_t)0xf;

    struct trap_frame *child_frame = (struct trap_frame *)frame_addr;
    *child_frame = *parent_tf;          /* byte-copy the parent's frame */
    child_frame->x[0] = 0;              /* child's fork return value */

    /* Callee-saved regs are never read by `nx_task_fork_child_entry`
     * (the thunk immediately restores from the trap frame).  Zero
     * them so a future hook that inspects the context finds a
     * clean state. */
    t->cpu_ctx.x19 = 0;
    t->cpu_ctx.x20 = 0;
    t->cpu_ctx.x21 = 0;
    t->cpu_ctx.x22 = 0;
    t->cpu_ctx.x23 = 0;
    t->cpu_ctx.x24 = 0;
    t->cpu_ctx.x25 = 0;
    t->cpu_ctx.x26 = 0;
    t->cpu_ctx.x27 = 0;
    t->cpu_ctx.x28 = 0;
    t->cpu_ctx.x29 = 0;  /* FP */

    /* LR = fork-child thunk; SP = frame base.  cpu_switch_to's
     * first-time entry into this task lands directly in the thunk
     * with SP pointing at a valid trap frame. */
    extern void nx_task_fork_child_entry(void);
    t->cpu_ctx.x30 = (uint64_t)(uintptr_t)&nx_task_fork_child_entry;
    t->cpu_ctx.sp   = (uint64_t)frame_addr;
    t->cpu_ctx.daif = 0;

    /* Inherit process — caller may override to the forked child
     * before enqueueing. */
    struct nx_task *caller = nx_task_current();
    t->process = (caller && caller->process) ? caller->process
                                             : &g_kernel_process;
    return t;
#endif
}

void nx_task_destroy(struct nx_task *t)
{
    if (!t) return;

    size_t pages = t->kstack_size / 4096U;
    if (pages == 0) pages = 1;
    free_kstack(t->kstack_base, pages);
    t->kstack_base = NULL;
    free_task_struct(t);
}

struct nx_task *nx_task_current(void)
{
#if __STDC_HOSTED__
    return g_current;
#else
    struct nx_task *t;
    asm volatile("mrs %0, tpidr_el1" : "=r"(t));
    return t;
#endif
}

#if __STDC_HOSTED__
void nx_task_set_current_for_test(struct nx_task *t)
{
    g_current = t;
}
#endif
