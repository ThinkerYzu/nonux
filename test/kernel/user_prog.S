/*
 * First-EL0-process test program — slice 5.5.
 *
 * Assembled into .rodata of kernel-test.bin; the ktest code copies
 * bytes [__user_prog_start, __user_prog_end) into the MMU's user
 * window (an EL0-accessible 2 MiB block at mmu_user_window_base())
 * and then calls drop_to_el0() at that VA.
 *
 * All address computations inside this program are PC-relative
 * (`adr`, `b`) so relocating the code to the user window works
 * without a linker pass.  Immediates are assembler-time constants,
 * also relocation-safe.
 *
 * The program issues one SVC (NX_SYS_DEBUG_WRITE, syscall number 1)
 * with a test message, then falls into an indefinite `wfe; b .` loop
 * at EL0.  Timer ticks will preempt the loop and give other tasks a
 * chance to run; the kernel test observes the debug_write counter to
 * prove the program actually reached the SVC.
 */

.section .rodata
.balign 16
.global __user_prog_start
.global __user_prog_end

__user_prog_start:

    /* x0 = pointer to message (PC-relative via adr) */
    adr     x0, _user_msg
    /* x1 = len (assembler-time constant) */
    mov     x1, #(_user_msg_end - _user_msg)
    /* x8 = NX_SYS_DEBUG_WRITE (1) */
    mov     x8, #1
    svc     #0

    /* Infinite wfe loop.  wfe parks the CPU until an event (timer IRQ
     * being the reliable one); the branch re-enters the wait.  Gentle
     * on the core and keeps the EL0 task from spamming the dispatcher. */
_user_loop:
    wfe
    b       _user_loop

_user_msg:
    .ascii "[el0] hello\n"
_user_msg_end:

__user_prog_end:
