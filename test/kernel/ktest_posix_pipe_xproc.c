/*
 * Kernel-side coverage for the slice 7.6 prereq — fork handle-table
 * inheritance + cross-process pipe.
 *
 * Validates that:
 *   1. `sys_fork` duplicates the parent's CHANNEL handles into the
 *      child's handle table (each endpoint gets a `_retain` so its
 *      `handle_refs` reflects both processes' references).
 *   2. The per-endpoint refcount in `framework/channel.c` correctly
 *      keeps an endpoint live until ALL handle holders close — a
 *      parent-side `close` doesn't kill the read end while the
 *      child is still reading from it.
 *   3. `sys_read` / `sys_write` route through HANDLE_CHANNEL
 *      across processes (the child's handle table contains a fresh
 *      slot pointing at the same endpoint object as the parent's).
 *
 * If fork inheritance is missing entirely, the child sees an empty
 * handle table and `posix_read(fds[0], ...)` returns NX_ENOENT; the
 * EL0 program exits with sentinel code 3 and the ktest fails.  If
 * the per-endpoint refcount is wrong (the parent's close fully
 * shuts down the read side), the child's read sees NX_EBUSY and
 * exits with code 4 — also a loud failure.
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

extern char __posix_pipe_xproc_prog_blob_start[];
extern char __posix_pipe_xproc_prog_blob_end[];

void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_xpipe_host;
static struct nx_task    *g_xpipe_task;
static uint64_t           g_xpipe_entry;

static size_t xpipe_blob_size(void)
{
    return (size_t)(__posix_pipe_xproc_prog_blob_end -
                    __posix_pipe_xproc_prog_blob_start);
}

static void xpipe_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(g_xpipe_entry, sp_el0);
}

KTEST(posix_pipe_xproc_parent_writes_child_reads_via_inherited_handles)
{
    nx_syscall_reset_for_test();
    KASSERT_EQ_U(nx_syscall_debug_write_calls(), 0);

    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    uint32_t host_pid;
    g_xpipe_host = nx_process_create("xpipe-host");
    KASSERT_NOT_NULL(g_xpipe_host);
    host_pid = g_xpipe_host->pid;

    int rc = nx_elf_load_into_process(g_xpipe_host,
                                      __posix_pipe_xproc_prog_blob_start,
                                      xpipe_blob_size(),
                                      &g_xpipe_entry);
    KASSERT_EQ_U(rc, NX_OK);

    g_xpipe_task = sched_spawn_kthread("xpipe-el0", xpipe_el0_kthread, 0);
    KASSERT_NOT_NULL(g_xpipe_task);
    g_xpipe_task->process = g_xpipe_host;

    /* Three markers expected: [xpipe-parent], [xpipe-child],
     * [xpipe-ok].  Each is one debug_write — counter == 3 means the
     * full round-trip succeeded. */
    const int max_yields = 4096;
    int reached = 0;
    for (int i = 0; i < max_yields; i++) {
        if (nx_syscall_debug_write_calls() >= 3) { reached = 1; break; }
        nx_task_yield();
    }
    KASSERT(reached);

    /* Independent check: the child should have exited 41.  The
     * parent process exits 0 once it observes the child's status. */
    int found = 0;
    for (uint32_t pid = host_pid + 1; pid < 16; pid++) {
        struct nx_process *p = nx_process_lookup_by_pid(pid);
        if (!p) continue;
        if (p->state != NX_PROCESS_STATE_EXITED) continue;
        if (p->exit_code != 41) continue;
        found = 1;
        break;
    }
    KASSERT(found);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_xpipe_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_xpipe_host = NULL;
    g_xpipe_task = NULL;
}
