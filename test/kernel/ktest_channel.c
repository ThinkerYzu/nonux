/*
 * Slice 5.6 — channels end-to-end from EL0.
 *
 * The test spawns a kthread that:
 *   1. Copies the baked-in channel-using EL0 program (bytes between
 *      __user_chan_prog_start and __user_chan_prog_end) into the
 *      MMU's user window.
 *   2. Drops to EL0 at the start of the program.
 *
 * The EL0 program creates a channel via SVC, sends a canonical
 * message across it, receives the message back on the peer endpoint,
 * and forwards the received bytes to UART via NX_SYS_DEBUG_WRITE.
 * The kernel test observes:
 *   - debug_write counter rises (EL0 reached the final SVC — meaning
 *     create + send + recv all succeeded or the program would have
 *     faulted earlier).
 *   - The live ktest log shows "[el0-chan-ok]" from UART.
 *
 * Independent of the slice-5.5 test: resets the syscall counter,
 * spawns its own kthread, uses its own user program.  The slice-5.5
 * EL0 task (still in memory but dequeued) is orphaned and doesn't
 * compete for the user window.
 */

#include "ktest.h"

#include "core/cpu/exception.h"
#include "core/mmu/mmu.h"
#include "core/sched/sched.h"
#include "core/sched/task.h"
#include "framework/registry.h"
#include "framework/syscall.h"
#include "interfaces/scheduler.h"

/* Emitted by test/kernel/user_prog_chan.S into .rodata. */
extern char __user_chan_prog_start[];
extern char __user_chan_prog_end[];

static struct nx_task *g_chan_el0_task;

static void chan_copy_prog_to_window(void *dst)
{
    size_t len = (size_t)(__user_chan_prog_end - __user_chan_prog_start);
    const char *src = __user_chan_prog_start;
    char       *dp  = dst;
    for (size_t i = 0; i < len; i++) dp[i] = src[i];
    if (len == 0) {
        kprintf("[el0-chan] embedded program is empty — giving up\n");
        for (;;) asm volatile ("wfe");
    }
}

static void chan_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    chan_copy_prog_to_window((void *)(uintptr_t)base);
    uint64_t sp_el0 = (base + size) & ~((uint64_t)0xfu);
    drop_to_el0(base, sp_el0);
}

KTEST(el0_channel_create_send_recv_round_trip)
{
    nx_syscall_reset_for_test();
    KASSERT_EQ_U(nx_syscall_debug_write_calls(), 0);

    g_chan_el0_task = sched_spawn_kthread("el0_chan", chan_el0_kthread, 0);
    KASSERT_NOT_NULL(g_chan_el0_task);

    /* Yield until EL0 reaches the final debug_write.  The program
     * does three SVCs in a row (create / send / recv) with no yield
     * between, so by the time debug_write fires every earlier SVC
     * must have succeeded — the counter rise is the end-to-end
     * success signal. */
    const int max_yields = 256;
    int reached = 0;
    for (int i = 0; i < max_yields; i++) {
        if (nx_syscall_debug_write_calls() > 0) { reached = 1; break; }
        nx_task_yield();
    }
    KASSERT(reached);

    /* Dequeue so the stranded EL0 task doesn't burn later timeslices. */
    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_chan_el0_task);
}
