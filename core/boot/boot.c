#include "core/lib/lib.h"

/*
 * boot_main — C entry point after start.S sets up EL1, stack, and BSS.
 *
 * Phase 1: Just print a banner and halt.
 * Future phases will add: DTB parsing, PMM init, GIC init,
 * component framework init, and scheduler launch.
 */

extern char __bss_start[];
extern char __bss_end[];
extern char __kernel_end[];
extern char __free_mem_start[];

void boot_main(void)
{
    uart_init();

    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  nonux — composable microkernel\n");
    kprintf("  ARM64 / QEMU virt\n");
    kprintf("========================================\n");
    kprintf("\n");

    kprintf("[boot] kernel loaded at 0x40000000\n");
    kprintf("[boot] BSS:  %p — %p\n",
            (uint64_t)__bss_start, (uint64_t)__bss_end);
    kprintf("[boot] kernel end:    %p\n", (uint64_t)__kernel_end);
    kprintf("[boot] free memory:   %p\n", (uint64_t)__free_mem_start);
    kprintf("\n");
    kprintf("[boot] Phase 1 complete — halting.\n");

    /* Halt — future phases will launch the scheduler here */
    for (;;)
        asm volatile("wfe");
}
