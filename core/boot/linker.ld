/* nonux linker script — ARM64 QEMU virt machine */

ENTRY(_start)

/* QEMU's `-kernel` on ARM64 injects a small bootloader at 0x40000000 and
 * loads the kernel image at RAM_BASE + 0x80000.  Matching that offset lets
 * our absolute symbol addresses line up with what the CPU actually sees. */

MEMORY
{
    RAM (rwx) : ORIGIN = 0x40080000, LENGTH = 64M
}

SECTIONS
{
    . = 0x40080000;

    .text : {
        KEEP(*(.text.boot))   /* _start must be first */
        *(.text .text.*)
    } > RAM

    .rodata : ALIGN(4096) {
        *(.rodata .rodata.*)
    } > RAM

    .data : ALIGN(4096) {
        *(.data .data.*)
    } > RAM

    .bss : ALIGN(4096) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    } > RAM

    . = ALIGN(4096);
    __kernel_end = .;

    /* Kernel stack — 256KB, grows downward */
    . = . + 0x40000;
    __stack_top = .;

    /* Everything after this is free memory for PMM */
    . = ALIGN(4096);
    __free_mem_start = .;
}
