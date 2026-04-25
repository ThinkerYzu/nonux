/*
 * Slice 7.6c.1 — embed posix_libc_prog.elf as a byte blob in
 * kernel-test.bin's .rodata.  Same shape as the other slice-7.x
 * blob wrappers.
 */

.section .rodata
.balign 16
.global __posix_libc_prog_blob_start
.global __posix_libc_prog_blob_end

__posix_libc_prog_blob_start:
.incbin "test/kernel/posix_libc_prog.elf"
__posix_libc_prog_blob_end:
