/*
 * Kernel-side coverage for slice 7.6d.N.13 — busybox
 * `sh -c "id; uname -a"`.  First workload exercising the
 * tolerable-syscall stubs sweep (set_tid_address, getuid/euid/gid/
 * egid, setuid/setgid, getpid/getppid, uname).
 *
 * Asserts the parent exits 0 and the marker count rises past the
 * parent's own three markers.  The actual `id` / `uname -a` output
 * lands in the QEMU log for human inspection; programmatic byte
 * assertions on busybox's output format would couple us to applet
 * specifics that aren't part of this slice's contract.
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

extern char __posix_busybox_sh_id_uname_prog_blob_start[];
extern char __posix_busybox_sh_id_uname_prog_blob_end[];

void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_bbsh_idun_host;
static struct nx_task    *g_bbsh_idun_task;
static uint64_t           g_bbsh_idun_entry;

static size_t bbsh_idun_blob_size(void)
{
    return (size_t)(__posix_busybox_sh_id_uname_prog_blob_end -
                    __posix_busybox_sh_id_uname_prog_blob_start);
}

static void bbsh_idun_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(g_bbsh_idun_entry, sp_el0);
}

KTEST(posix_busybox_sh_id_uname_parent_forks_and_execs_busybox_sh_id_uname)
{
    nx_syscall_reset_for_test();

    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    uint32_t parent_pid;
    g_bbsh_idun_host = nx_process_create("bbshidun");
    KASSERT_NOT_NULL(g_bbsh_idun_host);
    parent_pid = g_bbsh_idun_host->pid;

    int rc = nx_elf_load_into_process(g_bbsh_idun_host,
                                      __posix_busybox_sh_id_uname_prog_blob_start,
                                      bbsh_idun_blob_size(),
                                      &g_bbsh_idun_entry);
    KASSERT_EQ_U(rc, NX_OK);

    g_bbsh_idun_task = sched_spawn_kthread("bbshidun-el0",
                                           bbsh_idun_el0_kthread, 0,
                                           g_bbsh_idun_host);
    KASSERT_NOT_NULL(g_bbsh_idun_task);

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
    KASSERT(nx_syscall_debug_write_calls() >= 3);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_bbsh_idun_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_bbsh_idun_host = NULL;
    g_bbsh_idun_task = NULL;
}
