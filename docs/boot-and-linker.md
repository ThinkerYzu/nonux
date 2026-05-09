# Boot and linker — how nonux comes to life

How a freshly-powered (or freshly-`qemu-system-aarch64`-launched) machine
ends up running `boot_main()`. Concretely traces what nonux does on
ARM64 / QEMU virt, and uses that to motivate the general pieces:
bootloader, boot ROM, linker script, load address, ELF vs raw binary.

The relevant files in this repo:

- [`core/boot/start.S`](../core/boot/start.S) — first instructions executed.
- [`core/boot/boot.c`](../core/boot/boot.c) — `boot_main()`, the C entry point.
- [`core/boot/linker.ld`](../core/boot/linker.ld) — memory layout.
- [`Makefile`](../Makefile) — link + objcopy + QEMU invocation.

---

## Why a kernel needs help getting started

A program built for a hosted OS — your typical `gcc hello.c -o hello` —
relies on a long chain of services that exist before `main()` runs:

- Some other program (the OS) loaded the executable into memory.
- That program parsed the ELF header to find the entry point.
- It set up the initial stack, an environment block, argv.
- It zeroed BSS, mapped libc, ran C runtime startup (`_start` in libc),
  which eventually calls `main()`.

A kernel runs on bare metal. **Nothing exists yet.** No filesystem, no
loader, no stack, no `.bss` zeroing, often not even RAM that's been
trained by the memory controller. The CPU comes out of reset
fetching instructions from a fixed physical address with most of the
chip uninitialized.

A **bootloader** is the bridge from "CPU just woke up" to "kernel can
assume a sane environment and start its own bring-up".

Concretely a bootloader does some subset of:

1. Initialize DRAM / clock trees / on-chip peripherals enough that
   *something* can run from RAM.
2. Locate the kernel image (from flash, an SD card, a network, …).
3. Copy/decompress it to its expected load address.
4. Optionally pass parameters: a device tree, a command line.
5. Jump to the kernel's entry point in the right CPU mode.

On real ARM64 hardware this is typically a chain: a tiny boot ROM
(masked into the SoC) → a first-stage loader (`SPL`/`BL2`) →
ATF/`BL31` → U-Boot or EDK2 → Linux (or nonux). Each stage exists
because the previous one ran in a too-restrictive environment to do
the next stage's job.

For nonux on QEMU, the entire stack collapses into one trivial step,
which we explain next.

---

## QEMU's `-kernel` shortcut

The `make run` target is:

```
qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a53 \
    -nographic -kernel kernel.bin -m 1G
```

`-kernel kernel.bin` tells QEMU to act as the bootloader for us. QEMU:

1. Allocates 1 GiB of guest RAM starting at **`0x40000000`** — the
   `virt` machine's RAM base.
