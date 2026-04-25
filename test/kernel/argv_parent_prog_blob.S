/*
 * Slice 7.6c.4 — embed argv_parent_prog.elf as a byte blob in
 * kernel-test.bin's .rodata.  Same shape as the other slice-7.x
 * blob wrappers.
 */

.section .rodata
.balign 16
.global __argv_parent_prog_blob_start
.global __argv_parent_prog_blob_end

__argv_parent_prog_blob_start:
.incbin "test/kernel/argv_parent_prog.elf"
__argv_parent_prog_blob_end:
