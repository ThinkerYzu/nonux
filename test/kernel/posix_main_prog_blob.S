/*
 * Slice 7.6c.0 — embed posix_main_prog.elf as a byte blob in
 * kernel-test.bin's .rodata.  Same shape as init_prog_blob.S.
 */

.section .rodata
.balign 16
.global __posix_main_prog_blob_start
.global __posix_main_prog_blob_end

__posix_main_prog_blob_start:
.incbin "test/kernel/posix_main_prog.elf"
__posix_main_prog_blob_end:
