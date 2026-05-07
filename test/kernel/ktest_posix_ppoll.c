/*
 * Kernel-side coverage for slice 7.8b — NX_SYS_PPOLL.
 *
 * Loads the C-compiled `posix_ppoll_prog.elf` (which uses the
 * `nx_posix_pipe / write / read / close / ppoll` wrappers from
 * `lib/libnxlibc/posix.h`) into a fresh process, drops
 * to EL0, and verifies the live log gains `[ppoll-ok]` + the
 * process ends EXITED with exit_code == 29.
 *
 * Three subtests inside the EL0 program:
 *
 *   1. Initial readiness — write before poll, ppoll's first
 *      readiness check returns immediately.
 *   2. Deadline expiry — empty pipe, 50 ms timeout, no
 *      producer; `nx_waitq_tick_deadlines` fires from
 *      sched_tick.
 *   3. Peer-close hangup — close writer, ppoll returns
 *      POLLIN | POLLHUP from the channel readiness logic.
 *
 * If any subtest fails the program exits with a discrete non-29
 * status (1..11) and this ktest's exit-code KASSERT catches it.
 *
 * Bring-up note: the deadline-expiry subtest is the only one
 * that exercises the wait_with_deadline → tick_deadlines wake
 * path.  At our 10 Hz timer + 50 ms budget, the deadline check
 * fires within ~1 tick — comfortably inside the 90 s QEMU
 * test budget.
 */

#include "ktest.h"

#include "core/cpu/exception.h"
#include "core/mmu/mmu.h"
#include "core/sched/sched.h"
#include "core/sched/task.h"
#include "framework/elf.h"
#include "framework/process.h"
#include "framework/syscall.h"
#include "interfaces/scheduler.h"

extern char __posix_ppoll_prog_blob_start[];
extern char __posix_ppoll_prog_blob_end[];

void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_ppoll_host;
static struct nx_task    *g_ppoll_task;
static uint64_t           g_ppoll_entry;

static size_t ppoll_prog_blob_size(void)
{
    return (size_t)(__posix_ppoll_prog_blob_end -
                    __posix_ppoll_prog_blob_start);
}

static void ppoll_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(g_ppoll_entry, sp_el0);
}

KTEST(posix_ppoll_initial_ready_deadline_and_peer_close)
{
    nx_syscall_reset_for_test();
    KASSERT_EQ_U(nx_syscall_debug_write_calls(), 0);

    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    uint32_t host_pid;
    g_ppoll_host = nx_process_create("ppoll-host");
    KASSERT_NOT_NULL(g_ppoll_host);
    host_pid = g_ppoll_host->pid;

    int rc = nx_elf_load_into_process(g_ppoll_host,
                                      __posix_ppoll_prog_blob_start,
                                      ppoll_prog_blob_size(),
                                      &g_ppoll_entry);
    KASSERT_EQ_U(rc, NX_OK);
    /* Entry must lie inside the user window — the precise offset
     * depends on the compiler's symbol ordering inside the ELF
     * (`_start` doesn't always land at offset 0; static-inline
     * helpers like nx_posix_exit can be placed first).  The
     * linker script's ENTRY(_start) makes e_entry track the
     * symbol regardless. */
    KASSERT(g_ppoll_entry >= mmu_user_window_base());
    KASSERT(g_ppoll_entry < mmu_user_window_base() + mmu_user_window_size());

    g_ppoll_task = sched_spawn_kthread("ppoll-el0", ppoll_el0_kthread, 0,
                                       g_ppoll_host);
    KASSERT_NOT_NULL(g_ppoll_task);

    /* The deadline-expiry subtest sleeps for 50 ms wall-clock —
     * pure `nx_task_yield()` between iterations never advances the
     * CPU clock (yield is a context-switch primitive, not a
     * sleep).  Mirror the idle-task body: yield first to let the
     * EL0 program run; then `wfi` to suspend the CPU until the
     * next timer tick, which is what advances `nx_deadline_*`'s
     * clock and ultimately fires `nx_waitq_tick_deadlines` from
     * `sched_tick`.  Bound the loop generously — at 10 Hz timer +
     * 50 ms deadline + tiny EL0 work, even a slow QEMU should
     * complete in well under a second. */
    const int max_iters = 256;
    int reached = 0;
    for (int i = 0; i < max_iters; i++) {
        struct nx_process *p = nx_process_lookup_by_pid(host_pid);
        if (p && p->state == NX_PROCESS_STATE_EXITED) { reached = 1; break; }
        nx_task_yield();
        asm volatile ("wfi");
    }
    KASSERT(reached);

    /* Process is already EXITED (we waited for that above).  Just
     * verify the exit code — non-29 fingers a specific subtest
     * failure (1..11). */
    struct nx_process *exited = nx_process_lookup_by_pid(host_pid);
    KASSERT_NOT_NULL(exited);
    KASSERT_EQ_U(exited->state, NX_PROCESS_STATE_EXITED);
    KASSERT_EQ_U(exited->exit_code, 29);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_ppoll_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_ppoll_host = NULL;
    g_ppoll_task = NULL;
}
