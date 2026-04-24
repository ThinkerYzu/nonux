/*
 * Kernel-side coverage for slice 7.4c — exec.
 *
 * Setup:
 *   1. Purges any user-process tasks stranded by earlier slice 7.4
 *      ktests (fork, wait) from the scheduler runqueue so they
 *      don't each burn a full SCHED_RR quantum before this test's
 *      fresh kthread gets picked.
 *   2. Seeds ramfs with `/init` by writing the init_prog.elf blob
 *      (embedded via `.incbin` in init_prog_blob.S) through
 *      vfs_simple's ops.  vfs_simple → ramfs at runtime.
 *   3. Creates an EL0 process and drops into user_prog_exec.S,
 *      which forks; the child calls `NX_SYS_EXEC("/init")`, the
 *      parent waits.
 *
 * Observable invariants:
 *   - debug_write counter reaches ≥ 3: `[exec-parent]` from the
 *     parent pre-wait, `[el0-elf-ok]` from the exec'd init_prog,
 *     and `[exec-ok]` from the parent post-wait (because wait
 *     delivered init_prog's exit_code == 17).
 *   - Live ktest log contains all three markers in that order.
 *   - An EXITED process with exit_code == 17 exists after the run
 *     (the exec'd child — init_prog calls NX_SYS_EXIT(17) at its
 *     tail).
 */

#include "ktest.h"

#include "core/cpu/exception.h"
#include "core/mmu/mmu.h"
#include "core/sched/sched.h"
#include "core/sched/task.h"
#include "framework/component.h"
#include "framework/process.h"
#include "framework/registry.h"
#include "framework/syscall.h"
#include "interfaces/scheduler.h"
#include "interfaces/vfs.h"

extern char __user_exec_prog_start[];
extern char __user_exec_prog_end[];
extern char __init_prog_blob_start[];
extern char __init_prog_blob_end[];

/* sched_rr test-only helper — drains stranded user-process tasks
 * from the runqueue.  Declared here (not in sched.h) because it's a
 * policy-specific escape hatch, not part of the public scheduler
 * interface. */
void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_exec_parent;
static struct nx_task    *g_exec_task;

/*
 * Seed ramfs with `/init`.  Uses vfs_simple's ops directly (no
 * syscalls — those need a live process context).  The `vfs` slot
 * is bound at bootstrap; look it up and call its ops.
 */
static void seed_init_file(void)
{
    struct nx_slot *vs = nx_slot_lookup("vfs");
    if (!vs || !vs->active || !vs->active->descriptor) return;
    const struct nx_vfs_ops *vops =
        (const struct nx_vfs_ops *)vs->active->descriptor->iface_ops;
    void *vself = vs->active->impl;
    if (!vops) return;

    void *file = NULL;
    int rc = vops->open(vself, "/init",
                        NX_VFS_OPEN_READ | NX_VFS_OPEN_WRITE |
                        NX_VFS_OPEN_CREATE, &file);
    if (rc != NX_OK) return;

    size_t total = (size_t)(__init_prog_blob_end - __init_prog_blob_start);
    const uint8_t *src = (const uint8_t *)__init_prog_blob_start;
    size_t written = 0;
    while (written < total) {
        size_t chunk = total - written;
        if (chunk > 256) chunk = 256;
        int64_t n = vops->write(vself, file, src + written, chunk);
        if (n <= 0) break;
        written += (size_t)n;
    }
    vops->close(vself, file);
}

static void exec_copy_prog_to_window(void *dst)
{
    size_t len = (size_t)(__user_exec_prog_end - __user_exec_prog_start);
    const char *src = __user_exec_prog_start;
    char       *dp  = dst;
    for (size_t i = 0; i < len; i++) dp[i] = src[i];
}

static void exec_el0_kthread(void *arg)
{
    (void)arg;
    void *backing = mmu_address_space_user_backing(g_exec_parent->ttbr0_root);
    exec_copy_prog_to_window(backing);
    asm volatile ("dsb ish"  ::: "memory");
    asm volatile ("ic iallu" ::: "memory");
    asm volatile ("dsb ish"  ::: "memory");
    asm volatile ("isb");

    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t sp_el0 = (base + size) & ~((uint64_t)0xfu);
    drop_to_el0(base, sp_el0);
}

KTEST(exec_fork_child_execs_init_parent_waits_for_exit_17)
{
    /* Skip process reset — earlier ktests (fork, wait) strand
     * tasks in wfe; wiping the process table would crash the
     * TTBR0 flip on next pick.  Same convention as ktest_wait. */
    nx_syscall_reset_for_test();
    KASSERT_EQ_U(nx_syscall_debug_write_calls(), 0);

    /* Drain every user-process task left behind by earlier slice
     * 7.4 tests.  Without this, the stranded fork-child + wait-
     * child tasks (both parked in wfe at EL0) each consume a full
     * SCHED_RR quantum (1 second) before the scheduler rotates
     * past them, which pushes this test past the 15-second QEMU
     * timeout.  Safe: storage isn't freed, just unlinked — the
     * tasks stay out of the runqueue until (a future) real reap
     * destroys them. */
    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    seed_init_file();

    uint32_t exec_parent_pid;
    g_exec_parent = nx_process_create("exec-parent");
    KASSERT_NOT_NULL(g_exec_parent);
    exec_parent_pid = g_exec_parent->pid;

    g_exec_task = sched_spawn_kthread("exec-el0", exec_el0_kthread, 0);
    KASSERT_NOT_NULL(g_exec_task);
    g_exec_task->process = g_exec_parent;

    /* Three markers expected: [exec-parent], [el0-elf-ok] (from
     * the exec'd init_prog), [exec-ok] (from parent after wait
     * confirms status == 17). */
    const int max_yields = 2048;
    int reached = 0;
    for (int i = 0; i < max_yields; i++) {
        if (nx_syscall_debug_write_calls() >= 3) { reached = 1; break; }
        nx_task_yield();
    }
    KASSERT(reached);

    /* Independent check: the child process that was forked then
     * exec'd should have exit_code == 17. */
    int found = 0;
    for (uint32_t pid = exec_parent_pid + 1; pid < 16; pid++) {
        struct nx_process *p = nx_process_lookup_by_pid(pid);
        if (!p) continue;
        if (p->state != NX_PROCESS_STATE_EXITED) continue;
        if (p->exit_code != 17) continue;
        found = 1;
        break;
    }
    KASSERT(found);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_exec_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_exec_parent = NULL;
    g_exec_task   = NULL;
}
