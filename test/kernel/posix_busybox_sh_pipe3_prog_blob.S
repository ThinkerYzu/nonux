/*
 * Slice 7.6d.N.14 — embed posix_busybox_sh_pipe3_prog.elf as a byte
 * blob in kernel-test.bin's .rodata.  Consumed by
 * ktest_posix_busybox_sh_pipe3.c.
 */

.section .rodata
.balign 16
.global __posix_busybox_sh_pipe3_prog_blob_start
.global __posix_busybox_sh_pipe3_prog_blob_end

__posix_busybox_sh_pipe3_prog_blob_start:
.incbin "test/kernel/posix_busybox_sh_pipe3_prog.elf"
__posix_busybox_sh_pipe3_prog_blob_end:
