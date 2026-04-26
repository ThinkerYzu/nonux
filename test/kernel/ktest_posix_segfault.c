/*
 * Kernel-side coverage for slice 7.6d.3a — EL0 data abort →
 * SIGSEGV → exit 139.
 *
 * Drives `posix_segfault_prog` (libnxlibc-linked) which forks; the
 * child writes to PA 0 via VA 0 (device_block, AP=EL1-only — EL0
 * write faults with EC=0x24 / DFSC=permission).  `on_sync`'s
 * slice-7.6d.3a switch converts the lower-EL data abort into
 * `nx_process_exit(128 + SIGSEGV)` = 139; the parent's `wait()`
 * collects the 139 and emits `[segv-ok]`.
 *
 * Three markers expected: `[segv-parent][segv-child][segv-ok]`.
 * If the kernel's fault-conversion regresses, the child halt-
 * loops (or panics) and `[segv-ok]` never appears — the
 * `KASSERT(reached)` below catches it.
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

extern char __posix_segfault_prog_blob_start[];
extern char __posix_segfault_prog_blob_end[];

void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_segv_host;
static struct nx_task    *g_segv_task;
static uint64_t           g_segv_entry;

static size_t segv_blob_size(void)
{
    return (size_t)(__posix_segfault_prog_blob_end -
                    __posix_segfault_prog_blob_start);
}

static void segv_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(g_segv_entry, sp_el0);
}

KTEST(posix_segfault_child_data_abort_becomes_sigsegv_exit_139)
{
    nx_syscall_reset_for_test();

    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    uint32_t parent_pid;
    g_segv_host = nx_process_create("segv");
    KASSERT_NOT_NULL(g_segv_host);
    parent_pid = g_segv_host->pid;

    int rc = nx_elf_load_into_process(g_segv_host,
                                      __posix_segfault_prog_blob_start,
                                      segv_blob_size(),
                                      &g_segv_entry);
    KASSERT_EQ_U(rc, NX_OK);

    g_segv_task = sched_spawn_kthread("segv-el0", segv_el0_kthread, 0,
                                      g_segv_host);
    KASSERT_NOT_NULL(g_segv_task);

    /* Three debug_writes expected: [segv-parent], [segv-child],
     * [segv-ok].  Wait for parent to EXITED. */
    int found = 0;
    for (int i = 0; i < 4096; i++) {
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

    /* Independent check: a child process with exit_code = 128 +
     * SIGSEGV (139) should be in the table.  Parent exits 0; child
     * dies 139 via fault conversion. */
    int child_found = 0;
    for (uint32_t pid = parent_pid + 1; pid < 64; pid++) {
        struct nx_process *p = nx_process_lookup_by_pid(pid);
        if (!p) continue;
        if (p->state != NX_PROCESS_STATE_EXITED) continue;
        if (p->exit_code != 128 + 11) continue;
        child_found = 1;
        break;
    }
    KASSERT(child_found);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_segv_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_segv_host = NULL;
    g_segv_task = NULL;
}
