/*
 * nonux — ARM64 boot entry point
 *
 * QEMU virt machine enters at _start in EL1 (or EL2 depending on config).
 * We drop to EL1 if needed, set up the stack, zero BSS, and jump to boot_main().
 */

.section .text.boot

.global _start
_start:
    /* Check current exception level */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2         /* extract EL field (bits [3:2]) */
    cmp     x0, #2
    b.eq    from_el2
    cmp     x0, #1
    b.eq    at_el1
    /* EL3 or EL0 — not expected from QEMU, just hang */
    b       hang

from_el2:
    /* Configure EL1 from EL2 */

    /* CNTHCTL_EL2: allow EL1 to use the physical timer and counter.
     * Without this, CNTP_*_EL0 accesses from EL1 trap to EL2. */
    mrs     x0, cnthctl_el2
    orr     x0, x0, #3          /* EL1PCEN | EL1PCTEN */
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr    /* virt counter matches physical */

    /* HCR_EL2: RW=1 (EL1 is AArch64) */
    mov     x0, #(1 << 31)
    msr     hcr_el2, x0

    /* SPSR_EL2: return to EL1h (EL1, SP_EL1) with interrupts masked */
    mov     x0, #0x3c5         /* D=1, A=1, I=1, F=1, M=EL1h (0b00101) */
    msr     spsr_el2, x0

    /* ELR_EL2: return address is at_el1 */
    adr     x0, at_el1
    msr     elr_el2, x0

    /* Return to EL1 */
    eret

at_el1:
    /* Set up stack pointer (grows downward from __stack_top) */
    ldr     x0, =__stack_top
    mov     sp, x0

    /* Zero BSS section */
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
bss_loop:
    cmp     x0, x1
    b.ge    bss_done
    str     xzr, [x0], #8
    b       bss_loop
bss_done:

    /* Jump to C entry point */
    bl      boot_main

    /* boot_main should not return, but if it does, hang */
hang:
    wfe
    b       hang
