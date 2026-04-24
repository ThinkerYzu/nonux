/*
 * Slice 7.4d — embed the standalone posix_prog.elf as a byte blob
 * in kernel-test.bin's .rodata, flanked by `start` / `end` labels
 * the C test code can reference.  Same pattern as
 * init_prog_blob.S from slice 7.3 — the `.incbin` directive
 * reads the ELF as-is so framework/elf.c's parser can consume
 * it directly.
 *
 * Path is relative to where `aarch64-linux-gnu-gcc` is invoked —
 * the top-level Makefile runs from the nonux root.
 */

.section .rodata
.balign 16
.global __posix_prog_blob_start
.global __posix_prog_blob_end

__posix_prog_blob_start:
.incbin "test/kernel/posix_prog.elf"
__posix_prog_blob_end:
