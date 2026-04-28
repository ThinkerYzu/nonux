/*
 * Kernel-side coverage for slice 7.6d.N.15 — busybox
 * `sh -c "exec 3< /banner; head <&3"`.  First workload that
 * exercises FILE-fd inheritance through fork.
 *
 * Closes the gap deferred since slice 7.6a: `sys_fork` previously
 * inherited only HANDLE_CHANNEL entries.  HANDLE_FILE was skipped
 * out of caution about per-cursor state, but the slice 7.6d.N.8
 * vfs `retain` op (added for `dup3` / `fcntl(F_DUPFD)`) gives us
 * exactly the right primitive: bump the per-open's refs; both
 * parent and child point at the same `struct ramfs_open` (same
 * cursor + flags), matching POSIX's "fork shares the open file
 * description" semantic.
 *
 * Asserts the parent exits 0 + at least 3 marker writes.  The full
 * banner ("hello from initramfs\n") lands in the QEMU log between
 * the [bbsh-xfile-parent] marker and the [bbsh-xfile-status=00]
 * marker.  A regression that drops FILE inheritance would see head
 * exit 0 with no banner output (head reads from CONSOLE stdin which
 * returns EOF in v1).
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

extern char __posix_busybox_sh_xfile_prog_blob_start[];
extern char __posix_busybox_sh_xfile_prog_blob_end[];

void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_bbsh_xfile_host;
static struct nx_task    *g_bbsh_xfile_task;
static uint64_t           g_bbsh_xfile_entry;

static size_t bbsh_xfile_blob_size(void)
{
    return (size_t)(__posix_busybox_sh_xfile_prog_blob_end -
                    __posix_busybox_sh_xfile_prog_blob_start);
}

static void bbsh_xfile_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(g_bbsh_xfile_entry, sp_el0);
}

KTEST(posix_busybox_sh_xfile_parent_forks_and_execs_busybox_sh_xfile)
{
    nx_syscall_reset_for_test();

    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    uint32_t parent_pid;
    g_bbsh_xfile_host = nx_process_create("bbshxfile");
    KASSERT_NOT_NULL(g_bbsh_xfile_host);
    parent_pid = g_bbsh_xfile_host->pid;

    int rc = nx_elf_load_into_process(g_bbsh_xfile_host,
                                      __posix_busybox_sh_xfile_prog_blob_start,
                                      bbsh_xfile_blob_size(),
                                      &g_bbsh_xfile_entry);
    KASSERT_EQ_U(rc, NX_OK);

    g_bbsh_xfile_task = sched_spawn_kthread("bbshxfile-el0",
                                            bbsh_xfile_el0_kthread, 0,
                                            g_bbsh_xfile_host);
    KASSERT_NOT_NULL(g_bbsh_xfile_task);

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
    ops->dequeue(self, g_bbsh_xfile_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_bbsh_xfile_host = NULL;
    g_bbsh_xfile_task = NULL;
}
