/*
 * Kernel-side coverage for slice 7.6d.2c — first attempt at exec'ing
 * busybox.
 *
 * Drives `posix_busybox_help_prog` (libnxlibc-linked) which forks +
 * execve("/bin/busybox", { "/bin/busybox", "--help", NULL }, NULL)
 * against the initramfs-seeded busybox binary at /bin/busybox.
 *
 * Discovery-driven: we don't know what's going to fail first.  This
 * test is structured to NOT assert specific success — it asserts only
 * that the parent reaches its post-wait status print, so the live log
 * captures `[busybox-parent][busybox-status=NN]` at minimum.
 * Whichever NN we see drives the 7.6d.3+ enumeration in HANDOFF.md.
 *
 * Two acceptable outcomes for the parent:
 *  - exit 0 (parent ran to completion + printed the status marker).
 *    The CHILD's status (the [busybox-status=NN] marker in the live
 *    log) tells us what happened to the exec'd binary.
 *  - exit 1 (fork failed) — would be surprising; means our fork-side
 *    handle table or task-create regressed.
 *
 * We assert exit 0 (parent reached the marker print).  The actual
 * busybox failure mode is captured in the live log marker for the
 * session-log write-up to triage.
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

extern char __posix_busybox_help_prog_blob_start[];
extern char __posix_busybox_help_prog_blob_end[];

void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_bbh_host;
static struct nx_task    *g_bbh_task;
static uint64_t           g_bbh_entry;

static size_t bbh_blob_size(void)
{
    return (size_t)(__posix_busybox_help_prog_blob_end -
                    __posix_busybox_help_prog_blob_start);
}

static void bbh_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    /* Parent uses libnxlibc's crt0 — sp at top - 16 is fine. */
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(g_bbh_entry, sp_el0);
}

KTEST(posix_busybox_help_parent_forks_and_execs_busybox)
{
    nx_syscall_reset_for_test();

    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    uint32_t parent_pid;
    g_bbh_host = nx_process_create("bbh");
    KASSERT_NOT_NULL(g_bbh_host);
    parent_pid = g_bbh_host->pid;

    int rc = nx_elf_load_into_process(g_bbh_host,
                                      __posix_busybox_help_prog_blob_start,
                                      bbh_blob_size(),
                                      &g_bbh_entry);
    KASSERT_EQ_U(rc, NX_OK);

    g_bbh_task = sched_spawn_kthread("bbh-el0", bbh_el0_kthread, 0,
                                     g_bbh_host);
    KASSERT_NOT_NULL(g_bbh_task);

    /* Parent emits [busybox-parent], [busybox-status=NN], and one of
     * [busybox-help-ok] / [busybox-help-failed].  Exit 0 in either
     * outcome — busybox itself may fail, but the parent's job is to
     * report the status, not to make busybox succeed.  Higher yield
     * cap than other tests because exec'ing 2.29 MB takes more
     * memcpys + a longer load. */
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
    /* At least the 3 parent-side markers: [busybox-parent],
     * [busybox-status=NN], and one of [busybox-help-{ok,failed}].
     * The busybox child may emit more (its own --help output) if it
     * gets that far. */
    KASSERT(nx_syscall_debug_write_calls() >= 3);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_bbh_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_bbh_host = NULL;
    g_bbh_task = NULL;
}
