/*
 * Kernel-side coverage for slice 7.6d.N.0 — first attempt at exec'ing
 * busybox AS A SHELL.
 *
 * Drives `posix_busybox_sh_prog` (libnxlibc-linked) which forks +
 * execve("/bin/busybox", { "sh", "-c", "exit 42", NULL }, NULL)
 * against the initramfs-seeded busybox binary.  argv[0]="sh" routes
 * busybox's main() into the ash applet via basename(argv[0]).
 *
 * Discovery-driven, just like ktest_posix_busybox.c (slice 7.6d.2c).
 * We do NOT assert a specific child status — the actual outcome is
 * captured in the live log's `[bbsh-status=NN]` marker for the
 * session log to triage.  Three acceptable outcomes for the parent:
 *
 *  - exit 0: parent ran to completion + printed status marker.
 *    The CHILD's status (NN in `[bbsh-status=NN]`) tells us what
 *    happened to ash.
 *  - exit 1: fork failed — surprising; would mean fork-side
 *    handle-table or task-create regressed since slice 7.6d.3c.
 *  - kernel halt: would mean ash tripped a fault in a code path
 *    not covered by the slice-7.6d.3a EL0-fault-to-process-exit
 *    conversion.  Very surprising; would mean a kernel bug, not
 *    a userspace fault.
 *
 * Same yield-cap as the busybox --help test (32768) because
 * exec'ing 1.22 MB takes plenty of memcpys; ash startup before
 * even reaching `exit 42` does additional syscall work.
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

extern char __posix_busybox_sh_prog_blob_start[];
extern char __posix_busybox_sh_prog_blob_end[];

void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_bbsh_host;
static struct nx_task    *g_bbsh_task;
static uint64_t           g_bbsh_entry;

static size_t bbsh_blob_size(void)
{
    return (size_t)(__posix_busybox_sh_prog_blob_end -
                    __posix_busybox_sh_prog_blob_start);
}

static void bbsh_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(g_bbsh_entry, sp_el0);
}

KTEST(posix_busybox_sh_parent_forks_and_execs_busybox_sh)
{
    nx_syscall_reset_for_test();

    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    uint32_t parent_pid;
    g_bbsh_host = nx_process_create("bbsh");
    KASSERT_NOT_NULL(g_bbsh_host);
    parent_pid = g_bbsh_host->pid;

    int rc = nx_elf_load_into_process(g_bbsh_host,
                                      __posix_busybox_sh_prog_blob_start,
                                      bbsh_blob_size(),
                                      &g_bbsh_entry);
    KASSERT_EQ_U(rc, NX_OK);

    g_bbsh_task = sched_spawn_kthread("bbsh-el0", bbsh_el0_kthread, 0,
                                      g_bbsh_host);
    KASSERT_NOT_NULL(g_bbsh_task);

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
    /* At least the 3 parent-side markers: [bbsh-parent],
     * [bbsh-status=NN], and one of [bbsh-{ok,failed}].  busybox
     * may emit more of its own output if it gets that far. */
    KASSERT(nx_syscall_debug_write_calls() >= 3);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_bbsh_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_bbsh_host = NULL;
    g_bbsh_task = NULL;
}
