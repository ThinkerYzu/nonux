/*
 * Slice 7.6c.3c — embed musl_exec_parent_prog.elf as a byte blob in
 * kernel-test.bin's .rodata.  Same shape as the other slice-7.x
 * blob wrappers; consumed by ktest_musl_exec.c.
 */

.section .rodata
.balign 16
.global __musl_exec_parent_prog_blob_start
.global __musl_exec_parent_prog_blob_end

__musl_exec_parent_prog_blob_start:
.incbin "test/kernel/musl_exec_parent_prog.elf"
__musl_exec_parent_prog_blob_end:
