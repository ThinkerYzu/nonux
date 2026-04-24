/*
 * Linker script for the slice-7.3 init ELF.  Links everything at
 * VA 0x48000000, which matches the user window the slice-7.2 MMU
 * layer carves out per process.  One PT_LOAD segment covers .text
 * + any .rodata; ELF loader in framework/elf.c walks PT_LOADs and
 * memcopies into the target process's user backing.
 */

ENTRY(_start)

SECTIONS {
    . = 0x48000000;

    .text : {
        *(.text*)
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    .bss (NOLOAD) : {
        *(.bss*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.ARM.attributes)
        *(.eh_frame)
    }
}
