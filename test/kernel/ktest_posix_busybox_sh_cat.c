/*
 * Kernel-side coverage for slice 7.6d.N.7 — busybox `sh -c
 * "cat /banner"`.  First file-input escalation past the pipe
 * slice: ash forks once, child execve's busybox-as-cat against
 * the /banner ramfs file (seeded by initramfs as
 * "hello from initramfs\n").
 *
 * Same harness shape as ktest_posix_busybox_sh_pipe.  Discovery-
 * driven — we do NOT assert a specific child status; the actual
 * outcome lives in the live log between markers.
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

extern char __posix_busybox_sh_cat_prog_blob_start[];
extern char __posix_busybox_sh_cat_prog_blob_end[];

void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_bbsh_cat_host;
static struct nx_task    *g_bbsh_cat_task;
static uint64_t           g_bbsh_cat_entry;

static size_t bbsh_cat_blob_size(void)
{
    return (size_t)(__posix_busybox_sh_cat_prog_blob_end -
                    __posix_busybox_sh_cat_prog_blob_start);
}

static void bbsh_cat_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(g_bbsh_cat_entry, sp_el0);
}

KTEST(posix_busybox_sh_cat_parent_forks_and_execs_busybox_sh_cat)
{
    nx_syscall_reset_for_test();

    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    uint32_t parent_pid;
    g_bbsh_cat_host = nx_process_create("bbshcat");
    KASSERT_NOT_NULL(g_bbsh_cat_host);
    parent_pid = g_bbsh_cat_host->pid;

    int rc = nx_elf_load_into_process(g_bbsh_cat_host,
                                      __posix_busybox_sh_cat_prog_blob_start,
                                      bbsh_cat_blob_size(),
                                      &g_bbsh_cat_entry);
    KASSERT_EQ_U(rc, NX_OK);

    g_bbsh_cat_task = sched_spawn_kthread("bbshcat-el0",
                                          bbsh_cat_el0_kthread, 0,
                                          g_bbsh_cat_host);
    KASSERT_NOT_NULL(g_bbsh_cat_task);

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
    /* Parent emits at least 3 markers: [bbsh-cat-parent],
     * [bbsh-cat-status=NN], and one of [bbsh-cat-{ok,failed}]. */
    KASSERT(nx_syscall_debug_write_calls() >= 3);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_bbsh_cat_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_bbsh_cat_host = NULL;
    g_bbsh_cat_task = NULL;
}
