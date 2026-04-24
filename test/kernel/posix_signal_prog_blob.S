/*
 * Slice 7.5 — embed posix_signal_prog.elf as a byte blob in
 * kernel-test.bin's .rodata, flanked by `start` / `end` labels the
 * ktest references.  Same shape as init_prog_blob.S / posix_prog_blob.S.
 */

.section .rodata
.balign 16
.global __posix_signal_prog_blob_start
.global __posix_signal_prog_blob_end

__posix_signal_prog_blob_start:
.incbin "test/kernel/posix_signal_prog.elf"
__posix_signal_prog_blob_end:
