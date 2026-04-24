/*
 * Slice 7.4a EL0 test program — fork end-to-end.
 *
 * Embedded into kernel-test.bin's .rodata between
 * __user_fork_prog_start and __user_fork_prog_end.  The kernel
 * test memcopies the bytes into the parent process's user
 * window and drops to EL0 at _start.
 *
 * Program flow:
 *   1. svc #0 with x8 = NX_SYS_FORK (12).  On return, x0 is the
 *      pid of the child (parent) or 0 (child).
 *   2. Branch on x0:
 *        - x0 > 0 (parent): debug_write("[fork-parent]", 13).
 *        - x0 == 0 (child): debug_write("[fork-child]", 12).
 *      (Both branches use separate adr+svc pairs, so the parent
 *      and child emit disjoint UART bytes; the kernel test can
 *      see each marker independently in the live log.)
 *   3. Park in wfe.  The ktest harness dequeues both tasks after
 *      observing both markers.
 *
 * Address-space semantics (slice 7.2 + 7.4a):
 *   - At fork the child gets its own TTBR0 root with a byte-copy
 *     of the parent's user-window backing.  Subsequent writes by
 *     either process stay private.
 *   - Both branches reference the same PC-relative label
 *     addresses; because child == parent copy at fork time, the
 *     `.ascii` bytes at _parent_msg / _child_msg live in both
 *     address spaces at the same VAs.
 *
 * Syscall numbers (must track framework/syscall.h):
 *   #1  = NX_SYS_DEBUG_WRITE
 *   #12 = NX_SYS_FORK
 */

.section .rodata
.balign 16
.global __user_fork_prog_start
.global __user_fork_prog_end

__user_fork_prog_start:

    /* fork() */
    mov     x8, #12                         /* NX_SYS_FORK */
    svc     #0
    /* x0: child pid (parent) or 0 (child) */

    cbz     x0, _fork_child

_fork_parent:
    adr     x0, _parent_msg
    mov     x1, #(_parent_msg_end - _parent_msg)
    mov     x8, #1                          /* NX_SYS_DEBUG_WRITE */
    svc     #0
    b       _fork_park

_fork_child:
    adr     x0, _child_msg
    mov     x1, #(_child_msg_end - _child_msg)
    mov     x8, #1
    svc     #0
    /* fall through to park */

_fork_park:
    wfe
    b       _fork_park

.balign 8
_parent_msg:
    .ascii "[fork-parent]"
_parent_msg_end:

.balign 8
_child_msg:
    .ascii "[fork-child]"
_child_msg_end:

__user_fork_prog_end:
