/*
 * Kernel-side coverage for slice 7.6d.N.14 — busybox
 * `sh -c "echo hello | tr a-z A-Z | wc -c"`.  First 3-stage pipe.
 *
 * Surfaced — and this slice closes — a real kernel composition gap:
 * slice 7.6a's CHANNEL fork-inheritance was NOT slot-position-
 * preserving (it densely re-allocated CHANNELs into the child's
 * first INVALID slots).  That worked for 2-stage pipes but broke
 * 3+-stage pipes because ash closes its own pipe ends right after
 * each fork, leaving gaps in the parent's table.  The dense re-
 * allocation in subsequent forks assigned CHANNELs to *different*
 * child slot indices than the parent's pre-fork values, so the
 * child's user code dup3'd the wrong slot.  Fix: write CHANNELs
 * at the parent's slot index, overwriting any pre-installed CONSOLE
 * at that position.
 *
 * Asserts the parent exits 0 + at least 3 marker writes.  The "6\n"
 * output (HELLO\n is 6 bytes — wc -c counts including trailing
 * newline) lands in the QEMU log for human inspection.  A regression
 * that drops slot-position preservation would print "0\n" instead
 * (wc reads 0 bytes because the second pipe carries no data).
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

extern char __posix_busybox_sh_pipe3_prog_blob_start[];
extern char __posix_busybox_sh_pipe3_prog_blob_end[];

void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_bbsh_pipe3_host;
static struct nx_task    *g_bbsh_pipe3_task;
static uint64_t           g_bbsh_pipe3_entry;

static size_t bbsh_pipe3_blob_size(void)
{
    return (size_t)(__posix_busybox_sh_pipe3_prog_blob_end -
                    __posix_busybox_sh_pipe3_prog_blob_start);
}

static void bbsh_pipe3_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(g_bbsh_pipe3_entry, sp_el0);
}

KTEST(posix_busybox_sh_pipe3_parent_forks_and_execs_busybox_sh_pipe3)
{
    nx_syscall_reset_for_test();

    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    uint32_t parent_pid;
    g_bbsh_pipe3_host = nx_process_create("bbshpipe3");
    KASSERT_NOT_NULL(g_bbsh_pipe3_host);
    parent_pid = g_bbsh_pipe3_host->pid;

    int rc = nx_elf_load_into_process(g_bbsh_pipe3_host,
                                      __posix_busybox_sh_pipe3_prog_blob_start,
                                      bbsh_pipe3_blob_size(),
                                      &g_bbsh_pipe3_entry);
    KASSERT_EQ_U(rc, NX_OK);

    g_bbsh_pipe3_task = sched_spawn_kthread("bbshpipe3-el0",
                                            bbsh_pipe3_el0_kthread, 0,
                                            g_bbsh_pipe3_host);
    KASSERT_NOT_NULL(g_bbsh_pipe3_task);

    int found = 0;
    for (int i = 0; i < 32768; i++) {
        struct nx_process *p = nx_process_lookup_by_pid(parent_pid);
        if (p && p->state == NX_PROCESS_STATE_EXITED) {
            KASSERT_EQ_U(p->exit_code, 0);
            found = 1;
            break;
        }
        nx_task_yield();
    }
    KASSERT(found);
    KASSERT(nx_syscall_debug_write_calls() >= 3);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_bbsh_pipe3_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_bbsh_pipe3_host = NULL;
    g_bbsh_pipe3_task = NULL;
}
