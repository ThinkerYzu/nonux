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
    /* Slice 5.5: preserve SP_EL0 across the exception so EL0 callers
     * get their stack back on eret.  For EL1-origin traps SP_EL0 is
     * usually unused but we still preserve it — cheaper than branching
     * on the origin EL. */
    mrs     x2, sp_el0
    str     x2,       [sp, #0x0F8]
    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x0,  x1,  [sp, #0x100]
.endm

.macro RESTORE_TRAPFRAME
    ldp     x0,  x1,  [sp, #0x100]
    msr     elr_el1,  x0
    msr     spsr_el1, x1
    /* Slice 5.5: restore SP_EL0 before reloading x2 from memory. */
    ldr     x2,       [sp, #0x0F8]
    msr     sp_el0,  x2
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

    /* --- Lower EL using AArch64 (EL0 userspace, slice 5.5+) --- *
     *
     * Sync (SVC-from-EL0) routes to the same stub as sync-from-EL1 —
     * the dispatcher in on_sync reads ESR.EC to distinguish SVC from
     * real faults, and nx_syscall_dispatch itself doesn't care which
     * EL the caller was at.  IRQ/FIQ/SError reuse the same core-
     * driven stubs; they're EL-agnostic by construction. */
    .balign 0x80
    b       _sync_stub
    .balign 0x80
    b       _irq_stub
    .balign 0x80
    b       _fiq_stub
    .balign 0x80
    b       _serror_stub

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
    /* Slice 4.4: consult the reschedule shim before returning to the
     * interrupted context.  sched_check_resched reads
     * current->need_resched and, if set and preempt_count == 0, calls
     * pick_next + cpu_switch_to on this stack — that call may not
     * return here until the interrupted task is picked again in a
     * future reschedule.  Harmless no-op until sched_init has been
     * called. */
    bl      sched_check_resched
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

/*
 * Slice 7.4 — first-switch entry for a fork-created child task.
 *
 * When the scheduler picks a forked child for the first time,
 * `cpu_switch_to` restores its callee-saved regs + `ret`s to LR.
 * That LR was written by `nx_task_create_forked` to be
 * `nx_task_fork_child_entry` (this symbol).  At entry, SP points
 * at the child's pre-populated trap frame — an exact copy of the
 * parent's trap frame at the fork SVC, with `x[0]` overwritten to
 * 0 (the child's fork return value).
 *
 * We do RESTORE_TRAPFRAME (which both loads the saved x0..x30 +
 * SP_EL0 + ELR_EL1/SPSR_EL1 AND pops the 272-byte frame from the
 * kernel stack) then eret.  The eret delivers control to EL0 at
 * the instruction after the fork SVC, with x0 = 0.
 *
 * This is structurally identical to the tail of `_sync_stub` — we
 * reuse the RESTORE_TRAPFRAME macro so any future change to the
 * trap-frame layout propagates here automatically.
 */
.global nx_task_fork_child_entry
nx_task_fork_child_entry:
    RESTORE_TRAPFRAME
    eret
