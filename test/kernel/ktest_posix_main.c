/*
 * Kernel-side coverage for slice 7.6c.0 — EL0 C-runtime bootstrap.
 *
 * Validates that:
 *   1. `components/posix_shim/crt0.S`'s `_start` is at the ELF entry
 *      and gets invoked by `drop_to_el0` cleanly.
 *   2. crt0 sets up argc=1 + argv={ "nonux", NULL } and calls
 *      `main(argc, argv, envp)`.
 *   3. main's return value flows back through crt0 into
 *      `nx_posix_exit` — observable via the host process's
 *      exit_code field after the test yields.
 *
 * The demo program also exercises the static-inline libc helpers
 * (nx_strlen / nx_memcpy / nx_strcmp) added in the same slice; if
 * any of them generate calls to a missing symbol (memcpy from
 * libgcc, etc.) the link fails and `make` doesn't get this far.
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

extern char __posix_main_prog_blob_start[];
extern char __posix_main_prog_blob_end[];

void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_main_host;
static struct nx_task    *g_main_task;
static uint64_t           g_main_entry;

static size_t main_prog_blob_size(void)
{
    return (size_t)(__posix_main_prog_blob_end -
                    __posix_main_prog_blob_start);
}

static void main_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(g_main_entry, sp_el0);
}

KTEST(posix_main_entry_invokes_main_with_argv_and_returns_exit_code)
{
    nx_syscall_reset_for_test();
    KASSERT_EQ_U(nx_syscall_debug_write_calls(), 0);

    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    uint32_t host_pid;
    g_main_host = nx_process_create("main-host");
    KASSERT_NOT_NULL(g_main_host);
    host_pid = g_main_host->pid;

    int rc = nx_elf_load_into_process(g_main_host,
                                      __posix_main_prog_blob_start,
                                      main_prog_blob_size(),
                                      &g_main_entry);
    KASSERT_EQ_U(rc, NX_OK);
    KASSERT_EQ_U(g_main_entry, mmu_user_window_base());

    g_main_task = sched_spawn_kthread("main-el0", main_el0_kthread, 0);
    KASSERT_NOT_NULL(g_main_task);
    g_main_task->process = g_main_host;

    /* Single debug_write expected: `[main-ok]` from main's body. */
    const int max_yields = 2048;
    int reached = 0;
    for (int i = 0; i < max_yields; i++) {
        if (nx_syscall_debug_write_calls() >= 1) { reached = 1; break; }
        nx_task_yield();
    }
    KASSERT(reached);

    /* The host process must reach EXITED with exit_code == 47.  47
     * is `argc + 46` from main's return — verifying that crt0 read
     * argc=1 + dispatched the value back through nx_posix_exit. */
    int found = 0;
    for (int i = 0; i < 64; i++) {
        struct nx_process *p = nx_process_lookup_by_pid(host_pid);
        if (p && p->state == NX_PROCESS_STATE_EXITED) {
            KASSERT_EQ_U(p->exit_code, 47);
            found = 1;
            break;
        }
        nx_task_yield();
    }
    KASSERT(found);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_main_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_main_host = NULL;
    g_main_task = NULL;
}
