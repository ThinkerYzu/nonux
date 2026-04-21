/*
 * ARM64 context switch for the nonux core scheduler driver.
 *
 *     void cpu_switch_to(struct nx_task *prev, struct nx_task *next)
 *
 *         x0 = prev  (outgoing task)
 *         x1 = next  (incoming task)
 *
 * `struct nx_task` starts with `struct nx_cpu_ctx` at offset 0 (asserted
 * in task.h), so the task pointer doubles as a context pointer.  The
 * layout inside nx_cpu_ctx is:
 *
 *     0x00: x19, x20
 *     0x10: x21, x22
 *     0x20: x23, x24
 *     0x30: x25, x26
 *     0x40: x27, x28
 *     0x50: x29 (FP), x30 (LR)
 *     0x60: SP
 *
 * Every stp/ldp pair lands on a 16-byte offset so `-mstrict-align` stays
 * valid with the MMU still off (memory is Device-typed, which forbids
 * misaligned stp/ldp).
 *
 * Caller-saved registers (x0-x18) are not saved here: cpu_switch_to is a
 * plain C call, so the AAPCS call-site has already spilled anything the
 * compiler cared about.  Floating-point / NEON state: none — kernel
 * builds with -mgeneral-regs-only.
 *
 * `nx_task_bootstrap` is the first-switch thunk.  When a task newly
 * created by nx_task_create is switched *into* for the first time, its
 * saved LR (x30) points here.  The thunk then jumps to `entry(arg)`
 * using the entry pointer stashed in x19 and arg in x20.
 */

.section .text

.global cpu_switch_to
cpu_switch_to:
    /* Save prev's context. */
    stp     x19, x20, [x0, #0x00]
    stp     x21, x22, [x0, #0x10]
    stp     x23, x24, [x0, #0x20]
    stp     x25, x26, [x0, #0x30]
    stp     x27, x28, [x0, #0x40]
    stp     x29, x30, [x0, #0x50]
    mov     x2, sp
    str     x2,       [x0, #0x60]
    mrs     x2, daif
    str     x2,       [x0, #0x68]

    /* Load next's context. */
    ldp     x19, x20, [x1, #0x00]
    ldp     x21, x22, [x1, #0x10]
    ldp     x23, x24, [x1, #0x20]
    ldp     x25, x26, [x1, #0x30]
    ldp     x27, x28, [x1, #0x40]
    ldp     x29, x30, [x1, #0x50]
    ldr     x2,       [x1, #0x60]
    mov     sp, x2
    ldr     x2,       [x1, #0x68]
    msr     daif, x2

    /* Publish the new "current task" via TPIDR_EL1.  nx_task_current()
     * reads this from both C and future exception-return paths. */
    msr     tpidr_el1, x1

    /* `ret` jumps to LR (x30), which is either:
     *   - the caller's return address (normal switch between two tasks
     *     that have both run cpu_switch_to at least once), or
     *   - nx_task_bootstrap (for a task being switched into for the
     *     first time after nx_task_create).
     */
    ret


.global nx_task_bootstrap
nx_task_bootstrap:
    /* First-switch entry: x19 holds the entry function, x20 holds arg.
     * Move arg into x0 per AAPCS, then branch to entry.  If entry ever
     * returns we fall through to the park loop — nonux tasks are not
     * supposed to return from their entry in slice 4.1 (no `exit`
     * semantics yet). */
    mov     x0, x20
    blr     x19

    /* Entry returned — park.  Slice 4.4 wires this into nx_task_exit(). */
1:  wfe
    b       1b
