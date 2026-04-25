/*
 * Slice 5.5 — first EL0 process.
 *
 * The test spawns a dedicated kthread whose entry function:
 *
 *   1. Copies the baked-in EL0 program (bytes between
 *      __user_prog_start and __user_prog_end, living in .rodata of
 *      kernel-test.bin) into the MMU's user window (an EL0-accessible
 *      2 MiB block starting at mmu_user_window_base()).
 *   2. Points SP_EL0 at the top of the user window.
 *   3. Calls drop_to_el0(user_entry, sp_el0).
 *
 * drop_to_el0 is one-way: the kthread never returns to its C caller.
 * Once it eret's to EL0, the kthread's kernel stack is used only by
 * exception entries (timer IRQs preempting EL0, SVCs from the user
 * program).  The EL0 program issues NX_SYS_DEBUG_WRITE and then
 * parks in a wfe loop; the kernel test observes the syscall's
 * debug_write counter rising as the "yes, EL0 ran" signal.
 *
 * After the counter rises, the test dequeues the EL0 task from the
 * runqueue so it doesn't burn later tests' timeslices.  The task's
 * kstack + state buffer are leaked deliberately — proper EL0-task
 * exit + reap lands with slice 5.6 (channels) or a dedicated follow-up.
 */

#include "ktest.h"

#include "core/cpu/exception.h"
#include "core/mmu/mmu.h"
#include "core/sched/sched.h"
#include "core/sched/task.h"
#include "framework/registry.h"
#include "framework/syscall.h"
#include "interfaces/scheduler.h"

/* Emitted by test/kernel/user_prog.S into .rodata. */
extern char __user_prog_start[];
extern char __user_prog_end[];

/* Stashed pointer to the spawned EL0 task so the test can dequeue it
 * after the counter check.  Per-test static so each EL0 case is
 * independent of prior runs. */
static struct nx_task *g_el0_task;

static void copy_user_prog_to_window(void *dst)
{
    size_t len = (size_t)(__user_prog_end - __user_prog_start);
    const char *src = __user_prog_start;
    char       *dp  = dst;
    for (size_t i = 0; i < len; i++) dp[i] = src[i];
    /* Dummy dependency check — protects against the EL0 prog being
     * empty (linker-section accidentally elided).  If len == 0,
     * drop_to_el0 would jump to an invalid instruction stream.  Fail
     * loud rather than wander off. */
    if (len == 0) {
        kprintf("[el0] user_prog embedded len is 0 — giving up\n");
        for (;;) asm volatile ("wfe");
    }
}

static void el0_entry_kthread(void *arg)
{
    (void)arg;

    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();

    /* Copy the user program to the bottom of the user window. */
    copy_user_prog_to_window((void *)(uintptr_t)base);

    /* SP_EL0 at the top of the window, 16-byte aligned downward.
     * The user program doesn't actually use its stack in slice 5.5
     * (no function calls, all inline), but SPSR_EL1.M = EL0t selects
     * SP_EL0 so we must set it to a valid mapped address or the first
     * push / alignment-check on exception entry will fault. */
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);

    /* One-way jump. */
    drop_to_el0(base, sp_el0);
}

static void spawn_el0_task_once(void)
{
    if (g_el0_task) return;
    g_el0_task = sched_spawn_kthread("el0_prog", el0_entry_kthread, 0);
}

KTEST(drop_to_el0_runs_user_program_which_reaches_debug_write)
{
    nx_syscall_reset_for_test();
    uint64_t before = nx_syscall_debug_write_calls();
    KASSERT_EQ_U(before, 0);

    spawn_el0_task_once();
    KASSERT_NOT_NULL(g_el0_task);

    /* Yield from the ktest (idle) context until the debug_write
     * counter rises.  The user program does exactly one SVC, so we're
     * looking for the counter to hit 1.  Bounded loop: the default
     * quantum + wfe-pacing means a handful of yields is enough in
     * practice; 256 is a generous upper bound that still terminates
     * promptly on a broken code path. */
    const int max_yields = 256;
    int reached = 0;
    for (int i = 0; i < max_yields; i++) {
        if (nx_syscall_debug_write_calls() > before) { reached = 1; break; }
        nx_task_yield();
    }
    KASSERT(reached);
    KASSERT(nx_syscall_debug_write_calls() >= 1);

    /* Remove the EL0 task from the runqueue so subsequent tests don't
     * compete with it for timeslices.  We deliberately do NOT destroy
     * it — its kernel stack is still nominally live (an active trap
     * frame from the preempting tick lives on it).  Leak is
     * acceptable at this stage; a proper task-exit path lands in a
     * follow-up slice. */
    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_el0_task);
}

KTEST(el0_user_window_vars_are_sensible)
{
    /* Sanity check the user-window constants.  If the L2 permissions
     * were wrong the drop-to-EL0 test above would either fault into
     * halt_forever (unhandled non-SVC sync exception) or simply never
     * increment the counter.  This test just confirms the public
     * getters return reasonable values before we go trying to cp bytes
     * there. */
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    KASSERT(base >= 0x40000000UL);
    KASSERT(base <  0x80000000UL);
    KASSERT(size == (2UL * 1024 * 1024));
    KASSERT((base & (size - 1)) == 0);   /* 2 MiB-aligned */
    /* User prog bytes must fit (generously) inside the window. */
    size_t prog_len = (size_t)(__user_prog_end - __user_prog_start);
    KASSERT(prog_len > 0);
    KASSERT(prog_len < size);
}
