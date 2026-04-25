/*
 * Kernel-side coverage for slice 7.4d — posix_shim.
 *
 * `test/kernel/posix_prog_blob.S` embeds the standalone
 * `posix_prog.elf` (C-compiled EL0 binary that uses the
 * header-only posix_shim wrappers) into kernel-test.bin's
 * .rodata, flanked by `__posix_prog_blob_{start,end}` labels.
 * This test proves the C-level POSIX API produces the right
 * SVCs end-to-end.
 *
 * Flow:
 *   1. Purge stranded EL0-wfe tasks from prior slice-7.4
 *      ktests (fork / wait / exec) — each would otherwise burn
 *      a full sched_rr quantum before rotating past, pushing
 *      this test past the 15-second QEMU budget.  Same workaround
 *      as `ktest_exec`.
 *   2. Create a fresh user process; use the slice-7.3 ELF
 *      loader to copy posix_prog.elf's PT_LOADs into its
 *      address space.
 *   3. Spawn a kthread pinned to that process; drop to EL0 at
 *      the ELF entry.
 *   4. Yield until the debug_write counter reaches 3 (three
 *      markers: [posix-parent], [posix-child], [posix-ok]).
 *   5. Verify a process with exit_code == 23 exists in the
 *      table (the forked child, which calls nx_posix_exit(23)).
 *   6. Dequeue the host task.
 *
 * If the C-level wrappers in `components/posix_shim/posix.h`
 * emit the wrong SVC numbers or mangle the ABI, either the
 * markers won't appear or the exit code won't match — both
 * make the test fail loudly.
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

extern char __posix_prog_blob_start[];
extern char __posix_prog_blob_end[];

/* Test-only sched_rr helper — same one ktest_exec uses. */
void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_posix_host;
static struct nx_task    *g_posix_task;
static uint64_t           g_posix_entry;

static size_t posix_prog_blob_size(void)
{
    return (size_t)(__posix_prog_blob_end - __posix_prog_blob_start);
}

static void posix_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(g_posix_entry, sp_el0);
}

KTEST(posix_shim_fork_child_exit_23_parent_waits_and_emits_ok)
{
    /* Like ktest_exec and ktest_wait: skip nx_process_reset_for_test
     * because earlier ktests strand tasks in wfe and wiping the
     * process table would dangle their task->process pointers. */
    nx_syscall_reset_for_test();
    KASSERT_EQ_U(nx_syscall_debug_write_calls(), 0);

    /* Drain stranded user-process tasks so they don't starve our
     * kthread.  Same rationale as ktest_exec. */
    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    uint32_t host_pid;
    g_posix_host = nx_process_create("posix-host");
    KASSERT_NOT_NULL(g_posix_host);
    host_pid = g_posix_host->pid;

    /* Load the C-compiled EL0 ELF into the host process's user-
     * window backing.  Same flow as ktest_elf. */
    int rc = nx_elf_load_into_process(g_posix_host,
                                      __posix_prog_blob_start,
                                      posix_prog_blob_size(),
                                      &g_posix_entry);
    KASSERT_EQ_U(rc, NX_OK);
    KASSERT_EQ_U(g_posix_entry, mmu_user_window_base());

    g_posix_task = sched_spawn_kthread("posix-el0", posix_el0_kthread, 0);
    KASSERT_NOT_NULL(g_posix_task);
    g_posix_task->process = g_posix_host;

    /* Three markers expected: [posix-parent], [posix-child],
     * [posix-ok].  Each is a single NX_SYS_DEBUG_WRITE. */
    const int max_yields = 2048;
    int reached = 0;
    for (int i = 0; i < max_yields; i++) {
        if (nx_syscall_debug_write_calls() >= 3) { reached = 1; break; }
        nx_task_yield();
    }
    KASSERT(reached);

    /* Independent check: the forked child should have exit_code ==
     * 23.  Search pids > host_pid so we don't match stranded
     * processes from earlier ktests. */
    int found_child = 0;
    for (uint32_t pid = host_pid + 1; pid < 16; pid++) {
        struct nx_process *p = nx_process_lookup_by_pid(pid);
        if (!p) continue;
        if (p->state != NX_PROCESS_STATE_EXITED) continue;
        if (p->exit_code != 23) continue;
        found_child = 1;
        break;
    }
    KASSERT(found_child);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_posix_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_posix_host = NULL;
    g_posix_task = NULL;
}
