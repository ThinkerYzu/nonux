#include "core/lib/lib.h"
#include "core/mmu/mmu.h"
#include "core/pmm/pmm.h"
#include "core/cpu/exception.h"
#include "core/irq/irq.h"
#include "core/timer/timer.h"
#include "core/sched/sched.h"
#include "framework/bootstrap.h"
#include "framework/registry.h"

/*
 * boot_main — C entry point after start.S sets up EL1, stack, and BSS.
 *
 * Bring-up order:
 *   1. UART (low-level setup done in Phase 1).
 *   2. PMM over all RAM above __free_mem_start up to RAM_END.
 *   3. Exception vectors (VBAR_EL1).
 *   4. GIC distributor + CPU interface.
 *   5. ARM Generic Timer at 10 Hz.
 *   6. Framework bootstrap (slice 3.9a): walk the nx_components linker
 *      section, register slots + components from gen/slot_table.c,
 *      run init → enable in topo order, dump the composition.
 *   7. Unmask IRQ, park in wfi.  Tick handler prints once/sec.
 *
 * RAM_END is hardcoded to 0x80000000 (matches QEMU -m 1G).  Phase 5's DTB
 * parsing will replace this with the DTB-reported value.
 */

extern char __bss_start[];
extern char __bss_end[];
extern char __kernel_end[];
extern char __free_mem_start[];
extern char vectors[];

#define RAM_END 0x80000000UL

void boot_main(void)
{
    uart_init();

    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  nonux — composable microkernel\n");
    kprintf("  ARM64 / QEMU virt\n");
    kprintf("========================================\n");
    kprintf("\n");

    /* Slice 5.1: turn on the MMU as early as possible so the rest of
     * bring-up runs with D-cache + I-cache enabled and Normal-memory
     * semantics (unaligned access, weaker ordering).  VA = PA
     * everywhere we map — turning it on does not change any symbol
     * address, only how loads/stores / fetches behave. */
    mmu_init();
    kprintf("[mmu]  enabled (identity map: MMIO 0..1G Device, RAM 1..2G Normal)\n");

    kprintf("[boot] kernel loaded at 0x40080000 (QEMU's -kernel offset)\n");
    kprintf("[boot] BSS:  %p — %p\n",
            (uint64_t)__bss_start, (uint64_t)__bss_end);
    kprintf("[boot] kernel end:    %p\n", (uint64_t)__kernel_end);
    kprintf("[boot] free memory:   %p — %p\n",
            (uint64_t)__free_mem_start, (uint64_t)RAM_END);
    kprintf("\n");

    uintptr_t pmm_base = (uintptr_t)__free_mem_start;
    pmm_init(pmm_base, (size_t)(RAM_END - pmm_base));
    kprintf("[pmm]  total=%lu free=%lu pages (%lu KiB)\n",
            (uint64_t)pmm_total_count(),
            (uint64_t)pmm_free_count(),
            (uint64_t)pmm_free_count() * 4);

    vectors_install();
    kprintf("[cpu]  exception vectors installed at %p\n", (uint64_t)vectors);

    gic_init();
    kprintf("[gic]  distributor + CPU interface enabled\n");

    timer_init(10);

    /* Phase 3 bring-up — walk nx_components, register every slot +
     * descriptor, run init / enable in topo order.  Any non-OK return
     * leaves the composition partially up; for now we just log and
     * carry on (slice 3.9b will own the rollback story). */
    int fw_rc = nx_framework_bootstrap();
    if (fw_rc == NX_OK) {
        static char snap_buf[4096];
        struct nx_graph_snapshot *snap = nx_graph_snapshot_take();
        if (snap) {
            int n = nx_graph_snapshot_to_json(snap, snap_buf, sizeof snap_buf);
            kprintf("[fw]   composition (gen=%lu, %lu slots, %lu components):\n",
                    (uint64_t)nx_graph_snapshot_generation(snap),
                    (uint64_t)nx_graph_snapshot_slot_count(snap),
                    (uint64_t)nx_graph_snapshot_component_count(snap));
            if (n > 0) uart_puts(snap_buf);
            uart_puts("\n");
            nx_graph_snapshot_put(snap);
        }
    } else {
        kprintf("[fw]   bootstrap failed (rc=%d)\n", fw_rc);
    }

    /* Slice 4.4: transition the boot context into the idle task and
     * enqueue it as the scheduler's runqueue fallback BEFORE enabling
     * IRQs.  Doing it in this order means the very first tick after
     * irq_enable_local can drive sched_tick/sched_check_resched
     * against a well-formed current.  No-op if sched_init hasn't
     * run (e.g. the composition doesn't wire up a scheduler slot). */
    sched_start();

    irq_enable_local();

#ifdef NX_KTEST
    /* When built with -DNX_KTEST, run the in-kernel test suite instead
     * of the idle loop.  ktest_main runs in the idle-task context
     * (TPIDR_EL1 now points at the core driver's idle_task); tests
     * can spawn kthreads that preempt ktest_main via timer ticks and
     * yield back cooperatively.  ktest_main exits via semihosting. */
    extern void ktest_main(void) __attribute__((noreturn));
    ktest_main();
#else
    kprintf("[boot] idle: waiting for work.\n\n");

    for (;;)
        asm volatile("wfi");
#endif
}
