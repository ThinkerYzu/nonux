/*
 * Kernel-side coverage for slice 7.6d.N.11 — busybox
 * `sh -c "echo a >> /tmp/ap; echo b >> /tmp/ap"`.  First
 * append-redirect escalation past slice 7.6d.N.8.
 *
 * Exercises (in one go):
 *   - sys_openat O_WRONLY|O_CREAT|O_APPEND translates to
 *     NX_VFS_OPEN_{WRITE,CREATE,APPEND}
 *   - ramfs's seek-to-end-before-each-write semantic on an APPEND-
 *     flagged per-open: two separate opens land back-to-back at the
 *     end-of-file rather than stomping the first write at offset 0
 *
 * After the parent exits, the test re-opens /tmp/ap through vfs_simple
 * and KASSERTs the contents are exactly "a\nb\n" (4 bytes — load-
 * bearing assertion that catches a regression that drops APPEND on
 * the floor; without seek-to-end-before-write the second open would
 * truncate the first to "b\n").
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

extern char __posix_busybox_sh_append_prog_blob_start[];
extern char __posix_busybox_sh_append_prog_blob_end[];

void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_bbsh_append_host;
static struct nx_task    *g_bbsh_append_task;
static uint64_t           g_bbsh_append_entry;

static size_t bbsh_append_blob_size(void)
{
    return (size_t)(__posix_busybox_sh_append_prog_blob_end -
                    __posix_busybox_sh_append_prog_blob_start);
}

static void bbsh_append_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(g_bbsh_append_entry, sp_el0);
}

KTEST(posix_busybox_sh_append_parent_forks_and_execs_busybox_sh_append)
{
    nx_syscall_reset_for_test();

    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    uint32_t parent_pid;
    g_bbsh_append_host = nx_process_create("bbshappend");
    KASSERT_NOT_NULL(g_bbsh_append_host);
    parent_pid = g_bbsh_append_host->pid;

    int rc = nx_elf_load_into_process(g_bbsh_append_host,
                                      __posix_busybox_sh_append_prog_blob_start,
                                      bbsh_append_blob_size(),
                                      &g_bbsh_append_entry);
    KASSERT_EQ_U(rc, NX_OK);

    g_bbsh_append_task = sched_spawn_kthread("bbshappend-el0",
                                             bbsh_append_el0_kthread, 0,
                                             g_bbsh_append_host);
    KASSERT_NOT_NULL(g_bbsh_append_task);

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
    /* Parent emits at least 3 markers: [bbsh-append-parent],
     * [bbsh-append-status=NN], and one of [bbsh-append-{ok,failed}]. */
    KASSERT(nx_syscall_debug_write_calls() >= 3);

    /* Confirm /tmp/ap got "a\nb\n" written (both echoes appended).
     * Without slice 7.6d.N.11's seek-to-end-before-write, the second
     * open would create a fresh per-open with cursor=0 and stomp the
     * first echo, leaving "b\n" only — the strict 4-byte assertion
     * catches that regression. */
    {
        struct nx_slot *vs = nx_slot_lookup("vfs");
        const struct nx_vfs_ops *vops =
            (const struct nx_vfs_ops *)vs->active->descriptor->iface_ops;
        void *vself = vs->active->impl;
        void *file = NULL;
        int rc2 = vops->open(vself, "/tmp/ap", NX_VFS_OPEN_READ, &file);
        KASSERT_EQ_U(rc2, NX_OK);
        char buf[8] = { 0 };
        int64_t n = vops->read(vself, file, buf, sizeof buf - 1);
        KASSERT_EQ_U((int)n, 4);
        KASSERT_EQ_U(buf[0], 'a');
        KASSERT_EQ_U(buf[1], '\n');
        KASSERT_EQ_U(buf[2], 'b');
        KASSERT_EQ_U(buf[3], '\n');
        vops->close(vself, file);
    }

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_bbsh_append_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_bbsh_append_host = NULL;
    g_bbsh_append_task = NULL;
}
