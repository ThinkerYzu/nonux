/*
 * Kernel-side coverage for slice 7.4b — wait.
 *
 * Spawns an EL0 program that forks, has the child exit(42), and
 * has the parent wait on the child.  If wait delivers the status
 * correctly, the parent emits a final `[wait-ok]` marker.
 *
 * Observable invariants:
 *   - debug_write counter reaches ≥ 3: one from parent ("[wait-
 *     parent]"), one from child ("[wait-child]"), one from parent
 *     after wait ("[wait-ok]").
 *   - Live ktest log contains all three markers.
 *   - The child process's state is EXITED with exit_code == 42
 *     after the run.
 */

#include "ktest.h"

#include "core/cpu/exception.h"
#include "core/mmu/mmu.h"
#include "core/sched/sched.h"
#include "core/sched/task.h"
#include "framework/process.h"
#include "framework/syscall.h"
#include "interfaces/scheduler.h"

extern char __user_wait_prog_start[];
extern char __user_wait_prog_end[];

static struct nx_process *g_wait_parent;
static struct nx_task    *g_wait_task;

static void wait_copy_prog_to_window(void *dst)
{
    size_t len = (size_t)(__user_wait_prog_end - __user_wait_prog_start);
    const char *src = __user_wait_prog_start;
    char       *dp  = dst;
    for (size_t i = 0; i < len; i++) dp[i] = src[i];
}

static void wait_el0_kthread(void *arg)
{
    (void)arg;
    void *backing = mmu_address_space_user_backing(g_wait_parent->ttbr0_root);
    wait_copy_prog_to_window(backing);
    asm volatile ("dsb ish"  ::: "memory");
    asm volatile ("ic iallu" ::: "memory");
    asm volatile ("dsb ish"  ::: "memory");
    asm volatile ("isb");

    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(base, sp_el0);
}

KTEST(wait_fork_child_exit_42_returns_status_to_parent)
{
    /* NOTE: deliberately NOT calling nx_process_reset_for_test here.
     * Earlier ktests (e.g., ktest_fork) create their own processes
     * and strand their EL0 tasks in `wfe`.  Wiping the process
     * table would leave those stranded tasks with dangling
     * `process` pointers — the next time the scheduler picks one,
     * `sched_check_resched` dereferences the garbage and flips
     * TTBR0 to a bad value.  Instead we let prior processes live
     * on (their tasks keep parking in `wfe` harmlessly) and just
     * reset the syscall debug-write counter. */
    nx_syscall_reset_for_test();
    KASSERT_EQ_U(nx_syscall_debug_write_calls(), 0);

    uint32_t pid_before = 0;
    for (uint32_t pid = 1; pid < 16; pid++) {
        struct nx_process *p = nx_process_lookup_by_pid(pid);
        if (p) pid_before = pid;
    }

    g_wait_parent = nx_process_create("wait-parent");
    KASSERT_NOT_NULL(g_wait_parent);
    uint32_t wait_parent_pid = g_wait_parent->pid;

    g_wait_task = sched_spawn_kthread("wait-el0", wait_el0_kthread, 0,
                                      g_wait_parent);
    KASSERT_NOT_NULL(g_wait_task);

    /* Three debug_writes expected: parent pre-wait, child (before
     * exit), parent post-wait (the "wait-ok" marker).  If wait
     * fails to deliver the status correctly the third fires as
     * nothing (cmp/bne skips it), so the counter is the headline
     * assertion. */
    const int max_yields = 1024;
    int reached = 0;
    for (int i = 0; i < max_yields; i++) {
        if (nx_syscall_debug_write_calls() >= 3) { reached = 1; break; }
        nx_task_yield();
    }
    KASSERT(reached);

    /* Independent check: find THIS run's child process — the one
     * with pid > wait_parent_pid (i.e., allocated after the wait
     * parent, which was allocated last before the fork).  Avoids
     * spurious matches against stranded processes from earlier
     * ktests. */
    (void)pid_before;
    int found_child = 0;
    for (uint32_t pid = wait_parent_pid + 1; pid < 16; pid++) {
        struct nx_process *p = nx_process_lookup_by_pid(pid);
        if (!p) continue;
        KASSERT_EQ_U(p->state, NX_PROCESS_STATE_EXITED);
        KASSERT_EQ_U(p->exit_code, 42);
        found_child = 1;
        break;
    }
    KASSERT(found_child);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_wait_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_wait_parent = NULL;
    g_wait_task   = NULL;
}
