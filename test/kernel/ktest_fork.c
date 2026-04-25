/*
 * Kernel-side coverage for slice 7.4a — fork.
 *
 * Spawns an EL0 program that calls `NX_SYS_FORK` and takes
 * different UART-visible branches based on the return value.
 * Observable invariants:
 *
 *   - `debug_write` counter reaches ≥ 2 (parent writes
 *     "[fork-parent]", child writes "[fork-child]"; each is one
 *     NX_SYS_DEBUG_WRITE, so the counter rises by 2).
 *   - Live ktest log contains BOTH markers.  Proves the parent
 *     returned from fork with a nonzero x0 and the child
 *     returned with x0 == 0 — i.e. the trap-frame replay worked
 *     and the child's address space was populated with a copy of
 *     the parent's code.
 *   - Process count includes at least two user processes
 *     (parent + child).
 */

#include "ktest.h"

#include "core/cpu/exception.h"
#include "core/mmu/mmu.h"
#include "core/sched/sched.h"
#include "core/sched/task.h"
#include "framework/process.h"
#include "framework/syscall.h"
#include "interfaces/scheduler.h"

extern char __user_fork_prog_start[];
extern char __user_fork_prog_end[];

static struct nx_process *g_fork_parent;
static struct nx_task    *g_fork_task;

static void fork_copy_prog_to_window(void *dst)
{
    size_t len = (size_t)(__user_fork_prog_end - __user_fork_prog_start);
    const char *src = __user_fork_prog_start;
    char       *dp  = dst;
    for (size_t i = 0; i < len; i++) dp[i] = src[i];
}

static void fork_el0_kthread(void *arg)
{
    (void)arg;
    /* sched_check_resched already flipped TTBR0 to our process's
     * root on the way in.  Copy the EL0 program bytes into the
     * process's user-window backing via the MMU alias (avoids
     * depending on whichever TTBR0 is active — writes go to the
     * specific process's backing regardless). */
    void *backing = mmu_address_space_user_backing(g_fork_parent->ttbr0_root);
    fork_copy_prog_to_window(backing);
    /* Make freshly-written bytes reachable as instructions. */
    asm volatile ("dsb ish"  ::: "memory");
    asm volatile ("ic iallu" ::: "memory");
    asm volatile ("dsb ish"  ::: "memory");
    asm volatile ("isb");

    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(base, sp_el0);
}

KTEST(fork_el0_parent_and_child_each_emit_their_marker)
{
    nx_syscall_reset_for_test();
    nx_process_reset_for_test();
    KASSERT_EQ_U(nx_syscall_debug_write_calls(), 0);
    KASSERT_EQ_U(nx_process_count(), 1);      /* just the kernel process */

    g_fork_parent = nx_process_create("fork-parent");
    KASSERT_NOT_NULL(g_fork_parent);

    g_fork_task = sched_spawn_kthread("fork-el0", fork_el0_kthread, 0,
                                      g_fork_parent);
    KASSERT_NOT_NULL(g_fork_task);

    /* Yield until the debug_write counter reaches 2 — one from the
     * parent, one from the child.  Gives the scheduler time to
     * pick the forked child and run its trap-frame replay. */
    const int max_yields = 512;
    int reached = 0;
    for (int i = 0; i < max_yields; i++) {
        if (nx_syscall_debug_write_calls() >= 2) { reached = 1; break; }
        nx_task_yield();
    }
    KASSERT(reached);

    /* Process count now includes the kernel process + parent +
     * child = 3.  (The child was created by sys_fork during the
     * run above.) */
    KASSERT(nx_process_count() >= 3);

    /* Both EL0 tasks are parked in wfe — dequeue them so they
     * don't burn later timeslices.  The child task was enqueued
     * directly by sys_fork; walk the scheduler to find and
     * dequeue everything that isn't the current (test body)
     * task.  Simpler: the parent task we spawned is
     * `g_fork_task`; the child task is whatever else appeared.
     * We don't have an easy handle on the child task pointer, so
     * instead we just dequeue the parent + rely on the idle path
     * to eventually starve the child of cycles (it's parked in
     * wfe, so it just sits there until the next test reclaims). */
    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_fork_task);

    /* Return to kernel TTBR0 so the next test isn't accidentally
     * running against the parent's (soon-to-be-destroyed)
     * address space. */
    mmu_switch_address_space(mmu_kernel_address_space());

    /* Leave the child task stranded (it's parked in wfe and not
     * on the runqueue's head under round-robin it might still
     * cycle once per sweep but never does anything).  A proper
     * reap lands with slice 7.4c's wait().  Same convention the
     * other EL0 ktests use for their stranded tasks. */
    g_fork_parent = NULL;
    g_fork_task   = NULL;
}
