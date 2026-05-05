/*
 * Kernel-side coverage for slice 7.6d.N.9 — busybox
 * `sh -c "cat /banner > /tmp/copy"`.  First stdout-redirection-to-
 * file escalation against an EXTERNAL command (cat).
 *
 * Closes one structural question slice 7.6d.N.8 left open: a FILE
 * handle installed at fd 1 in a forked child has to survive sys_exec
 * into a new image.  sys_exec does not touch the handle table — the
 * per-open struct is in kheap and the underlying ramfs file lives
 * in the ramfs static array, both stable across the address-space
 * swap.  This slice verifies that end-to-end.
 *
 * After the parent exits, the test re-opens /tmp/copy through
 * vfs_simple and KASSERTs the contents are exactly the banner
 * ("hello from initramfs\n", 21 bytes).
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

extern char __posix_busybox_sh_copy_prog_blob_start[];
extern char __posix_busybox_sh_copy_prog_blob_end[];

void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_bbsh_copy_host;
static struct nx_task    *g_bbsh_copy_task;
static uint64_t           g_bbsh_copy_entry;

static size_t bbsh_copy_blob_size(void)
{
    return (size_t)(__posix_busybox_sh_copy_prog_blob_end -
                    __posix_busybox_sh_copy_prog_blob_start);
}

static void bbsh_copy_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(g_bbsh_copy_entry, sp_el0);
}

KTEST(posix_busybox_sh_copy_parent_forks_and_execs_busybox_sh_copy)
{
    nx_syscall_reset_for_test();

    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    uint32_t parent_pid;
    g_bbsh_copy_host = nx_process_create("bbshcopy");
    KASSERT_NOT_NULL(g_bbsh_copy_host);
    parent_pid = g_bbsh_copy_host->pid;

    int rc = nx_elf_load_into_process(g_bbsh_copy_host,
                                      __posix_busybox_sh_copy_prog_blob_start,
                                      bbsh_copy_blob_size(),
                                      &g_bbsh_copy_entry);
    KASSERT_EQ_U(rc, NX_OK);

    g_bbsh_copy_task = sched_spawn_kthread("bbshcopy-el0",
                                           bbsh_copy_el0_kthread, 0,
                                           g_bbsh_copy_host);
    KASSERT_NOT_NULL(g_bbsh_copy_task);

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
    /* Parent emits at least 3 markers: [bbsh-copy-parent],
     * [bbsh-copy-status=NN], and one of [bbsh-copy-{ok,failed}]. */
    KASSERT(nx_syscall_debug_write_calls() >= 3);

    /* Confirm /tmp/copy got the banner written by re-opening it
     * through vfs_simple and reading back. */
    {
        struct nx_slot *vs = nx_slot_lookup("vfs");
        const struct nx_vfs_ops *vops =
            (const struct nx_vfs_ops *)vs->active->descriptor->iface_ops;
        void *vself = vs->active->impl;
        uint32_t file = vops->open(vself, "/tmp/copy", NX_VFS_OPEN_READ);
        KASSERT(file != 0);
        char buf[32] = { 0 };
        int64_t n = vops->read(vself, file, buf, sizeof buf - 1);
        /* /banner is "hello from initramfs\n" — 21 bytes. */
        KASSERT_EQ_U((int)n, 21);
        const char expected[] = "hello from initramfs\n";
        for (int i = 0; i < 21; i++)
            KASSERT_EQ_U((unsigned)buf[i], (unsigned)expected[i]);
        vops->close(vself, file);
    }

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_bbsh_copy_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_bbsh_copy_host = NULL;
    g_bbsh_copy_task = NULL;
}
