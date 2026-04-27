/*
 * Slice 7.6d.N.6 — embed posix_busybox_sh_pipe_prog.elf as a byte
 * blob in kernel-test.bin's .rodata.  Same shape as the other
 * slice-7.x blob wrappers; consumed by ktest_posix_busybox_sh_pipe.c.
 */

.section .rodata
.balign 16
.global __posix_busybox_sh_pipe_prog_blob_start
.global __posix_busybox_sh_pipe_prog_blob_end

__posix_busybox_sh_pipe_prog_blob_start:
.incbin "test/kernel/posix_busybox_sh_pipe_prog.elf"
__posix_busybox_sh_pipe_prog_blob_end:
