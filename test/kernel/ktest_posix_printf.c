/*
 * Kernel-side coverage for slice 7.6c.2 — libnxlibc's printf
 * subset.
 *
 * The demo program calls nxlibc_printf with every supported
 * conversion.  This ktest can only verify the live log via the
 * debug_write counter (we don't capture UART output programmatically
 * — slice 5.5 explicitly punted that), so the ktest's positive
 * signal is "the demo emitted N debug_writes and then exited with
 * the expected code".
 *
 *   - 9 nxlibc_printf calls in the demo's body, each producing
 *     exactly one debug_write (printf rolls its bytes into one
 *     write).
 *   - 1 nxlibc_puts call, which produces 2 writes (string + newline).
 *
 * Total: 11 debug_writes.  Plus the host process exits 37.  If any
 * formatter is broken (e.g. %d emits nothing, or printf segfaults),
 * the count drops or the exit code changes.
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

extern char __posix_printf_prog_blob_start[];
extern char __posix_printf_prog_blob_end[];

void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_pf_host;
static struct nx_task    *g_pf_task;
static uint64_t           g_pf_entry;

static size_t pf_blob_size(void)
{
    return (size_t)(__posix_printf_prog_blob_end -
                    __posix_printf_prog_blob_start);
}

static void pf_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(g_pf_entry, sp_el0);
}

KTEST(posix_printf_emits_every_conversion_and_exits_37)
{
    nx_syscall_reset_for_test();
    KASSERT_EQ_U(nx_syscall_debug_write_calls(), 0);

    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    uint32_t host_pid;
    g_pf_host = nx_process_create("printf-host");
    KASSERT_NOT_NULL(g_pf_host);
    host_pid = g_pf_host->pid;

    int rc = nx_elf_load_into_process(g_pf_host,
                                      __posix_printf_prog_blob_start,
                                      pf_blob_size(),
                                      &g_pf_entry);
    KASSERT_EQ_U(rc, NX_OK);

    g_pf_task = sched_spawn_kthread("printf-el0", pf_el0_kthread, 0);
    KASSERT_NOT_NULL(g_pf_task);
    g_pf_task->process = g_pf_host;

    /* 9 printf + 1 atoi-printf + 1 puts (=2 writes — body + newline)
     * = 12 debug_writes total.  Use ≥ 10 as the lower bound to be
     * tolerant of count off-by-one if a future formatter coalesces
     * its output. */
    const int max_yields = 4096;
    int reached = 0;
    for (int i = 0; i < max_yields; i++) {
        if (nx_syscall_debug_write_calls() >= 10) { reached = 1; break; }
        nx_task_yield();
    }
    KASSERT(reached);

    /* Host process exits 37 (the demo's nxlibc_exit at success). */
    int found = 0;
    for (int i = 0; i < 64; i++) {
        struct nx_process *p = nx_process_lookup_by_pid(host_pid);
        if (p && p->state == NX_PROCESS_STATE_EXITED) {
            KASSERT_EQ_U(p->exit_code, 37);
            found = 1;
            break;
        }
        nx_task_yield();
    }
    KASSERT(found);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_pf_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_pf_host = NULL;
    g_pf_task = NULL;
}
