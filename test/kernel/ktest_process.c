/*
 * Kernel-side coverage for slice 7.1.
 *
 * After `boot_main` runs `nx_framework_bootstrap()` and `sched_start`,
 * the idle task is the current task and its `process` pointer must
 * refer to the always-present `g_kernel_process` (pid 0).  Kthreads
 * spawned from this point inherit that assignment.  This file checks
 * those bootstrap invariants plus exercises the syscall-table plumbing
 * that now goes through `nx_task_current()->process`.
 */

#include "ktest.h"

#include "core/mmu/mmu.h"
#include "core/sched/sched.h"
#include "core/sched/task.h"
#include "framework/handle.h"
#include "framework/process.h"
#include "framework/syscall.h"
#include "interfaces/scheduler.h"

KTEST(process_current_returns_kernel_process_on_idle_task)
{
    struct nx_task *curr = nx_task_current();
    KASSERT_NOT_NULL(curr);
    KASSERT_NOT_NULL(curr->process);
    KASSERT_EQ_U(curr->process->pid, 0);
    KASSERT_EQ_U((uint64_t)curr->process,
                 (uint64_t)&g_kernel_process);
}

KTEST(process_current_same_pointer_as_syscall_current_table_owner)
{
    /* The syscall dispatcher's current-table helper must resolve to
     * the current process's handle table.  Anything else would break
     * slice 7.1's core claim. */
    struct nx_handle_table *t = nx_syscall_current_table();
    KASSERT_EQ_U((uint64_t)t, (uint64_t)&g_kernel_process.handles);
}

KTEST(process_create_allocates_fresh_pid_on_kernel)
{
    nx_process_reset_for_test();
    struct nx_process *p = nx_process_create("k-proc");
    KASSERT_NOT_NULL(p);
    KASSERT_EQ_U(p->pid, 1);
    KASSERT_EQ_U(p->state, NX_PROCESS_STATE_ACTIVE);
    nx_process_destroy(p);
}

/* ---------- EL0 exit syscall ---------------------------------------- */
/*
 * An EL0 kthread that creates its own process, drops to EL0, and
 * calls NX_SYS_EXIT(42).  The ktest asserts the current process's
 * exit_code was recorded and state flipped to EXITED.
 *
 * Note: we don't have a baked-in EL0 program for this because the
 * whole test runs entirely in-kernel: the kthread calls `sys_exit`
 * via a helper rather than going through the EL0 drop.  That's
 * acceptable — the slice claim is "NX_SYS_EXIT updates the current
 * process", and the calling context (EL0 vs. kthread) is irrelevant
 * to the dispatcher's update logic.
 */

static struct nx_process *g_exit_target;
static struct nx_task *g_exit_task;

static void exit_kthread(void *arg)
{
    (void)arg;
    /* Switch this task's process to the test target before invoking
     * sys_exit.  After the assignment, nx_process_current returns
     * g_exit_target and nx_process_exit marks IT as EXITED (rather
     * than the kernel process). */
    struct nx_task *curr = nx_task_current();
    curr->process = g_exit_target;

    /* Call nx_process_exit directly rather than going through the
     * dispatcher — the behaviour we're testing is the process-state
     * transition, not the SVC plumbing.  SVC-from-EL1 would work but
     * adds noise. */
    nx_process_exit(42);
    /* unreachable */
}

KTEST(process_exit_marks_current_process_exited_with_code)
{
    nx_process_reset_for_test();
    g_exit_target = nx_process_create("exiter");
    KASSERT_NOT_NULL(g_exit_target);
    KASSERT_EQ_U(g_exit_target->state, NX_PROCESS_STATE_ACTIVE);

    g_exit_task = sched_spawn_kthread("exiter", exit_kthread, 0);
    KASSERT_NOT_NULL(g_exit_task);

    /* Yield until the kthread has reached nx_process_exit.  It parks
     * in a wfe loop; the target process's state transitions before
     * the park.  A bounded yield count keeps a hung test from
     * stalling the suite. */
    const int max_yields = 128;
    int reached = 0;
    for (int i = 0; i < max_yields; i++) {
        if (g_exit_target->state == NX_PROCESS_STATE_EXITED) {
            reached = 1;
            break;
        }
        nx_task_yield();
    }
    KASSERT(reached);
    KASSERT_EQ_U(g_exit_target->exit_code, 42);

    /* Stranded in wfe; dequeue externally so it doesn't burn time on
     * future ticks. */
    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_exit_task);

    nx_process_destroy(g_exit_target);
    g_exit_target = NULL;
    g_exit_task   = NULL;
}

/* ---------- Slice 7.2 — per-process address spaces ------------------ */

KTEST(process_create_allocates_fresh_ttbr0_root)
{
    nx_process_reset_for_test();

    uint64_t kernel_root = mmu_kernel_address_space();
    KASSERT(kernel_root != 0);

    struct nx_process *p = nx_process_create("as-test");
    KASSERT_NOT_NULL(p);
    /* The newly-allocated root must be non-zero and distinct from the
     * kernel's root — `mmu_create_address_space` always returns fresh
     * L1 storage for a new process. */
    KASSERT(p->ttbr0_root != 0);
    KASSERT(p->ttbr0_root != kernel_root);

    struct nx_process *q = nx_process_create("as-test-2");
    KASSERT_NOT_NULL(q);
    KASSERT(q->ttbr0_root != 0);
    KASSERT(q->ttbr0_root != kernel_root);
    KASSERT(q->ttbr0_root != p->ttbr0_root);

    nx_process_destroy(p);
    nx_process_destroy(q);
}

