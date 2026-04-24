/*
 * Slice 7.4c EL0 test program — fork + child exec + parent wait.
 *
 * Embedded into kernel-test.bin's .rodata between
 * __user_exec_prog_start and __user_exec_prog_end.  Program flow:
 *
 *   1. fork() via NX_SYS_FORK.
 *   2. Child branch (x0 == 0):
 *        - exec("/init") via NX_SYS_EXEC — replaces the child's
 *          address space with init_prog.elf (debug_write
 *          "[el0-elf-ok]" + exit(17)).  exec returns only on
 *          failure; parent sees the child's exit via wait.
 *        - (unreached on success) exit(99) as fallback so
 *          ktest_exec's status check flags an anomaly.
 *   3. Parent branch (x0 > 0):
 *        a. save child pid in x19.
 *        b. debug_write("[exec-parent]", 13).
 *        c. wait(x19, &status_buf) — polls until child EXITED,
 *           writes exit_code to status_buf, returns child pid.
 *        d. load status_buf; compare to 17 (init_prog's exit code).
 *           If equal, debug_write "[exec-ok]".
 *        e. park.
 *
 * Ten-marker live log invariant after this program runs:
 *   [exec-parent] → [el0-elf-ok] (child's exec'd program emits) →
 *   [exec-ok].  Three debug_writes in that order (plus whatever
 *   earlier ktests produced).
 *
 * Syscall numbers (must track framework/syscall.h):
 *   #1  = NX_SYS_DEBUG_WRITE
 *   #11 = NX_SYS_EXIT
 *   #12 = NX_SYS_FORK
 *   #13 = NX_SYS_WAIT
 *   #14 = NX_SYS_EXEC
 */

.section .rodata
.balign 16
.global __user_exec_prog_start
.global __user_exec_prog_end

__user_exec_prog_start:

    /* fork() */
    mov     x8, #12                         /* NX_SYS_FORK */
    svc     #0
    cbz     x0, _exec_child

_exec_parent:
    mov     x19, x0                         /* save child pid */

    /* debug_write("[exec-parent]", 13) */
    adr     x0, _parent_msg
    mov     x1, #(_parent_msg_end - _parent_msg)
    mov     x8, #1
    svc     #0

    /* wait(child_pid, &_status_buf) */
    mov     x0, x19
    adr     x1, _status_buf
    mov     x8, #13                         /* NX_SYS_WAIT */
    svc     #0

    /* load status and compare to 17 (init_prog's exit code) */
    adr     x1, _status_buf
    ldr     w2, [x1]
    cmp     w2, #17
    bne     _exec_park

    /* debug_write("[exec-ok]", 9) */
    adr     x0, _ok_msg
    mov     x1, #(_ok_msg_end - _ok_msg)
    mov     x8, #1
    svc     #0

    b       _exec_park

_exec_child:
    /* exec("/init"). */
    adr     x0, _init_path
    mov     x8, #14                         /* NX_SYS_EXEC */
    svc     #0

    /* exec only returns on failure — fall through to a sentinel
     * exit so the parent's wait sees a distinct code. */
    mov     x0, #99
    mov     x8, #11                         /* NX_SYS_EXIT */
    svc     #0

_exec_park:
    wfe
    b       _exec_park

.balign 8
_parent_msg:
    .ascii "[exec-parent]"
_parent_msg_end:

.balign 8
_ok_msg:
    .ascii "[exec-ok]"
_ok_msg_end:

.balign 8
_init_path:
    .asciz "/init"

.balign 8
_status_buf:
    .word 0
    .word 0

__user_exec_prog_end:
