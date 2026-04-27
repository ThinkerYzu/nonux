/*
 * Kernel-side coverage for slice 7.6c.3c — musl-linked printf demo.
 *
 * Validates that musl's full stdio surface (vfprintf + the locale
 * machinery + softfloat-via-libgcc) runs end-to-end on nonux.
 * Loaded via drop_to_el0 with `sp_el0 = top - 64` (same pattern as
 * ktest_posix_musl).
 *
 * The demo's printf format string exercises every "doesn't need
 * float" conversion: `%d %u %x %s %c \n`.  vfprintf still pulls in
 * the float-formatting code path because the format-string parser
 * dispatches on the conversion at runtime — even when no `%f` is
 * present, the %f code is reachable and so the linker keeps it.
 * That's what forced the `LIBGCC` addition to the link line:
 * vfprintf references `__netf2` etc. (long-double softfloat).
 *
 * If musl's locale init or vfprintf calls into mmap (heap
 * extension), that returns -ENOSYS in our v1 (NX_SYS_MMAP isn't
 * implemented yet).  musl's vfprintf is engineered to use stack-
 * local storage for the simple paths — the no-`%f` printf below
 * fits in that path.  An eventual %f-using demo would force
 * NX_SYS_MMAP to land before it could pass.
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

extern char __posix_musl_printf_prog_blob_start[];
extern char __posix_musl_printf_prog_blob_end[];

void sched_rr_purge_user_tasks(void *self, struct nx_task *keep);

static struct nx_process *g_mp_host;
static struct nx_task    *g_mp_task;
static uint64_t           g_mp_entry;

static size_t mp_blob_size(void)
{
    return (size_t)(__posix_musl_printf_prog_blob_end -
                    __posix_musl_printf_prog_blob_start);
}

static void mp_el0_kthread(void *arg)
{
    (void)arg;
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    /* Same `top - 64` slack as posix_musl's ktest — drop_to_el0
     * leaves zero-init backing, musl's __init_libc envp/auxv walk
     * needs a few terminators below the boundary. */
    uint64_t sp_el0 = (base + size - 64u) & ~((uint64_t)0xfu);
    drop_to_el0(g_mp_entry, sp_el0);
}

KTEST(posix_musl_printf_emits_every_conversion_and_exits_67)
{
    nx_syscall_reset_for_test();
    KASSERT_EQ_U(nx_syscall_debug_write_calls(), 0);

    void *sself = sched_self_for_test();
    sched_rr_purge_user_tasks(sself, NULL);

    uint32_t host_pid;
    g_mp_host = nx_process_create("musl-printf");
    KASSERT_NOT_NULL(g_mp_host);
    host_pid = g_mp_host->pid;

    int rc = nx_elf_load_into_process(g_mp_host,
                                      __posix_musl_printf_prog_blob_start,
                                      mp_blob_size(),
                                      &g_mp_entry);
    KASSERT_EQ_U(rc, NX_OK);

    g_mp_task = sched_spawn_kthread("musl-pf-el0", mp_el0_kthread, 0,
                                    g_mp_host);
    KASSERT_NOT_NULL(g_mp_task);

    int found = 0;
    for (int i = 0; i < 8192; i++) {
        struct nx_process *p = nx_process_lookup_by_pid(host_pid);
        if (p && p->state == NX_PROCESS_STATE_EXITED) {
            KASSERT_EQ_U(p->exit_code, 67);
            found = 1;
            break;
        }
        nx_task_yield();
    }
    KASSERT(found);
    /* musl's vfprintf write-flushes the formatted output as a single
     * buffered write through fd 1 → CONSOLE handle (slice 7.6d.N.6b)
     * → nx_console_write.  Pre-7.6d.N.6b this hit the magic-fd
     * fallback to NX_SYS_DEBUG_WRITE; the counter rename reflects
     * the new dispatch path. */
    KASSERT(nx_console_write_calls() >= 1);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_mp_task);

    mmu_switch_address_space(mmu_kernel_address_space());
    g_mp_host = NULL;
    g_mp_task = NULL;
}
