/*
 * Kernel-side coverage for slice 7.6d.N.10a — busybox
 * `sh -c "cat < /banner"`.  First STDIN-redirection-from-file
 * escalation: ash forks once, the child opens /banner with
 * O_RDONLY, dup3s the new file handle onto fd 0 (slot 2 per
 * slice 7.6d.N.6b's POSIX-STDIN-FILENO-=-0 routing), execve's
 * /bin/cat with no path argument; cat reads from stdin (the
 * FILE arm of sys_read via the slot-2 redirection) and writes
 * to stdout (CONSOLE).
 *
 * Composition coverage that's new for this slice:
 *   - dup3 with newfd=0 (the special-case branch added in
 *     slice 7.6d.N.6b) installing a HANDLE_FILE rather than a
 *     HANDLE_CHANNEL.
 *   - sys_read FILE arm reached via slot 2 (POSIX STDIN_FILENO),
 *     not slot 3+ as in slice 7.6d.N.7's `cat /banner` path.
 *   - sys_exec preserving a HANDLE_FILE handle that lives at
 *     slot 2 specifically (slice N.9 covered slot 0 / fd 1).
 *
 * Same harness shape as ktest_posix_busybox_sh_cat: discovery-
 * driven, only asserts parent exits 0.  cat's actual output goes
 * to the live UART log between markers.
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

extern char __posix_busybox_sh_stdin_prog_blob_start[];
extern char __posix_busybox_sh_stdin_prog_blob_end[];

void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_bbsh_stdin_host;
static struct nx_task    *g_bbsh_stdin_task;
static uint64_t           g_bbsh_stdin_entry;

static size_t bbsh_stdin_blob_size(void)
{
    return (size_t)(__posix_busybox_sh_stdin_prog_blob_end -
                    __posix_busybox_sh_stdin_prog_blob_start);
}

static void bbsh_stdin_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(g_bbsh_stdin_entry, sp_el0);
}

KTEST(posix_busybox_sh_stdin_parent_forks_and_execs_busybox_sh_stdin)
{
    nx_syscall_reset_for_test();

    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    uint32_t parent_pid;
    g_bbsh_stdin_host = nx_process_create("bbshstdin");
    KASSERT_NOT_NULL(g_bbsh_stdin_host);
    parent_pid = g_bbsh_stdin_host->pid;

    int rc = nx_elf_load_into_process(g_bbsh_stdin_host,
                                      __posix_busybox_sh_stdin_prog_blob_start,
                                      bbsh_stdin_blob_size(),
                                      &g_bbsh_stdin_entry);
    KASSERT_EQ_U(rc, NX_OK);

    g_bbsh_stdin_task = sched_spawn_kthread("bbshstdin-el0",
                                            bbsh_stdin_el0_kthread, 0,
                                            g_bbsh_stdin_host);
    KASSERT_NOT_NULL(g_bbsh_stdin_task);

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
    /* Parent emits at least 3 markers: [bbsh-stdin-parent],
     * [bbsh-stdin-status=NN], and one of [bbsh-stdin-{ok,failed}]. */
    KASSERT(nx_syscall_debug_write_calls() >= 3);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_bbsh_stdin_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_bbsh_stdin_host = NULL;
    g_bbsh_stdin_task = NULL;
}