2. Drops a tiny built-in stub (sometimes called the "QEMU
   mini-bootloader") into the very bottom of RAM at `0x40000000`. The
   stub's job is to land the boot core at the kernel entry in the
   right state.
3. Loads `kernel.bin` at **`RAM_BASE + 0x80000`**, i.e.
   **`0x40080000`** for ARM64 `virt`. The 512 KiB gap leaves room for
   a Linux Image header, a flattened device tree, the mini-loader
   stub, and any parameter blocks.
4. Sets the boot CPU's PC to `0x40080000` (after running the stub),
   delivers `x0 = address-of-DTB` per the Linux ARM64 boot protocol,
   and starts execution.

That's the whole "bootloader" for nonux. Everything from
**`_start`** onward is our code.

To verify the load address from outside the kernel:

```
$ aarch64-linux-gnu-readelf -h kernel.elf | grep Entry
  Entry point address:               0x40080000
```

That number being `0x40080000` is not a coincidence — see "the linker
script" below.

> **Aside — real-hardware-shaped boot.** QEMU also supports `-bios`,
> which loads a binary at `0x00000000` and gives you the actual boot
> ROM seat. nonux doesn't use that path; the `-kernel` shortcut is
> sufficient because we don't model SPL → ATF → U-Boot. If we ever
> add a Linux Image header (see [HANDOFF.md](../HANDOFF.md)
> "Infrastructure polish"), `-kernel` will keep working — Linux uses
> it the same way.

---

## What the CPU sees on entry

When QEMU jumps to `0x40080000`, the boot core is in a specific state
that the kernel must respect:

| Item               | Value on QEMU virt (ARM64)              |
|--------------------|-----------------------------------------|
| Exception level    | EL2 (or EL1 if `-machine virtualization=off`) |
| AArch64?           | Yes                                     |
| MMU                | Off (VA = PA, identity)                 |
| Caches             | Off                                     |
| Interrupts         | Masked at PSTATE                        |
| `x0`               | Address of DTB (Linux ARM64 conv.)      |
| `x1` – `x3`        | Reserved / zero                         |
| Stack pointer      | Undefined — must not be used yet        |

EL ("exception level") is ARM64's privilege-ring concept: EL3 is
firmware/secure, EL2 is hypervisor, EL1 is OS kernel, EL0 is
userspace. nonux is a kernel, so it wants to run at EL1; QEMU drops
us in at EL2 to leave room for hypervisor experimentation, and we
bounce ourselves down on first instruction.

`start.S` therefore needs to do, in this order:

1. Read `CurrentEL`. If we're at EL2, drop to EL1 via `eret` after
   programming `HCR_EL2`, `SPSR_EL2`, `ELR_EL2`, `CNTHCTL_EL2`,
   `CNTVOFF_EL2`. If already at EL1, fall through.
2. Set up a stack — `sp = __stack_top` (a symbol from the linker
   script).
3. Zero `.bss` (the C standard requires zero-initialised globals).
4. Branch to the C entry point (`bl boot_main`).

That's exactly what [`core/boot/start.S`](../core/boot/start.S) does.
After step 4 the C world starts: `boot_main` brings up the MMU, PMM,
GIC, timer, framework, and (depending on build) hands off to either
the idle loop, an in-kernel test driver, or the busybox-as-init path.

> **`.text.boot`.** `_start` is in a dedicated `.text.boot` section,
> not the generic `.text`. The linker script forces `.text.boot`
> first inside `.text`, so the very first byte of `kernel.bin` is the
> first instruction of `_start`. That guarantees `0x40080000`
> resolves to "branch to the EL-detection code", regardless of what
> order the linker would have otherwise picked.

---

## The linker script — telling the linker about reality

A normal hosted program is linked against a default linker script
that targets a generic ELF executable for the platform's loader. The
loader chooses the load address dynamically (PIE), or the kernel
respects the ELF program headers' `p_vaddr` and maps pages from
disk.

A bare-metal kernel has none of that flexibility. The address the
CPU will start fetching from is **fixed by hardware/QEMU
convention**, and the kernel's symbol addresses must match. If the
linker assumes load address `0x10000` but QEMU jumps to
`0x40080000`, every absolute reference (function pointer table, jump
table, string literal) is off by 1 GiB and the kernel crashes
immediately.

The linker script `core/boot/linker.ld` declares two things:

1. The **entry point** symbol — `ENTRY(_start)`. (This sets the ELF
   header's `Elf64_Ehdr.e_entry`, which is informational for raw
   binaries but matters if you ever boot from ELF.)
2. The **memory layout** — what address each section ends up at.

Here's the relevant excerpt:

```ld
ENTRY(_start)

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
        ...
    } > RAM

    .data : ALIGN(4096) { *(.data .data.*) } > RAM

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
```

Reading top to bottom:

- **`. = 0x40080000`** — the *location counter* starts at the
  address QEMU loads us at. Every section laid down after this point
  has its symbols' absolute addresses computed from this base.
- **`.text` first, `.text.boot` first within `.text`** — guarantees
  `_start` is byte 0 of the image.
- **`.rodata` 4 KiB-aligned** — leaves room for fine-grained page
  permissions (R-only) once we have an MMU.
- **`__start_kernel_test_registry` / `__stop_kernel_test_registry`**
  and **`__start_nx_components` / `__stop_nx_components`** — these
  bracket sections that the linker itself populates. Each in-kernel
  test (`KTEST(...)` macro) and each component
  (`NX_COMPONENT_REGISTER(...)` macro) emits a const descriptor into
  one of these sections. At boot, [`framework/bootstrap.c`](../framework/bootstrap.c)
  walks `[__start_nx_components, __stop_nx_components)` to discover
  every component compiled into the image.
- **`.bss`** — zero-initialised data; `start.S` zeroes it before C
  code runs.
- **`__kernel_end`, `__stack_top`, `__free_mem_start`** — symbols
  the kernel reads at runtime to know where it ends and free RAM
  begins. The PMM in [`core/pmm/`](../core/pmm/) starts giving out
  pages from `__free_mem_start`.

The contract between the linker script and the rest of the kernel is:

| Symbol                  | Producer       | Consumer                              |
|-------------------------|----------------|---------------------------------------|
| `_start`                | `start.S`      | linker (entry), QEMU (PC at boot)     |
| `__bss_start`/`__bss_end` | linker.ld    | `start.S` (BSS-zeroing loop)          |
| `__stack_top`           | linker.ld      | `start.S` (initial SP)                |
| `__kernel_end`          | linker.ld      | informational (boot.c log)            |
| `__free_mem_start`      | linker.ld      | `boot.c` (`pmm_init` base)            |
| `__start_nx_components` | linker.ld      | `framework/bootstrap.c`               |

---

## ELF vs raw binary — `objcopy` and why

`ld -T linker.ld` produces `kernel.elf`. ELF is structured:
sections, segments, symbol table, relocation tables, ELF header.
QEMU's `-kernel` mode for ARM64 expects a **flat binary** — no ELF
header, no padding for sections that start at non-`0` offsets — just
"the bytes that go into RAM at the load address, in order".

`objcopy -O binary` strips the ELF wrapping and emits exactly that:

```
$ aarch64-linux-gnu-objcopy -O binary kernel.elf kernel.bin
$ file kernel.elf kernel.bin
kernel.elf: ELF 64-bit LSB executable, ARM aarch64, ...
kernel.bin: data
```

Mental model:

- `kernel.elf` is for tools (GDB, `readelf`, `nm`, `objdump`). It
  carries debug info, symbol table, section structure.
- `kernel.bin` is for QEMU. Byte 0 of the file is what lands at
  `0x40080000` in guest RAM.

`make debug` runs QEMU with `-s -S` (gdbstub on port 1234, halt at
boot), and the user attaches GDB to `kernel.elf` for symbols. Both
files describe the same code at the same addresses; only the
packaging differs.

---

## Putting it together — the boot timeline

```
QEMU starts
    │
    ▼
QEMU mini-loader runs at 0x40000000:
    sets x0 = DTB addr, jumps to 0x40080000
    │
    ▼
0x40080000: _start (start.S)
    │
    ├── EL2 → drop to EL1 via eret
    ├── set sp = __stack_top
    ├── zero [.bss_start, .bss_end)
    └── bl boot_main
        │
        ▼
boot.c: boot_main()
    │
    ├── uart_init()                              [Phase 1]
    ├── mmu_init()                               [slice 5.1]
    ├── pmm_init(__free_mem_start, RAM_END)
    ├── pmm_reserve_range(user-window VA range)
    ├── vectors_install()                        [EL1 vector table]
    ├── gic_init()                               [interrupt controller]
    ├── timer_init(10)                           [10 Hz tick]
    ├── nx_console_init()                        [PL011 RX ISR]
    ├── nx_framework_bootstrap()                 [walk nx_components]
    ├── sched_start()                            [boot ctx → idle task]
    ├── irq_enable_local()
    │
    └── one of:
        - idle loop (production)
        - ktest_main() (kernel test build)
        - nx_init_busybox_main() (interactive shell build)
```

By the time `nx_framework_bootstrap()` runs we're back in normal C-land:
the MMU is on, allocator works, the registry is populated from objects
the linker dropped into `nx_components`, and the framework can go from
"image of bytes in RAM" to "running composition with seven slots
filled in topo order".

---

## Variants and side notes

- **Three kernel binaries.** The same `linker.ld` is used for
  `kernel.bin`, `kernel-test.bin`, and `kernel-busybox.bin`. They
  differ in which `.o` files get linked in and which preprocessor
  flag (`-DNX_KTEST` / `-DNX_INIT_BUSYBOX`) selects the post-bring-up
  path in `boot_main`. The `kernel_test_registry` section in
  `linker.ld` is only populated in the test build (the `KTEST(...)`
  macro is gated on `NX_KTEST`), but the section markers exist
  unconditionally so production builds don't need a different script.
- **`MEMORY` block of 64 MiB.** Even though QEMU has 1 GiB of RAM,
  the linker script declares only 64 MiB available to the kernel
  image. The PMM owns the rest at runtime; the linker just needs
  enough room for code/data and the static stack. Increasing this is
  trivial if the kernel ever grows past 64 MiB of static .text +
  .rodata + .data + stack.
- **No PIE, no relocations.** The kernel is linked at its absolute
  load address. Every symbol reference resolves at link time.
  `kernel.bin` has no relocation entries; what you build is what runs.
- **Why `_start` doesn't return.** `start.S` calls `boot_main` via
  `bl`. If `boot_main` ever returned, LR would point back into
  `start.S`'s `hang:` — which is `wfe; b hang`. In practice
  `boot_main` does its own forever-loop (`wfi`) so we never return.
- **`__attribute__((section("nx_components")))`.** The bootstrap
  story relies on the linker stitching every component's static
  descriptor into one contiguous range. The component-side magic is
  the `NX_COMPONENT_REGISTER` macro, which expands to a
  section-attributed `static const struct nx_component_descriptor`
  variable. The linker script's `KEEP(*(nx_components))` makes sure
  the section isn't garbage-collected by `--gc-sections` (we don't
  pass that flag, but `KEEP` is cheap insurance).

---

## See also

- [`framework-bootstrap.md`](framework-bootstrap.md) — what
  `nx_framework_bootstrap()` does with the components found in the
  `nx_components` section.
- [`../core/README.md`](../core/README.md) — the rest of `core/`,
  including `mmu/`, `pmm/`, `irq/`, `timer/`, `sched/`, which all
  come up in the order shown above.
- [`../README.md`](../README.md) — project-level overview and how to
  run `make run`.
- [Linux Documentation/arm64/booting.rst](https://www.kernel.org/doc/html/latest/arch/arm64/booting.html)
  — the AArch64 boot protocol that QEMU's `-kernel` mode emulates.
- ARM Architecture Reference Manual §D — exception levels,
  `CurrentEL`, `eret`, the registers `start.S` programs to drop from
  EL2 to EL1.
