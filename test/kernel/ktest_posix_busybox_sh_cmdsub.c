/*
 * Kernel-side coverage for slice 7.6d.N.10b — busybox
 * `sh -c "echo $(cat /banner)"`.  Command-substitution
 * escalation: a non-exec'd parent shell process consumes the
 * pipe between fork and the inner cat's exit, then dispatches
 * the outer echo builtin against the captured bytes.
 *
 * Composition coverage that's new for this slice:
 *   - sys_read CHANNEL arm reached from a process role that
 *     has NEVER called sys_exec — slice 7.6d.N.6b's pipe test
 *     had cat (an exec'd child) as the consumer.  Validates
 *     that the fork-time CHANNEL inheritance from slice 7.6a
 *     composes with the slot-2-routing + yield-loop-on-EAGAIN
 *     work from slice 7.6d.N.6b in the parent role.
 *   - waitpid(child) ordering with pipe-EOF: the parent must
 *     close its copy of the write end, drain the read end to
 *     EOF, and only then reap; the slice 7.6d.N.6b machinery
 *     handles all three transitions.
 *
 * Same harness shape as ktest_posix_busybox_sh_cat: discovery-
 * driven, only asserts parent exits 0.  ash's actual substituted
 * output goes to the live UART log between markers.
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

extern char __posix_busybox_sh_cmdsub_prog_blob_start[];
extern char __posix_busybox_sh_cmdsub_prog_blob_end[];

void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_bbsh_cmdsub_host;
static struct nx_task    *g_bbsh_cmdsub_task;
static uint64_t           g_bbsh_cmdsub_entry;

static size_t bbsh_cmdsub_blob_size(void)
{
    return (size_t)(__posix_busybox_sh_cmdsub_prog_blob_end -
                    __posix_busybox_sh_cmdsub_prog_blob_start);
}

static void bbsh_cmdsub_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(g_bbsh_cmdsub_entry, sp_el0);
}

KTEST(posix_busybox_sh_cmdsub_parent_forks_and_execs_busybox_sh_cmdsub)
{
    nx_syscall_reset_for_test();

    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    uint32_t parent_pid;
    g_bbsh_cmdsub_host = nx_process_create("bbshcmdsub");
    KASSERT_NOT_NULL(g_bbsh_cmdsub_host);
    parent_pid = g_bbsh_cmdsub_host->pid;

    int rc = nx_elf_load_into_process(g_bbsh_cmdsub_host,
                                      __posix_busybox_sh_cmdsub_prog_blob_start,
                                      bbsh_cmdsub_blob_size(),
                                      &g_bbsh_cmdsub_entry);
    KASSERT_EQ_U(rc, NX_OK);

    g_bbsh_cmdsub_task = sched_spawn_kthread("bbshcmdsub-el0",
                                             bbsh_cmdsub_el0_kthread, 0,
                                             g_bbsh_cmdsub_host);
    KASSERT_NOT_NULL(g_bbsh_cmdsub_task);

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
    /* Parent emits at least 3 markers: [bbsh-cmdsub-parent],
     * [bbsh-cmdsub-status=NN], and one of [bbsh-cmdsub-{ok,failed}]. */
    KASSERT(nx_syscall_debug_write_calls() >= 3);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_bbsh_cmdsub_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_bbsh_cmdsub_host = NULL;
    g_bbsh_cmdsub_task = NULL;
}
