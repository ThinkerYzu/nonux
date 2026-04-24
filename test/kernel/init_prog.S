/*
 * Slice 7.3 — tiny EL0 program shipped as a standalone ELF.
 *
 * Unlike `user_prog.S` / `user_prog_chan.S` / `user_prog_file.S` /
 * `user_prog_readdir.S` (which are baked into kernel-test.bin's
 * .rodata and memcopied into the user window at ktest time),
 * `init_prog.S` is assembled + linked as its OWN aarch64 ELF
 * binary, linked at VA 0x48000000 (the user window).  The
 * slice-7.3 ELF loader reads the resulting `init_prog.elf` from
 * an in-memory blob, validates it, and copies every PT_LOAD
 * segment into a target process's address space.  The kernel
 * test (`ktest_elf.c`) then `drop_to_el0`s at the program's
 * entry point.
 *
 * Program flow:
 *   1. NX_SYS_DEBUG_WRITE("[el0-elf-ok]", 13).
 *   2. wfe park — ktest dequeues externally.
 *
 * The linker places `_start` at offset 0 of the text section, so
 * with `init_prog.ld` setting the base to 0x48000000, the entry
 * point IS 0x48000000 and execution begins immediately after the
 * 2 MiB user-window mapping resolves the first fetch.
 */

.section .text
.global _start

_start:
    /* NX_SYS_DEBUG_WRITE("[el0-elf-ok]", 13) */
    adr     x0, _marker
    mov     x1, #(_marker_end - _marker)
    mov     x8, #1                          /* NX_SYS_DEBUG_WRITE */
    svc     #0

_park:
    wfe
    b       _park

.balign 8
_marker:
    .ascii "[el0-elf-ok]"
_marker_end:
