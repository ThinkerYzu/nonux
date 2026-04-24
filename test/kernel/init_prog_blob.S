/*
 * Slice 7.3 — embed the standalone init_prog.elf as a byte blob
 * in kernel-test.bin's .rodata, flanked by `start` / `end` labels
 * the C test code can reference.  The `.incbin` directive reads
 * the file as-is (header + segments + section table) so the
 * embedded bytes are a valid ELF that `framework/elf.c`'s parser
 * can consume.
 *
 * Path is relative to where `aarch64-linux-gnu-gcc` is invoked —
 * the top-level Makefile runs from the nonux root, which is where
 * `test/kernel/init_prog.elf` lives after the build rule produces
 * it.
 */

.section .rodata
.balign 16
.global __init_prog_blob_start
.global __init_prog_blob_end

__init_prog_blob_start:
.incbin "test/kernel/init_prog.elf"
__init_prog_blob_end:
