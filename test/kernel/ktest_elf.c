/*
 * Kernel-side coverage for slice 7.3.
 *
 * `test/kernel/init_prog_blob.S` embeds the standalone `init_prog.elf`
 * (linked at VA 0x48000000) into kernel-test.bin's .rodata, flanked
 * by `__init_prog_blob_{start,end}` labels.  This test:
 *
 *   1. Creates a fresh user process (slice-7.2 per-process address
 *      space).
 *   2. Calls `nx_elf_load_into_process` to copy every PT_LOAD
 *      segment from the embedded blob into the process's user-
 *      window backing.
 *   3. Spawns a kthread, reassigns it to the new process, and
 *      drops to EL0 at the ELF's entry point.
 *   4. Yields until the debug_write counter rises — proof the
 *      EL0 code executed `NX_SYS_DEBUG_WRITE("[el0-elf-ok]", 13)`.
 *      Live ktest log carries the marker bytes.
 *   5. Dequeues the stranded task.
 *
 * Two complementary tests: one inspects the parser output
 * (header fields, segment count / shape), the other drives the
 * end-to-end load + drop.
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

extern char __init_prog_blob_start[];
extern char __init_prog_blob_end[];

static size_t init_prog_blob_size(void)
{
    return (size_t)(__init_prog_blob_end - __init_prog_blob_start);
}

KTEST(elf_parse_header_reports_entry_and_one_pt_load)
{
    struct nx_elf_info info = {0};
    int rc = nx_elf_parse(__init_prog_blob_start, init_prog_blob_size(),
                          &info);
    KASSERT_EQ_U(rc, NX_OK);
    /* init_prog.ld pins the entry at the user-window base. */
    KASSERT_EQ_U(info.entry, mmu_user_window_base());
    /* init_prog.ld puts everything in one section → one PT_LOAD. */
    KASSERT_EQ_U(info.segment_count, 1);
}

KTEST(elf_parse_rejects_malformed_magic)
{
    /* Copy the blob into a mutable buffer so we can smash the magic
     * without touching the real .rodata (which would likely fault). */
    static uint8_t corrupt[128];
    for (int i = 0; i < 128; i++) corrupt[i] = __init_prog_blob_start[i];
    corrupt[0] = 0xFF;  /* not 0x7F */

    struct nx_elf_info info = {0};
    int rc = nx_elf_parse(corrupt, 128, &info);
    KASSERT_EQ_U((uint64_t)rc, (uint64_t)(int64_t)NX_EINVAL);
}

/* ---------- end-to-end load + EL0 round-trip ------------------------ */

static struct nx_process *g_elf_target;
static struct nx_task    *g_elf_task;
static uint64_t           g_elf_entry;

static void elf_el0_kthread(void *arg)
{
    (void)arg;
    /* On entry, sched_check_resched has already flipped TTBR0 to our
     * process's root (the kthread's `process` pointer was reassigned
     * before it ran).  The ELF was loaded while the test body held
     * the kernel address space — the bytes live in our user backing. */
    uint64_t base = mmu_user_window_base();
    uint64_t size = mmu_user_window_size();
    uint64_t sp_el0 = (base + size - 16u) & ~((uint64_t)0xfu);
    drop_to_el0(g_elf_entry, sp_el0);
}

KTEST(elf_load_and_drop_to_el0_runs_marker_syscall)
{
    nx_syscall_reset_for_test();
    nx_process_reset_for_test();
    KASSERT_EQ_U(nx_syscall_debug_write_calls(), 0);

    g_elf_target = nx_process_create("elf-target");
    KASSERT_NOT_NULL(g_elf_target);

    /* Load the ELF's PT_LOADs into the target's user backing.  The
     * kernel-side copy writes via the backing's identity-mapped
     * kernel VA (returned by `mmu_address_space_user_backing`), so
     * no TTBR0 flip is needed during the copy itself. */
    int rc = nx_elf_load_into_process(g_elf_target,
                                      __init_prog_blob_start,
                                      init_prog_blob_size(),
                                      &g_elf_entry);
    KASSERT_EQ_U(rc, NX_OK);
    KASSERT_EQ_U(g_elf_entry, mmu_user_window_base());

    g_elf_task = sched_spawn_kthread("elf-el0", elf_el0_kthread, 0);
    KASSERT_NOT_NULL(g_elf_task);
    g_elf_task->process = g_elf_target;

    /* Yield until the EL0 program reaches its single debug_write.
     * That SVC is the only counter-bumping event in the program, so
     * a non-zero count means the program ran. */
    const int max_yields = 256;
    int reached = 0;
    for (int i = 0; i < max_yields; i++) {
        if (nx_syscall_debug_write_calls() > 0) { reached = 1; break; }
        nx_task_yield();
    }
    KASSERT(reached);

    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    ops->dequeue(self, g_elf_task);

    /* Restore kernel address space so subsequent tests aren't
     * accidentally running against the target's TTBR0. */
    mmu_switch_address_space(mmu_kernel_address_space());
    nx_process_destroy(g_elf_target);
    g_elf_target = NULL;
    g_elf_task   = NULL;
}
