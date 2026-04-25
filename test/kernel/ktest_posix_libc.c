/*
 * Kernel-side coverage for slice 7.6c.1 — libnxlibc.a + POSIX-named
 * surface.
 *
 * Validates that:
 *   1. `libnxlibc.a` builds and links cleanly into an EL0 C
 *      program.
 *   2. POSIX-named functions (`write`, `_exit`, `strlen`, `memcpy`,
 *      `strcmp`) reach the same NX_SYS_* surface as the
 *      nx_posix_*-named static inlines.
 *   3. `write(STDOUT_FILENO, ...)`'s stdout-routing in nxlibc.c
 *      lands in NX_SYS_DEBUG_WRITE — observable via the
 *      debug_write counter and the live log marker.
 *   4. `_exit(rv)` flows through nxlibc → nx_posix_exit →
 *      NX_SYS_EXIT — observable via the host process's exit_code.
 *
 * If musl arrives in 7.6c.2 with a libc.a substituting for
 * libnxlibc.a, this exact test re-runs to catch any ABI drift —
 * the program source doesn't change.
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

extern char __posix_libc_prog_blob_start[];
extern char __posix_libc_prog_blob_end[];

void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_libc_host;
static struct nx_task    *g_libc_task;
static uint64_t           g_libc_entry;

static size_t libc_prog_blob_size(void)
{
    return (size_t)(__posix_libc_prog_blob_end -
                    __posix_libc_prog_blob_start);
}

static void libc_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(g_libc_entry, sp_el0);
}

KTEST(posix_libc_write_routes_stdout_through_debug_and_exit_53)
{
    nx_syscall_reset_for_test();
    KASSERT_EQ_U(nx_syscall_debug_write_calls(), 0);

    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    uint32_t host_pid;
    g_libc_host = nx_process_create("libc-host");
    KASSERT_NOT_NULL(g_libc_host);
    host_pid = g_libc_host->pid;

    int rc = nx_elf_load_into_process(g_libc_host,
                                      __posix_libc_prog_blob_start,
                                      libc_prog_blob_size(),
                                      &g_libc_entry);
    KASSERT_EQ_U(rc, NX_OK);

    g_libc_task = sched_spawn_kthread("libc-el0", libc_el0_kthread, 0);
    KASSERT_NOT_NULL(g_libc_task);
    g_libc_task->process = g_libc_host;

    /* Single debug_write expected: `[libc-ok]` from nxlibc_write
     * routing fd=1 through nx_posix_debug_write. */
    const int max_yields = 2048;
    int reached = 0;
    for (int i = 0; i < max_yields; i++) {
        if (nx_syscall_debug_write_calls() >= 1) { reached = 1; break; }
        nx_task_yield();
    }
    KASSERT(reached);

    /* Host process should EXIT with code 53 (the demo's
     * nxlibc_exit(53) at success). */
    int found = 0;
    for (int i = 0; i < 64; i++) {
        struct nx_process *p = nx_process_lookup_by_pid(host_pid);
        if (p && p->state == NX_PROCESS_STATE_EXITED) {
            KASSERT_EQ_U(p->exit_code, 53);
            found = 1;
            break;
        }
        nx_task_yield();
    }
    KASSERT(found);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_libc_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_libc_host = NULL;
    g_libc_task = NULL;
}