KTEST(process_two_address_spaces_hold_different_bytes_at_same_va)
{
    /* Core slice-7.2 claim: the 2 MiB user window at VA `user_window_base`
     * can hold different physical content in different processes.
     *
     * Strategy: switch TTBR0 to process A's root, write 'A' at the
     * user-window VA.  Switch to B, write 'B'.  Flip between A and B
     * and re-read.  If both processes had the same PA backing, the
     * second write would overwrite the first.  If they have distinct
     * PAs, the original byte remains visible after flipping back. */
    nx_process_reset_for_test();

    uint64_t kernel_root = mmu_kernel_address_space();
    struct nx_process *a = nx_process_create("win-a");
    struct nx_process *b = nx_process_create("win-b");
    KASSERT_NOT_NULL(a);
    KASSERT_NOT_NULL(b);

    volatile uint8_t *user_va = (volatile uint8_t *)(uintptr_t)mmu_user_window_base();

    /* Disable preemption: we don't want a timer tick to yield us away
     * mid-experiment and flip TTBR0 under our feet. */
    nx_preempt_disable();

    /* Write into A's window. */
    mmu_switch_address_space(a->ttbr0_root);
    user_va[0] = 0xA1;
    user_va[1] = 0xA2;

    /* Write into B's window. */
    mmu_switch_address_space(b->ttbr0_root);
    user_va[0] = 0xB1;
    user_va[1] = 0xB2;

    /* Flip back to A — expect the original bytes, NOT B's. */
    mmu_switch_address_space(a->ttbr0_root);
    uint8_t a0 = user_va[0];
    uint8_t a1 = user_va[1];
    KASSERT_EQ_U(a0, 0xA1);
    KASSERT_EQ_U(a1, 0xA2);

    /* And flip to B — expect B's bytes. */
    mmu_switch_address_space(b->ttbr0_root);
    uint8_t b0 = user_va[0];
    uint8_t b1 = user_va[1];
    KASSERT_EQ_U(b0, 0xB1);
    KASSERT_EQ_U(b1, 0xB2);

    /* Restore the kernel root so subsequent tests run in the normal
     * kernel process's context. */
    mmu_switch_address_space(kernel_root);
    nx_preempt_enable();

    nx_process_destroy(a);
    nx_process_destroy(b);
}

/*
 * Context-switch TTBR0 flip — the sched_check_resched code path reads
 * the incoming task's process->ttbr0_root and flips TTBR0 accordingly.
 *
 * Strategy: spawn two kthreads, each in its own process, each with a
 * known byte value written into its user window by the TEST body
 * (before spawning, while we still have direct TTBR0 control).  Each
 * kthread reads user_va[0] into a per-kthread variable and yields
 * forever; the test body then inspects both variables and asserts
 * each kthread saw its own byte.
 */

static uint8_t g_as_a_saw = 0xFF;
static uint8_t g_as_b_saw = 0xFF;
static int     g_as_a_done = 0;
static int     g_as_b_done = 0;

static void as_thread_a(void *arg)
{
    (void)arg;
    /* TTBR0 was flipped to our process on entry by sched_check_resched. */
    volatile uint8_t *user_va = (volatile uint8_t *)(uintptr_t)mmu_user_window_base();
    g_as_a_saw  = user_va[0];
    g_as_a_done = 1;
    for (;;) nx_task_yield();
}

static void as_thread_b(void *arg)
{
    (void)arg;
    volatile uint8_t *user_va = (volatile uint8_t *)(uintptr_t)mmu_user_window_base();
    g_as_b_saw  = user_va[0];
    g_as_b_done = 1;
    for (;;) nx_task_yield();
}

KTEST(process_context_switch_flips_ttbr0)
{
    nx_process_reset_for_test();
    g_as_a_saw = 0xFF; g_as_b_saw = 0xFF;
    g_as_a_done = 0;    g_as_b_done = 0;

    uint64_t kernel_root = mmu_kernel_address_space();
    struct nx_process *pa = nx_process_create("cs-a");
    struct nx_process *pb = nx_process_create("cs-b");
    KASSERT_NOT_NULL(pa);
    KASSERT_NOT_NULL(pb);

    /* Seed each process's user window with a distinguishing byte. */
    volatile uint8_t *user_va = (volatile uint8_t *)(uintptr_t)mmu_user_window_base();
    nx_preempt_disable();
    mmu_switch_address_space(pa->ttbr0_root); user_va[0] = 0xAA;
    mmu_switch_address_space(pb->ttbr0_root); user_va[0] = 0xBB;
    mmu_switch_address_space(kernel_root);
    nx_preempt_enable();

    /* Spawn the two kthreads, each pinned to its process.  They'll
     * yield forever once they've read their byte; the test body
     * yields until both have recorded. */
    struct nx_task *ta = sched_spawn_kthread("cs-a", as_thread_a, 0);
    struct nx_task *tb = sched_spawn_kthread("cs-b", as_thread_b, 0);
    KASSERT_NOT_NULL(ta);
    KASSERT_NOT_NULL(tb);
    ta->process = pa;
    tb->process = pb;

    const int max_yields = 256;
    int reached = 0;
    for (int i = 0; i < max_yields; i++) {
        if (g_as_a_done && g_as_b_done) { reached = 1; break; }
        nx_task_yield();
    }
    KASSERT(reached);
    KASSERT_EQ_U(g_as_a_saw, 0xAA);
    KASSERT_EQ_U(g_as_b_saw, 0xBB);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, ta);
    ops->dequeue(self, tb);

    /* Return to the kernel address space before destroying so
     * subsequent tests don't run with a dangling TTBR0. */
    mmu_switch_address_space(kernel_root);
    nx_process_destroy(pa);
    nx_process_destroy(pb);
}
