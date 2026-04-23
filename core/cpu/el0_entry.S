/*
 * drop_to_el0(pc, sp) — Phase 5 slice 5.5.
 *
 * One-way jump from EL1 kernel code to EL0 user code:
 *
 *     x0 = EL0 entry PC   (ELR_EL1)
 *     x1 = initial SP_EL0
 *
 * Programs SPSR_EL1 for EL0t (M[3:0] = 0b0000, which is EL0 using SP_EL0)
 * with IRQ + FIQ unmasked so the timer drives preemption of EL0 tasks.
 * The only way back to EL1 is via an exception (SVC, IRQ, fault).
 *
 * No caller-side return path: this function never returns to its C
 * caller.  Callers typically invoke it from a kthread whose sole job
 * is to run the EL0 program, so the kthread's kstack is effectively
 * abandoned once eret runs.  Slice 5.6+ adds proper task-exit / reap
 * semantics.
 */

.section .text
.global drop_to_el0

drop_to_el0:
    msr     sp_el0, x1               /* initial user stack */
    msr     elr_el1, x0              /* entry PC on eret */

    /* SPSR_EL1 for EL0t: bits [3:0] = 0b0000 (EL0 with SP_EL0 selected),
     * DAIF = 0b0000 (unmasked: let IRQs / SError reach the handlers
     * while EL0 is running). */
    mov     x2, #0
    msr     spsr_el1, x2

    /* Hygiene: wipe GP registers before handing control to EL0 so
     * leftover kernel pointer values don't leak out.  eret itself
     * doesn't touch x0..x30 — whatever values are live at eret is
     * what EL0 sees — so we zero everything here, including the x0 /
     * x1 inputs now that SP_EL0 / ELR_EL1 have already consumed them. */
    mov     x0,  #0
    mov     x1,  #0
    mov     x2,  #0
    mov     x3,  #0
    mov     x4,  #0
    mov     x5,  #0
    mov     x6,  #0
    mov     x7,  #0
    mov     x8,  #0
    mov     x9,  #0
    mov     x10, #0
    mov     x11, #0
    mov     x12, #0
    mov     x13, #0
    mov     x14, #0
    mov     x15, #0
    mov     x16, #0
    mov     x17, #0
    mov     x18, #0
    mov     x29, #0    /* frame pointer */
    mov     x30, #0    /* link register */

    eret
