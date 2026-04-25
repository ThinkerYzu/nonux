/*
 * Kernel-side coverage for slice 7.5 — NX_SYS_SIGNAL (SIGTERM
 * delivery).
 *
 * Loads the C-compiled `posix_signal_prog.elf` (parent fork +
 * SIGTERM to child + waitpid + assert) into a fresh process,
 * drops to EL0, and verifies the live log gains three markers —
 * `[signal-parent]`, `[signal-child]`, `[signal-ok]` — plus the
 * child process ends EXITED with `exit_code == 128 +
 * NX_POSIX_SIGTERM == 143`.
 *
 * The `[signal-ok]` marker is emitted only when the parent's
 * `waitpid` reports the child exited with exactly `128 +
 * NX_POSIX_SIGTERM`.  If the signal delivery path is broken
 * (e.g., `sched_check_resched` forgets the poll, the child never
 * gets preempted, or the exit code lands as something other than
 * `128 + signo`), the parent falls through without emitting the
 * marker and the counter stops at 2, failing the assertion.
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

extern char __posix_signal_prog_blob_start[];
extern char __posix_signal_prog_blob_end[];

void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_sig_host;
static struct nx_task    *g_sig_task;
static uint64_t           g_sig_entry;

static size_t signal_prog_blob_size(void)
{
    return (size_t)(__posix_signal_prog_blob_end -
                    __posix_signal_prog_blob_start);
}

static void sig_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(g_sig_entry, sp_el0);
}

KTEST(posix_signal_sigterm_kills_forked_child_with_status_143)
{
    nx_syscall_reset_for_test();
    KASSERT_EQ_U(nx_syscall_debug_write_calls(), 0);

    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    uint32_t host_pid;
    g_sig_host = nx_process_create("signal-host");
    KASSERT_NOT_NULL(g_sig_host);
    host_pid = g_sig_host->pid;

    int rc = nx_elf_load_into_process(g_sig_host,
                                      __posix_signal_prog_blob_start,
                                      signal_prog_blob_size(),
                                      &g_sig_entry);
    KASSERT_EQ_U(rc, NX_OK);

    g_sig_task = sched_spawn_kthread("signal-el0", sig_el0_kthread, 0,
                                     g_sig_host);
    KASSERT_NOT_NULL(g_sig_task);

    /* Three markers expected: [signal-parent] (parent pre-kill),
     * [signal-child] (child pre-loop), [signal-ok] (parent after
     * wait confirms status == 143).  Each is one debug_write. */
    const int max_yields = 4096;
    int reached = 0;
    for (int i = 0; i < max_yields; i++) {
        if (nx_syscall_debug_write_calls() >= 3) { reached = 1; break; }
        nx_task_yield();
    }
    KASSERT(reached);

    /* Independent check: one of the forked children in the
     * process table must have exit_code == 128 + NX_SIGTERM.  The
     * parent exits with 31 via its normal path; the child dies
     * with 143. */
    int found = 0;
    for (uint32_t pid = host_pid + 1; pid < 16; pid++) {
        struct nx_process *p = nx_process_lookup_by_pid(pid);
        if (!p) continue;
        if (p->state != NX_PROCESS_STATE_EXITED) continue;
        if (p->exit_code != 128 + NX_SIGTERM) continue;
        found = 1;
        break;
    }
    KASSERT(found);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_sig_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_sig_host = NULL;
    g_sig_task = NULL;
}
