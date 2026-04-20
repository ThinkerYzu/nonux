/*
 * EL1 exception vector table.
 *
 * 16 entries of 128 bytes, 2 KiB aligned.  The common save/restore code is
 * too large to fit in a 32-instruction vector entry, so each entry is a
 * single branch to an out-of-line stub.
 *
 * Trap frame layout (272 bytes, 16-byte aligned):
 *   0x000: x0..x1     (stp pairs, 16 bytes each)
 *   0x010: x2..x3
 *   ...
 *   0x0E0: x28..x29
 *   0x0F0: x30 + SP_EL0 placeholder
 *   0x100: ELR_EL1 + SPSR_EL1
 */

.section .text

.macro SAVE_TRAPFRAME
    sub     sp, sp, #272
    stp     x0,  x1,  [sp, #0x000]
    stp     x2,  x3,  [sp, #0x010]
    stp     x4,  x5,  [sp, #0x020]
    stp     x6,  x7,  [sp, #0x030]
    stp     x8,  x9,  [sp, #0x040]
    stp     x10, x11, [sp, #0x050]
    stp     x12, x13, [sp, #0x060]
    stp     x14, x15, [sp, #0x070]
    stp     x16, x17, [sp, #0x080]
    stp     x18, x19, [sp, #0x090]
    stp     x20, x21, [sp, #0x0A0]
    stp     x22, x23, [sp, #0x0B0]
    stp     x24, x25, [sp, #0x0C0]
    stp     x26, x27, [sp, #0x0D0]
    stp     x28, x29, [sp, #0x0E0]
    str     x30,      [sp, #0x0F0]
    str     xzr,      [sp, #0x0F8]
    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x0,  x1,  [sp, #0x100]
.endm

.macro RESTORE_TRAPFRAME
    ldp     x0,  x1,  [sp, #0x100]
    msr     elr_el1,  x0
    msr     spsr_el1, x1
    ldr     x30,      [sp, #0x0F0]
    ldp     x28, x29, [sp, #0x0E0]
    ldp     x26, x27, [sp, #0x0D0]
    ldp     x24, x25, [sp, #0x0C0]
    ldp     x22, x23, [sp, #0x0B0]
    ldp     x20, x21, [sp, #0x0A0]
    ldp     x18, x19, [sp, #0x090]
    ldp     x16, x17, [sp, #0x080]
    ldp     x14, x15, [sp, #0x070]
    ldp     x12, x13, [sp, #0x060]
    ldp     x10, x11, [sp, #0x050]
    ldp     x8,  x9,  [sp, #0x040]
    ldp     x6,  x7,  [sp, #0x030]
    ldp     x4,  x5,  [sp, #0x020]
    ldp     x2,  x3,  [sp, #0x010]
    ldp     x0,  x1,  [sp, #0x000]
    add     sp, sp, #272
.endm

.balign 2048
.global vectors
vectors:
    /* --- Current EL with SP_EL0 (unused — we run on SP_EL1) --- */
    .balign 0x80
    b       _unimpl_stub
    .balign 0x80
    b       _unimpl_stub
    .balign 0x80
    b       _unimpl_stub
    .balign 0x80
    b       _unimpl_stub

    /* --- Current EL with SP_ELx (EL1h — this is us) --- */
    .balign 0x80
    b       _sync_stub
    .balign 0x80
    b       _irq_stub
    .balign 0x80
    b       _fiq_stub
    .balign 0x80
    b       _serror_stub

    /* --- Lower EL using AArch64 (userspace — later phases) --- */
    .balign 0x80
    b       _unimpl_stub
    .balign 0x80
    b       _unimpl_stub
    .balign 0x80
    b       _unimpl_stub
    .balign 0x80
    b       _unimpl_stub

    /* --- Lower EL using AArch32 (not supported) --- */
    .balign 0x80
    b       _unimpl_stub
    .balign 0x80
    b       _unimpl_stub
    .balign 0x80
    b       _unimpl_stub
    .balign 0x80
    b       _unimpl_stub

_sync_stub:
    SAVE_TRAPFRAME
    mov     x0, sp
    bl      on_sync
    RESTORE_TRAPFRAME
    eret

_irq_stub:
    SAVE_TRAPFRAME
    mov     x0, sp
    bl      on_irq
    RESTORE_TRAPFRAME
    eret

_fiq_stub:
    SAVE_TRAPFRAME
    mov     x0, sp
    bl      on_fiq
    RESTORE_TRAPFRAME
    eret

_serror_stub:
    SAVE_TRAPFRAME
    mov     x0, sp
    bl      on_serror
    RESTORE_TRAPFRAME
    eret

_unimpl_stub:
    SAVE_TRAPFRAME
    mov     x0, sp
    bl      on_unimpl
    RESTORE_TRAPFRAME
    eret
