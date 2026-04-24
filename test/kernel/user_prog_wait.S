/*
 * Slice 7.4b EL0 test program — fork + child exit + parent wait.
 *
 * Embedded into kernel-test.bin's .rodata between
 * __user_wait_prog_start and __user_wait_prog_end.  Program flow:
 *
 *   1. fork() via NX_SYS_FORK.
 *   2. Child branch (x0 == 0):
 *        a. debug_write("[wait-child]", 12)
 *        b. exit(42) via NX_SYS_EXIT — process-state flips to
 *           EXITED, task parks in wfe.
 *   3. Parent branch (x0 > 0):
 *        a. Save child pid in x19 (callee-saved across SVCs).
 *        b. debug_write("[wait-parent]", 13).
 *        c. Call NX_SYS_WAIT(x19, &status_buf).  Syscall yields
 *           until child's state is EXITED, then copy_to_user's
 *           the exit_code into status_buf.  Returns child's pid.
 *        d. Load status_buf.  If it equals 42, debug_write
 *           "[wait-ok]".  (The marker appearing in UART is the
 *           kernel test's proof that wait actually delivered the
 *           child's code back to user space.)
 *        e. park in wfe.
 *
 * Syscall numbers (must track framework/syscall.h):
 *   #1  = NX_SYS_DEBUG_WRITE
 *   #11 = NX_SYS_EXIT
 *   #12 = NX_SYS_FORK
 *   #13 = NX_SYS_WAIT
 */

.section .rodata
.balign 16
.global __user_wait_prog_start
.global __user_wait_prog_end

__user_wait_prog_start:

    /* fork() */
    mov     x8, #12                         /* NX_SYS_FORK */
    svc     #0
    cbz     x0, _wait_child

_wait_parent:
    mov     x19, x0                         /* save child pid */

    /* debug_write("[wait-parent]", 13) */
    adr     x0, _parent_msg
    mov     x1, #(_parent_msg_end - _parent_msg)
    mov     x8, #1                          /* NX_SYS_DEBUG_WRITE */
    svc     #0

    /* wait(x19, &_status_buf) — yields until child EXITED, then
     * copies exit_code into _status_buf and returns child pid in x0. */
    mov     x0, x19
    adr     x1, _status_buf
    mov     x8, #13                         /* NX_SYS_WAIT */
    svc     #0

    /* Load the delivered status.  Compare against 42. */
    adr     x1, _status_buf
    ldr     w2, [x1]
    cmp     w2, #42
    bne     _wait_park

    /* debug_write("[wait-ok]", 9) */
    adr     x0, _ok_msg
    mov     x1, #(_ok_msg_end - _ok_msg)
    mov     x8, #1
    svc     #0

    b       _wait_park

_wait_child:
    /* debug_write("[wait-child]", 12) */
    adr     x0, _child_msg
    mov     x1, #(_child_msg_end - _child_msg)
    mov     x8, #1
    svc     #0

    /* exit(42) */
    mov     x0, #42
    mov     x8, #11                         /* NX_SYS_EXIT */
    svc     #0
    /* unreachable — sys_exit parks the task */

_wait_park:
    wfe
    b       _wait_park

.balign 8
_parent_msg:
    .ascii "[wait-parent]"
_parent_msg_end:

.balign 8
_child_msg:
    .ascii "[wait-child]"
_child_msg_end:

.balign 8
_ok_msg:
    .ascii "[wait-ok]"
_ok_msg_end:

.balign 8
_status_buf:
    .word 0
    .word 0

__user_wait_prog_end:
