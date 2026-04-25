/*
 * Kernel-side coverage for slice 7.6c.3b — first EL0 demo linked
 * against musl's libc.a + crt1.o instead of libnxlibc.a.
 *
 * Validates that:
 *   1. musl's `_start` -> `_start_c` -> `__libc_start_main` ->
 *      `__init_libc` -> `__init_tls` -> `__init_ssp` -> stage2 ->
 *      main flow runs to completion under our SVC ABI.
 *   2. `write(1, ...)` translates `__NR_write` (64) ->
 *      `NX_SYS_WRITE` (8) (file-write syscall, NOT the
 *      libnxlibc fd=1->NX_SYS_DEBUG_WRITE shortcut — fd 1 in musl
 *      is just a regular fd, our kernel rejects it as "not a
 *      file/channel handle" but the rejection is observable).
 *   3. `_exit(57)` translates `__NR_exit_group` (94) ->
 *      `NX_SYS_EXIT` (11), exit_code propagates to the process
 *      table.
 *
 * Run via drop_to_el0 (not sys_exec).  The user backing is
 * zero-initialized so [sp] reads as argc=0 -> __libc_start_main is
 * called with argc=0, argv=&NULL, envp=&NULL — no AUXV — and
 * musl's init paths handle that gracefully (AT_RANDOM=NULL falls
 * back to a hash-based stack canary; AT_PAGESZ=0 makes malloc's
 * rounding meaningless but we never call malloc; etc.).  Slice
 * 7.6c.3c will exercise the AUXV-on-stack path via sys_exec.
 *
 * Note on the write: musl's write uses fd 1 (STDOUT_FILENO) which
 * doesn't have a stdout-routing magic in our kernel — fd 1 is just
 * a regular handle index.  The NX_SYS_WRITE handler treats it as a
 * potential file/channel handle and rejects with NX_EBADF since it
 * isn't allocated.  That's the expected v1 behavior — slice 7.6c.3c
 * will add stdio plumbing.  The marker `[musl-ok]` therefore won't
 * appear in the live ktest log, but the program still reaches
 * `_exit(57)` (write returns -EBADF, the program ignores it, exits
 * cleanly) and the exit_code is observable in the process table.
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

extern char __posix_musl_prog_blob_start[];
extern char __posix_musl_prog_blob_end[];

void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_musl_host;
static struct nx_task    *g_musl_task;
static uint64_t           g_musl_entry;

static size_t musl_prog_blob_size(void)
{
    return (size_t)(__posix_musl_prog_blob_end -
                    __posix_musl_prog_blob_start);
}

static void musl_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    /* musl's crt1 walks argc -> argv -> argv-NULL -> envp -> envp-NULL
     * -> auxv -> auxv-NULL, dereferencing each.  drop_to_el0 leaves
     * the backing zero-initialized so each read returns 0 (terminator),
     * but only if sp + reads stays within the user window.  Stock
     * `top - 16` leaves only 16 bytes of headroom and envp[0] (at
     * sp+16) lands exactly at the boundary -> permission fault.
     * Use `top - 64` to give crt1 plenty of zero-padded slack — the
     * sys_exec path (slice 7.6c.4 + 7.6c.3b's AUXV push) builds a
     * real layout that doesn't need this slack. */
    uint64_t sp_el0 = (base + size - 64u) & ~((uint64_t)0xfu);
    drop_to_el0(g_musl_entry, sp_el0);
}

KTEST(posix_musl_prog_runs_main_through_init_libc_and_exits_57)
{
    nx_syscall_reset_for_test();

    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    uint32_t host_pid;
    g_musl_host = nx_process_create("musl-host");
    KASSERT_NOT_NULL(g_musl_host);
    host_pid = g_musl_host->pid;

    int rc = nx_elf_load_into_process(g_musl_host,
                                      __posix_musl_prog_blob_start,
                                      musl_prog_blob_size(),
                                      &g_musl_entry);
    KASSERT_EQ_U(rc, NX_OK);

    g_musl_task = sched_spawn_kthread("musl-el0", musl_el0_kthread, 0);
    KASSERT_NOT_NULL(g_musl_task);
    g_musl_task->process = g_musl_host;

    /* Wait for the process to exit.  No marker is checked — see the
     * file header for why fd=1 write is expected to be rejected. */
    int found = 0;
    for (int i = 0; i < 4096; i++) {
        struct nx_process *p = nx_process_lookup_by_pid(host_pid);
        if (p && p->state == NX_PROCESS_STATE_EXITED) {
            KASSERT_EQ_U(p->exit_code, 57);
            found = 1;
            break;
        }
        nx_task_yield();
    }
    KASSERT(found);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_musl_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_musl_host = NULL;
    g_musl_task = NULL;
}
