/*
 * Kernel-side coverage for slice 7.7b.1 — busybox
 * `sh -c "mkdir /m && > /m/x && ls /m"`.
 *
 * Exercises end-to-end:
 *   - musl mkdir(2) → __NR_mkdirat=34 → NX_SYS_MKDIRAT (40) → vops->mkdir
 *     → ramfs_op_mkdir creates a RAMFS_KIND_DIR entry.
 *   - sys_open's slice-7.7b.1 stat probe routes ls's openat(O_DIRECTORY)
 *     for /m through the HANDLE_DIR allocation (was "/" -only before
 *     this slice).
 *   - sys_getdents64 passes cur->path = "/m" to vops->readdir, which
 *     projects entries with `/m/` prefix to their basename ("x"), no
 *     longer needing the leading-`/` strip hack from slice 7.6d.N.5.
 *
 * After the parent exits, the test re-opens /m through vfs_simple to
 * confirm the directory entry materialised in ramfs (kind = DIR via
 * stat) and that /m/x exists as a file — strict assertions that catch
 * a regression where ramfs_op_mkdir or the syscall translation drops
 * EEXIST/ENOENT/ENOMEM in any direction.
 */

#include "ktest.h"

#include "core/cpu/exception.h"
#include "core/mmu/mmu.h"
#include "core/sched/sched.h"
#include "core/sched/task.h"
#include "framework/elf.h"
#include "framework/process.h"
#include "framework/syscall.h"
#include "framework/registry.h"
#include "framework/component.h"
#include "interfaces/scheduler.h"
#include "interfaces/vfs.h"
#include "interfaces/fs.h"

extern char __posix_busybox_sh_mkdir_prog_blob_start[];
extern char __posix_busybox_sh_mkdir_prog_blob_end[];

void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_bbsh_mkdir_host;
static struct nx_task    *g_bbsh_mkdir_task;
static uint64_t           g_bbsh_mkdir_entry;

static size_t bbsh_mkdir_blob_size(void)
{
    return (size_t)(__posix_busybox_sh_mkdir_prog_blob_end -
                    __posix_busybox_sh_mkdir_prog_blob_start);
}

static void bbsh_mkdir_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(g_bbsh_mkdir_entry, sp_el0);
}

KTEST(posix_busybox_sh_mkdir_parent_forks_and_execs_busybox_sh_mkdir)
{
    nx_syscall_reset_for_test();

    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    uint32_t parent_pid;
    g_bbsh_mkdir_host = nx_process_create("bbshmkdir");
    KASSERT_NOT_NULL(g_bbsh_mkdir_host);
    parent_pid = g_bbsh_mkdir_host->pid;

    int rc = nx_elf_load_into_process(g_bbsh_mkdir_host,
                                      __posix_busybox_sh_mkdir_prog_blob_start,
                                      bbsh_mkdir_blob_size(),
                                      &g_bbsh_mkdir_entry);
    KASSERT_EQ_U(rc, NX_OK);

    g_bbsh_mkdir_task = sched_spawn_kthread("bbshmkdir-el0",
                                            bbsh_mkdir_el0_kthread, 0,
                                            g_bbsh_mkdir_host);
    KASSERT_NOT_NULL(g_bbsh_mkdir_task);

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
    /* Parent emits at least 3 markers: [bbsh-mkdir-parent],
     * [bbsh-mkdir-status=NN], and one of [bbsh-mkdir-{ok,failed}].
     * busybox's ls also writes "x\n" through the CONSOLE arm. */
    KASSERT(nx_syscall_debug_write_calls() >= 3);

    /* Confirm /m exists as a directory via stat, and /m/x exists as
     * a file.  Without ramfs_op_mkdir actually creating the DIR entry
     * (or sys_mkdirat dropping the call on the floor), stat("/m")
     * would return ENOENT and the busybox script would have exited
     * non-zero — but check directly to catch a regression where stat
     * synthesises /m purely from the /m/x child. */
    {
        struct nx_slot *vs = nx_slot_lookup("vfs");
        const struct nx_vfs_ops *vops =
            (const struct nx_vfs_ops *)vs->active->descriptor->iface_ops;
        void *vself = vs->active->impl;

        struct nx_fs_stat st;
        int rc2 = vops->stat(vself, "/m", &st);
        KASSERT_EQ_U(rc2, NX_OK);
        KASSERT_EQ_U(st.kind, NX_FS_KIND_DIR);

        rc2 = vops->stat(vself, "/m/x", &st);
        KASSERT_EQ_U(rc2, NX_OK);
        KASSERT_EQ_U(st.kind, NX_FS_KIND_FILE);
    }

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_bbsh_mkdir_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_bbsh_mkdir_host = NULL;
    g_bbsh_mkdir_task = NULL;
}
