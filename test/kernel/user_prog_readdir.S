/*
 * Slice 6.4 EL0 test program — readdir end-to-end.
 *
 * Assembled into .rodata of kernel-test.bin between the marker symbols
 * __user_readdir_prog_start and __user_readdir_prog_end.  The kernel
 * test memcopies the bytes into the MMU's user window and calls
 * drop_to_el0().  All addressing is PC-relative.
 *
 * Program flow:
 *
 *   1. NX_SYS_OPEN("/rd", WRITE|CREATE) → handle; NX_SYS_HANDLE_CLOSE.
 *      (Seeds a new file so readdir has fresh content to find.  At
 *      this point ramfs already holds /ktest_hello and /hello from
 *      slice-6.2/6.3 tests.)
 *   2. Loop: NX_SYS_READDIR(&cookie, &ent).  On NX_OK,
 *      NX_SYS_DEBUG_WRITE(&ent.name, ent.name_len) + a newline.  On
 *      NX_ENOENT exit the loop.
 *   3. NX_SYS_DEBUG_WRITE("[el0-rdr-ok]\n", 13) — visual success
 *      marker; the kernel test counts debug_write calls and asserts
 *      both the loop body ran (≥ 1 file seen) and the tail marker
 *      fired.
 *   4. wfe/b loop — park.
 *
 * Syscall numbers (must track framework/syscall.h):
 *   #1 = NX_SYS_DEBUG_WRITE
 *   #2 = NX_SYS_HANDLE_CLOSE
 *   #6 = NX_SYS_OPEN
 *   #10 = NX_SYS_READDIR
 *
 * Flag values (must track interfaces/vfs.h):
 *   2 = NX_VFS_OPEN_WRITE
 *   4 = NX_VFS_OPEN_CREATE
 *
 * Dirent layout (must track interfaces/fs.h):
 *   offset  0: uint32_t name_len
 *   offset  4: char     name[NX_FS_DIRENT_NAME_MAX]
 */

.section .rodata
.balign 16
.global __user_readdir_prog_start
.global __user_readdir_prog_end

__user_readdir_prog_start:

    /* 1a. open("/rd", WRITE|CREATE) */
    adr     x0, _seed_path
    mov     x1, #6                          /* WRITE=2 | CREATE=4 */
    mov     x8, #6                          /* NX_SYS_OPEN */
    svc     #0
    mov     x19, x0                         /* save handle */

    /* 1b. close(handle) */
    mov     x0, x19
    mov     x8, #2                          /* NX_SYS_HANDLE_CLOSE */
    svc     #0

_readdir_loop:
    /* 2. readdir(&cookie, &ent) */
    adr     x0, _cookie
    adr     x1, _ent
    mov     x8, #10                         /* NX_SYS_READDIR */
    svc     #0
    /* On NX_ENOENT (=-4), x0 == -4; on NX_OK, x0 == 0. */
    cmp     x0, #0
    bne     _readdir_loop_done

    /* 2a. debug_write(&ent.name, ent.name_len) */
    adr     x0, _ent                        /* &ent */
    ldr     w1, [x0]                        /* ent.name_len (w1 zero-extended) */
    add     x0, x0, #4                      /* &ent.name */
    mov     x8, #1                          /* NX_SYS_DEBUG_WRITE */
    svc     #0

    /* 2b. debug_write("\n", 1) — readability in the ktest log */
    adr     x0, _newline
    mov     x1, #1
    mov     x8, #1
    svc     #0

    b       _readdir_loop

_readdir_loop_done:
    /* 3. debug_write("[el0-rdr-ok]\n", 13) */
    adr     x0, _marker
    mov     x1, #(_marker_end - _marker)
    mov     x8, #1
    svc     #0

_readdir_park:
    wfe
    b       _readdir_park

.balign 8
_seed_path:
    .asciz "/rd"

.balign 8
_newline:
    .ascii "\n"

.balign 8
_marker:
    .ascii "[el0-rdr-ok]\n"
_marker_end:

.balign 8
_cookie:
    .word 0
    .word 0                                 /* padding so adjacent fields align */

/* struct nx_fs_dirent — must match interfaces/fs.h layout. */
.balign 8
_ent:
    .word 0                                 /* name_len */
    .space 64                               /* char name[NX_FS_DIRENT_NAME_MAX] */

__user_readdir_prog_end:
