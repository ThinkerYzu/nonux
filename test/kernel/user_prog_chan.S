/*
 * Slice 5.6 EL0 test program — channels end-to-end.
 *
 * Assembled into .rodata of kernel-test.bin between the marker symbols
 * __user_chan_prog_start and __user_chan_prog_end.  The kernel test
 * memcopies the bytes into the MMU's user window and calls
 * drop_to_el0().  All addressing is PC-relative so the program
 * functions correctly from its relocated VA.
 *
 * Program flow:
 *
 *   1. NX_SYS_CHANNEL_CREATE(&end0, &end1) — kernel returns two
 *      handle IDs via copy_to_user, so end0_store and end1_store
 *      (both inside the user window) get the fresh handles.
 *   2. NX_SYS_CHANNEL_SEND(end0, "el0-chan-ok", 11) — the bytes
 *      land in end1's inbox.
 *   3. NX_SYS_CHANNEL_RECV(end1, recv_buf, cap=64) — kernel pops
 *      the message and copies it back out.  x0 returns the byte
 *      count (should be 11).
 *   4. NX_SYS_DEBUG_WRITE(recv_buf, bytes_received) — forwards
 *      the received payload to UART, incrementing the debug_write
 *      counter the kernel test watches.
 *   5. wfe/b loop forever — the kernel test dequeues this task
 *      after it's observed the counter + a unique end-to-end byte
 *      pattern in the channel payload.
 *
 * Syscall numbers (must track framework/syscall.h):
 *   #1 = NX_SYS_DEBUG_WRITE
 *   #3 = NX_SYS_CHANNEL_CREATE
 *   #4 = NX_SYS_CHANNEL_SEND
 *   #5 = NX_SYS_CHANNEL_RECV
 */

.section .rodata
.balign 16
.global __user_chan_prog_start
.global __user_chan_prog_end

__user_chan_prog_start:

    /* x0 = &end0_store, x1 = &end1_store, x8 = CHANNEL_CREATE */
    adr     x0, _end0_store
    adr     x1, _end1_store
    mov     x8, #3
    svc     #0

    /* x0 = end0 handle, x1 = &msg, x2 = len, x8 = CHANNEL_SEND */
    ldr     w0, _end0_store
    adr     x1, _chan_msg
    mov     x2, #(_chan_msg_end - _chan_msg)
    mov     x8, #4
    svc     #0

    /* x0 = end1 handle, x1 = &recv_buf, x2 = cap, x8 = CHANNEL_RECV.
     * On return x0 = bytes received. */
    ldr     w0, _end1_store
    adr     x1, _recv_buf
    mov     x2, #64
    mov     x8, #5
    svc     #0
    /* Save received byte count in x19 (callee-saved: survives the
     * next syscall's register clobbering). */
    mov     x19, x0

    /* NX_SYS_DEBUG_WRITE(recv_buf, x19). */
    adr     x0, _recv_buf
    mov     x1, x19
    mov     x8, #1
    svc     #0

    /* Park — rely on timer ticks to preempt us back to the kernel
     * test so it can observe the syscall counter. */
_user_chan_park:
    wfe
    b       _user_chan_park

.balign 8
_end0_store:
    .word 0
    .word 0
_end1_store:
    .word 0
    .word 0

_chan_msg:
    .ascii "[el0-chan-ok]"
_chan_msg_end:

.balign 8
_recv_buf:
    .space 64

__user_chan_prog_end:
