/*
 * Kernel-side coverage for slice 7.6d.N.12 — busybox
 * `sh -c "trap 'echo bye' EXIT; echo body"`.  First workload that
 * exercises the rt_sigaction / rt_sigprocmask stubs.
 *
 * Exercises:
 *   - NX_SYS_RT_SIGACTION = 27 stub returning 0 success
 *   - NX_SYS_RT_SIGPROCMASK = 28 stub returning 0 success
 *   - ash startup walks its trap table without bailing on ENOSYS
 *   - The EXIT pseudo-signal trap fires on the implicit exit at end
 *     of script — handled entirely inside ash, no kernel handler
 *     dispatch needed
 *
 * Asserts the parent exits 0.  Output bytes "body\nbye\n" go through
 * stdout → CONSOLE → debug_write and end up in the QEMU log; we
 * count them as part of nx_syscall_debug_write_calls() rising past
 * the parent's own marker count.
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

extern char __posix_busybox_sh_trap_prog_blob_start[];
extern char __posix_busybox_sh_trap_prog_blob_end[];

void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_bbsh_trap_host;
static struct nx_task    *g_bbsh_trap_task;
static uint64_t           g_bbsh_trap_entry;

static size_t bbsh_trap_blob_size(void)
{
    return (size_t)(__posix_busybox_sh_trap_prog_blob_end -
                    __posix_busybox_sh_trap_prog_blob_start);
}

static void bbsh_trap_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(g_bbsh_trap_entry, sp_el0);
}

KTEST(posix_busybox_sh_trap_parent_forks_and_execs_busybox_sh_trap)
{
    nx_syscall_reset_for_test();

    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    uint32_t parent_pid;
    g_bbsh_trap_host = nx_process_create("bbshtrap");
    KASSERT_NOT_NULL(g_bbsh_trap_host);
    parent_pid = g_bbsh_trap_host->pid;

    int rc = nx_elf_load_into_process(g_bbsh_trap_host,
                                      __posix_busybox_sh_trap_prog_blob_start,
                                      bbsh_trap_blob_size(),
                                      &g_bbsh_trap_entry);
    KASSERT_EQ_U(rc, NX_OK);

    g_bbsh_trap_task = sched_spawn_kthread("bbshtrap-el0",
                                           bbsh_trap_el0_kthread, 0,
                                           g_bbsh_trap_host);
    KASSERT_NOT_NULL(g_bbsh_trap_task);

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
    /* Parent emits at least 3 markers: [bbsh-trap-parent],
     * [bbsh-trap-status=NN], [bbsh-trap-{ok,failed}].  ash + the
     * "body\n" + "bye\n" outputs add to the count but we don't
     * require an exact number — the [bbsh-trap-ok] marker is the
     * load-bearing assertion. */
    KASSERT(nx_syscall_debug_write_calls() >= 3);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_bbsh_trap_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_bbsh_trap_host = NULL;
    g_bbsh_trap_task = NULL;
}
