/*
 * Kernel-side coverage for slice 7.6c.4 — sys_exec argv push.
 *
 * Validates that:
 *   1. `nxlibc_execve(path, argv, NULL)` forwards argv through
 *      `NX_SYS_EXEC` to the kernel.
 *   2. `sys_exec` copies the argv strings out of the OLD address
 *      space's user memory before the TTBR0 flip.
 *   3. `sys_exec` builds the System V argv layout in the new
 *      user backing (via `mmu_address_space_user_backing`'s kernel
 *      alias) at the high end of the user window.
 *   4. The trap-frame `sp_el0` is set to the `argc` slot inside
 *      that layout, so when the exec'd child enters EL0,
 *      `crt0` reads argc from `[sp]` and the right values are
 *      there.
 *   5. The crt0 → main argument plumbing forwards argc/argv into
 *      the C-level `int main(int argc, char **argv, char **envp)`
 *      signature — matched against the parent's
 *      `{ "/argv_child", "hello", "world", NULL }`.
 *
 * Ktest pins the round-trip with three observations:
 *   a. The child reaches its `[argv-child-ok]` marker (proves
 *      every argv slot matched bytes-for-bytes).
 *   b. The parent emits `[argv-ok]` after observing the child's
 *      exit status == 63 (= 3 + 60 from the child's success path).
 *   c. Some EXITED process in the table has exit_code == 63.
 *
 * If any layer of the round-trip drops or corrupts an argv slot,
 * the child takes one of its sentinel-exit branches (81..85) — the
 * `[argv-ok]` marker never fires, the ktest's status assertion
 * fails, and the live log carries which slot regressed.
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

extern char __argv_parent_prog_blob_start[];
extern char __argv_parent_prog_blob_end[];

void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_argv_host;
static struct nx_task    *g_argv_task;
static uint64_t           g_argv_entry;

static size_t argv_blob_size(void)
{
    return (size_t)(__argv_parent_prog_blob_end -
                    __argv_parent_prog_blob_start);
}

static void argv_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(g_argv_entry, sp_el0);
}

KTEST(sys_exec_pushes_argv_and_child_observes_strings_via_main)
{
    nx_syscall_reset_for_test();
    KASSERT_EQ_U(nx_syscall_debug_write_calls(), 0);

    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    uint32_t host_pid;
    g_argv_host = nx_process_create("argv-host");
    KASSERT_NOT_NULL(g_argv_host);
    host_pid = g_argv_host->pid;

    int rc = nx_elf_load_into_process(g_argv_host,
                                      __argv_parent_prog_blob_start,
                                      argv_blob_size(),
                                      &g_argv_entry);
    KASSERT_EQ_U(rc, NX_OK);

    g_argv_task = sched_spawn_kthread("argv-el0", argv_el0_kthread, 0,
                                      g_argv_host);
    KASSERT_NOT_NULL(g_argv_task);

    /* Markers expected (in any scheduling order between parent and
     * child, but all 6 must arrive):
     *   parent:  [argv-parent], [argv-ok]                       — 2 writes
     *   child:   [argv-child argc=3]                            — 1 write
     *            [argv-child argv[0..2]=...] (3 prints)         — 3 writes
     *            [argv-child-ok]                                — 1 write
     * Total: 7 debug_writes. */
    const int max_yields = 4096;
    int reached = 0;
    for (int i = 0; i < max_yields; i++) {
        if (nx_syscall_debug_write_calls() >= 7) { reached = 1; break; }
        nx_task_yield();
    }
    KASSERT(reached);

    /* The forked-then-exec'd child should have exit_code == 63
     * (= argc + 60 from argv_child_prog's success path).  Search
     * pids > host_pid so stranded processes from earlier ktests
     * can't spoof the match.  Upper bound is the new
     * NX_PROCESS_TABLE_CAPACITY = 32; pid_next is monotonic and
     * the cumulative-test count of created processes hits the
     * teens by the time argv_push runs. */
    int found_child = 0;
    for (uint32_t pid = host_pid + 1; pid < 64; pid++) {
        struct nx_process *p = nx_process_lookup_by_pid(pid);
        if (!p) continue;
        if (p->state != NX_PROCESS_STATE_EXITED) continue;
        if (p->exit_code != 63) continue;
        found_child = 1;
        break;
    }
    KASSERT(found_child);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_argv_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_argv_host = NULL;
    g_argv_task = NULL;
}
