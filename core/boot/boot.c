#include "core/lib/lib.h"
#include "core/pmm/pmm.h"
#include "core/cpu/exception.h"
#include "core/irq/irq.h"
#include "core/timer/timer.h"

/*
 * boot_main — C entry point after start.S sets up EL1, stack, and BSS.
 *
 * Phase 2 bring-up order:
 *   1. UART (already done in Phase 1).
 *   2. PMM over all RAM above __free_mem_start up to RAM_END.
 *   3. Exception vectors (VBAR_EL1).
 *   4. GIC distributor + CPU interface.
 *   5. ARM Generic Timer at 10 Hz.
 *   6. Unmask IRQ, park in wfi.  Tick handler prints once/sec.
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

    irq_enable_local();

#ifdef NX_KTEST
    /* When built with -DNX_KTEST, run the in-kernel test suite instead
     * of the idle loop.  ktest_main exits via semihosting. */
    extern void ktest_main(void) __attribute__((noreturn));
    ktest_main();
#else
    kprintf("[boot] Phase 2 ready — tick prints once/sec.\n\n");

    for (;;)
        asm volatile("wfi");
#endif
}
