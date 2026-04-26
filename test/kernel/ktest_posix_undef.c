/*
 * Kernel-side coverage for slice 7.6d.3a — EL0 undefined-instruction
 * → SIGILL → exit 132.
 *
 * Drives `posix_undef_prog` (libnxlibc-linked) which forks; the child
 * executes `.word 0` (encoding `0x00000000`, the canonical UDF #0).
 * The CPU traps with EC=0x00 (Unknown reason); `on_sync` reads
 * `tf->pstate` to confirm the saved EL was EL0t and converts to
 * `nx_process_exit(128 + SIGILL)` = 132.  Parent's `wait()` collects
 * the 132 and emits `[undef-ok]`.
 *
 * Three markers expected: `[undef-parent][undef-child][undef-ok]`.
 * If the kernel mis-attributes EC=0x00 from EL0 as a kernel undef,
 * it'd panic instead of converting; the `KASSERT(reached)` catches
 * the regression.
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

extern char __posix_undef_prog_blob_start[];
extern char __posix_undef_prog_blob_end[];

void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_undef_host;
static struct nx_task    *g_undef_task;
static uint64_t           g_undef_entry;

static size_t undef_blob_size(void)
{
    return (size_t)(__posix_undef_prog_blob_end -
                    __posix_undef_prog_blob_start);
}

static void undef_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(g_undef_entry, sp_el0);
}

KTEST(posix_undef_child_executes_udf_becomes_sigill_exit_132)
{
    nx_syscall_reset_for_test();

    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    uint32_t parent_pid;
    g_undef_host = nx_process_create("undef");
    KASSERT_NOT_NULL(g_undef_host);
    parent_pid = g_undef_host->pid;

    int rc = nx_elf_load_into_process(g_undef_host,
                                      __posix_undef_prog_blob_start,
                                      undef_blob_size(),
                                      &g_undef_entry);
    KASSERT_EQ_U(rc, NX_OK);

    g_undef_task = sched_spawn_kthread("undef-el0", undef_el0_kthread, 0,
                                       g_undef_host);
    KASSERT_NOT_NULL(g_undef_task);

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

    /* Independent check: child exit_code = 128 + SIGILL (132). */
    int child_found = 0;
    for (uint32_t pid = parent_pid + 1; pid < 64; pid++) {
        struct nx_process *p = nx_process_lookup_by_pid(pid);
        if (!p) continue;
        if (p->state != NX_PROCESS_STATE_EXITED) continue;
        if (p->exit_code != 128 + 4) continue;
        child_found = 1;
        break;
    }
    KASSERT(child_found);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_undef_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_undef_host = NULL;
    g_undef_task = NULL;
}
