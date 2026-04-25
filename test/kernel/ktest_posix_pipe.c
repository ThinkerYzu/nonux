/*
 * Kernel-side coverage for slice 7.5 — NX_SYS_PIPE.
 *
 * Loads the C-compiled `posix_pipe_prog.elf` (which uses the
 * `nx_posix_pipe / write / read / close` wrappers from
 * `components/posix_shim/posix.h`) into a fresh process, drops to
 * EL0, and verifies the live log gains `[pipe-ok]` + the process
 * ends EXITED with exit_code == 29.  Mechanics:
 *
 *   1. Purge stranded user-process tasks (same pattern as
 *      `ktest_exec`/`ktest_posix`) so the 15 s QEMU budget holds.
 *   2. Create `pipe-host` process; load the ELF via the slice 7.3
 *      loader.
 *   3. Spawn kthread pinned to it, drop to EL0.
 *   4. Yield until the debug_write counter hits 1 (the
 *      `[pipe-ok]` marker is the single debug_write the program
 *      emits — errors return non-zero exit codes without hitting
 *      debug_write).
 *   5. Confirm the host process is EXITED with exit_code == 29.
 *
 * If sys_pipe ever mis-allocates handle rights (e.g. puts READ on
 * both ends, or drops write rights on fds[1]) the write or read
 * inside the EL0 program returns NX_EPERM, the comparison fails,
 * the exit code becomes non-29, and this test fails loudly.
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

extern char __posix_pipe_prog_blob_start[];
extern char __posix_pipe_prog_blob_end[];

void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_pipe_host;
static struct nx_task    *g_pipe_task;
static uint64_t           g_pipe_entry;

static size_t pipe_prog_blob_size(void)
{
    return (size_t)(__posix_pipe_prog_blob_end - __posix_pipe_prog_blob_start);
}

static void pipe_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(g_pipe_entry, sp_el0);
}

KTEST(posix_pipe_write_read_roundtrip_via_nx_posix_wrappers)
{
    nx_syscall_reset_for_test();
    KASSERT_EQ_U(nx_syscall_debug_write_calls(), 0);

    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    uint32_t host_pid;
    g_pipe_host = nx_process_create("pipe-host");
    KASSERT_NOT_NULL(g_pipe_host);
    host_pid = g_pipe_host->pid;

    int rc = nx_elf_load_into_process(g_pipe_host,
                                      __posix_pipe_prog_blob_start,
                                      pipe_prog_blob_size(),
                                      &g_pipe_entry);
    KASSERT_EQ_U(rc, NX_OK);
    KASSERT_EQ_U(g_pipe_entry, mmu_user_window_base());

    g_pipe_task = sched_spawn_kthread("pipe-el0", pipe_el0_kthread, 0,
                                      g_pipe_host);
    KASSERT_NOT_NULL(g_pipe_task);

    /* Single debug_write expected: `[pipe-ok]`.  Any earlier error
     * in the EL0 program calls exit() before hitting debug_write,
     * so a counter of 1 means the round-trip succeeded. */
    const int max_yields = 2048;
    int reached = 0;
    for (int i = 0; i < max_yields; i++) {
        if (nx_syscall_debug_write_calls() >= 1) { reached = 1; break; }
        nx_task_yield();
    }
    KASSERT(reached);

    /* The process must have reached exit(29) after emitting the
     * marker — verifying the exit code catches mis-allocated
     * rights / dispatch failures that would make the EL0 program
     * take its error-exit branches (1..4). */
    int found = 0;
    for (int i = 0; i < 64; i++) {
        struct nx_process *p = nx_process_lookup_by_pid(host_pid);
        if (p && p->state == NX_PROCESS_STATE_EXITED) {
            KASSERT_EQ_U(p->exit_code, 29);
            found = 1;
            break;
        }
        nx_task_yield();
    }
    KASSERT(found);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_pipe_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_pipe_host = NULL;
    g_pipe_task = NULL;
}
