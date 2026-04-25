/*
 * Slice 7.6c.3c — embed posix_musl_printf_prog.elf as a byte blob
 * in kernel-test.bin's .rodata.  Same shape as the other slice-7.x
 * blob wrappers; consumed by ktest_posix_musl_printf.c.
 */

.section .rodata
.balign 16
.global __posix_musl_printf_prog_blob_start
.global __posix_musl_printf_prog_blob_end

__posix_musl_printf_prog_blob_start:
.incbin "test/kernel/posix_musl_printf_prog.elf"
__posix_musl_printf_prog_blob_end:
