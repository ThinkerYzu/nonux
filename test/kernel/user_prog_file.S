/*
 * Slice 6.3 EL0 test program — file syscalls end-to-end.
 *
 * Assembled into .rodata of kernel-test.bin between the marker symbols
 * __user_file_prog_start and __user_file_prog_end.  The kernel test
 * memcopies the bytes into the MMU's user window and calls
 * drop_to_el0().  All addressing is PC-relative.
 *
 * Program flow:
 *
 *   1. NX_SYS_OPEN("/hello", READ|WRITE|CREATE) → handle in x0, saved
 *      in x19 (callee-saved across the next SVCs).
 *   2. NX_SYS_WRITE(x19, "[el0-file-ok]", 13).
 *   3. NX_SYS_HANDLE_CLOSE(x19) — runs the HANDLE_FILE destructor
 *      through vfs_simple → ramfs.
 *   4. NX_SYS_OPEN("/hello", READ) → handle in x0, saved in x19.
 *   5. NX_SYS_READ(x19, recv_buf, 64).  x0 returns the byte count
 *      (13), saved in x20.
 *   6. NX_SYS_HANDLE_CLOSE(x19).
 *   7. NX_SYS_DEBUG_WRITE(recv_buf, x20) — forwards the payload to
 *      UART, bumping the debug_write counter the kernel test watches.
 *   8. wfe/b loop — rely on the scheduler to preempt us back.
 *
 * Syscall numbers (must track framework/syscall.h):
 *   #1 = NX_SYS_DEBUG_WRITE
 *   #2 = NX_SYS_HANDLE_CLOSE
 *   #6 = NX_SYS_OPEN
 *   #7 = NX_SYS_READ
 *   #8 = NX_SYS_WRITE
 *
 * Flag values (must track interfaces/vfs.h):
 *   1 = NX_VFS_OPEN_READ
 *   2 = NX_VFS_OPEN_WRITE
 *   4 = NX_VFS_OPEN_CREATE
 */

.section .rodata
.balign 16
.global __user_file_prog_start
.global __user_file_prog_end

__user_file_prog_start:

    /* 1. open("/hello", READ|WRITE|CREATE) */
    adr     x0, _path
    mov     x1, #7                          /* 1 | 2 | 4 */
    mov     x8, #6                          /* NX_SYS_OPEN */
    svc     #0
    mov     x19, x0                         /* w-handle */

    /* 2. write(w, "[el0-file-ok]", 13) */
    mov     x0, x19
    adr     x1, _msg
    mov     x2, #(_msg_end - _msg)
    mov     x8, #8                          /* NX_SYS_WRITE */
    svc     #0

    /* 3. close(w) — runs HANDLE_FILE destructor */
    mov     x0, x19
    mov     x8, #2                          /* NX_SYS_HANDLE_CLOSE */
    svc     #0

    /* 4. open("/hello", READ) */
    adr     x0, _path
    mov     x1, #1                          /* READ */
    mov     x8, #6
    svc     #0
    mov     x19, x0                         /* r-handle */

    /* 5. read(r, recv_buf, 64) */
    mov     x0, x19
    adr     x1, _recv_buf
    mov     x2, #64
    mov     x8, #7                          /* NX_SYS_READ */
    svc     #0
    mov     x20, x0                         /* byte count */

    /* 6. close(r) */
    mov     x0, x19
    mov     x8, #2
    svc     #0

    /* 7. debug_write(recv_buf, x20) — produces "[el0-file-ok]" on UART */
    adr     x0, _recv_buf
    mov     x1, x20
    mov     x8, #1                          /* NX_SYS_DEBUG_WRITE */
    svc     #0

_user_file_park:
    wfe
    b       _user_file_park

.balign 8
_path:
    .asciz "/hello"                         /* 6 chars + NUL */

.balign 8
_msg:
    .ascii "[el0-file-ok]"
_msg_end:

.balign 8
_recv_buf:
    .space 64

__user_file_prog_end:
