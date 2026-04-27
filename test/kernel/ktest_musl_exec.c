/*
 * Kernel-side coverage for slice 7.6c.3c — sys_exec → musl-linked
 * child round-trip, validates AUXV consumption end-to-end.
 *
 * Drives `musl_exec_parent_prog` (libnxlibc-linked) which forks +
 * execve's `/musl_prog` (musl-linked, the same posix_musl_prog ELF
 * embedded as `/musl_prog` in the initramfs).  sys_exec walks the
 * AUXV-on-stack layout slice 7.6c.3b builds; musl's __init_libc
 * reads AT_RANDOM + AT_PAGESZ.
 *
 * Success condition: the child runs all the way through
 * __libc_start_main + main + write(1, "[musl-ok]") + _exit(57); the
 * parent waits, observes status == 57, emits `[musl-exec-ok]`, and
 * exits.  If anything in the AUXV path is broken, the child either
 * faults during init or returns a non-57 exit code; either way the
 * parent's `[musl-exec-ok]` marker doesn't appear and the
 * exit_code assertion below fails.
 *
 * Three markers expected in the live log:
 *   [musl-exec-parent]  (from this parent, before the wait)
 *   [musl-ok]           (from the musl-linked child's main)
 *   [musl-exec-ok]      (from the parent on observing status == 57)
 */

#include "ktest.h"

#include "core/cpu/exception.h"
#include "core/mmu/mmu.h"
#include "core/sched/sched.h"
#include "core/sched/task.h"
#include "framework/console.h"
#include "framework/elf.h"
#include "framework/process.h"
#include "framework/syscall.h"
#include "interfaces/scheduler.h"

extern char __musl_exec_parent_prog_blob_start[];
extern char __musl_exec_parent_prog_blob_end[];

void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_mep_host;
static struct nx_task    *g_mep_task;
static uint64_t           g_mep_entry;

static size_t mep_blob_size(void)
{
    return (size_t)(__musl_exec_parent_prog_blob_end -
                    __musl_exec_parent_prog_blob_start);
}

static void mep_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    /* Parent uses libnxlibc's crt0 (synthesized argv) — sp at
     * top - 16 is fine, libnxlibc's crt0 doesn't walk envp/auxv. */
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(g_mep_entry, sp_el0);
}

KTEST(musl_exec_parent_forks_and_execs_musl_child_returns_57)
{
    nx_syscall_reset_for_test();

    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    uint32_t parent_pid;
    g_mep_host = nx_process_create("musl-exec");
    KASSERT_NOT_NULL(g_mep_host);
    parent_pid = g_mep_host->pid;

    int rc = nx_elf_load_into_process(g_mep_host,
                                      __musl_exec_parent_prog_blob_start,
                                      mep_blob_size(),
                                      &g_mep_entry);
    KASSERT_EQ_U(rc, NX_OK);

    g_mep_task = sched_spawn_kthread("musl-exec-el0", mep_el0_kthread, 0,
                                     g_mep_host);
    KASSERT_NOT_NULL(g_mep_task);

    /* The parent emits two markers + waits for the child;
     * child emits one marker (`[musl-ok]`) + exits 57.  Total:
     * 3 debug_writes on the success path.  Wait for the parent to
     * EXITED. */
    int found = 0;
    for (int i = 0; i < 8192; i++) {
        struct nx_process *p = nx_process_lookup_by_pid(parent_pid);
        if (p && p->state == NX_PROCESS_STATE_EXITED) {
            KASSERT_EQ_U(p->exit_code, 0);
            found = 1;
            break;
        }
        nx_task_yield();
    }
    KASSERT(found);
    /* Three markers: parent's [musl-exec-parent] + [musl-exec-ok] both
     * go through libnxlibc's nxlibc_write fd=1 shortcut → NX_SYS_DEBUG_WRITE
     * (debug_write_calls path); child's [musl-ok] goes through musl's
     * write(1) → NX_SYS_WRITE → CONSOLE handle → nx_console_write
     * (console_write_calls path, slice 7.6d.N.6b).  Combined ≥ 3. */
    KASSERT(nx_syscall_debug_write_calls() >= 2);
    KASSERT(nx_console_write_calls() >= 1);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_mep_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_mep_host = NULL;
    g_mep_task = NULL;
}
