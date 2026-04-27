/*
 * Kernel-side coverage for slice 7.6d.N.4 — busybox `sh -c
 * "ls /"`.  First non-builtin escalation: ash forks + execve's
 * `/bin/ls` (duplicate of `/bin/busybox`; busybox's main
 * dispatches to the `ls` applet via `basename(argv[0])`).
 *
 * Same harness shape as the prior busybox-sh tests.  Discovery-
 * driven — we do NOT assert a specific child status; the actual
 * outcome lives in the live log between markers.
 *
 * Same yield-cap as the prior busybox tests (32768 ticks).  ash
 * starts up + parses + forks + waits; the forked busybox-as-ls
 * starts up (1.22 MB exec memcpy + musl init) + walks the root
 * directory + writes; back to ash for shutdown.
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

extern char __posix_busybox_sh_ls_prog_blob_start[];
extern char __posix_busybox_sh_ls_prog_blob_end[];

void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_bbsh_ls_host;
static struct nx_task    *g_bbsh_ls_task;
static uint64_t           g_bbsh_ls_entry;

static size_t bbsh_ls_blob_size(void)
{
    return (size_t)(__posix_busybox_sh_ls_prog_blob_end -
                    __posix_busybox_sh_ls_prog_blob_start);
}

static void bbsh_ls_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(g_bbsh_ls_entry, sp_el0);
}

KTEST(posix_busybox_sh_ls_parent_forks_and_execs_busybox_sh_ls)
{
    nx_syscall_reset_for_test();

    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    uint32_t parent_pid;
    g_bbsh_ls_host = nx_process_create("bbshls");
    KASSERT_NOT_NULL(g_bbsh_ls_host);
    parent_pid = g_bbsh_ls_host->pid;

    int rc = nx_elf_load_into_process(g_bbsh_ls_host,
                                      __posix_busybox_sh_ls_prog_blob_start,
                                      bbsh_ls_blob_size(),
                                      &g_bbsh_ls_entry);
    KASSERT_EQ_U(rc, NX_OK);

    g_bbsh_ls_task = sched_spawn_kthread("bbshls-el0",
                                         bbsh_ls_el0_kthread, 0,
                                         g_bbsh_ls_host);
    KASSERT_NOT_NULL(g_bbsh_ls_task);

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
    /* Parent emits at least 3 markers: [bbsh-ls-parent],
     * [bbsh-ls-status=NN], and one of [bbsh-ls-{ok,failed}]. */
    KASSERT(nx_syscall_debug_write_calls() >= 3);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_bbsh_ls_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_bbsh_ls_host = NULL;
    g_bbsh_ls_task = NULL;
}
