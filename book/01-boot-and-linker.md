# Boot and linker — how nonux comes to life

Imagine turning on a computer. The screen flickers. After a few
seconds, you see a login screen. **A lot happened in between.**

This doc walks through the very beginning of that "in between" — the
moment the CPU wakes up and starts running our kernel. We'll explain
what a *bootloader* does, why we need one, what a *linker script* is,
where the kernel ends up in memory, and how all the pieces fit
together.

This is written for people who are looking inside a kernel for the
**first time**. We'll explain every new word as we go. If you forget
what something means later, scroll back to the "Terms you'll see"
section.

The files we'll be talking about:

- [`core/boot/start.S`](../core/boot/start.S) — the very first
  instructions our kernel runs, written in assembly.
- [`core/boot/boot.c`](../core/boot/boot.c) — `boot_main()`, where
  things switch over to C code.
- [`core/boot/linker.ld`](../core/boot/linker.ld) — a small text
  file that tells the linker where everything goes.
- [`Makefile`](../Makefile) — the build recipe.

---

## A quick analogy first

Think of starting a computer like **opening a restaurant** in the
morning.

When the doors aren't open yet, nothing works. The lights are off,
the stove is cold, the coffee machine is unplugged, no one has
counted the cash drawer, the chairs are stacked on the tables.
Customers can't just walk in and order — somebody has to come in
early and *set things up*.

A **bootloader** is the person who comes in early. It turns on the
power, fires up the equipment, sets out the supplies, and only
*then* lets the kernel — the cook — start working. The kernel
expects the kitchen to be ready when it shows up. If a bootloader
hasn't done its job, the kernel can't even take a first step.

The rest of this doc explains, in detail, what "setting up the
kitchen" means for a real computer.

---

## Terms you'll see

These words come up over and over. Here's a plain-English version
of each. You don't need to memorize them — come back here whenever
something feels confusing.

- **CPU.** The chip that executes program instructions.
- **RAM.** The computer's main memory. Code and data live here
  while the computer is running. When you turn the power off, RAM
  forgets everything.
- **Address.** A number that names a spot in memory. Like a house
  number on a long street: address 100 is one byte, address 101 is
  the next byte, and so on.
- **Register.** A tiny storage slot *inside* the CPU itself.
  Faster than RAM, but there are only a few dozen of them. The CPU
  uses them as scratch space.
- **Kernel.** The core program of an operating system. It's the
  thing that talks to the hardware, runs your other programs, and
  shares the CPU between them. nonux is a kernel.
- **Operating system (OS).** The big software bundle that includes
  the kernel plus all the standard programs (shell, file utilities,
  drivers, libraries). Linux, Windows, macOS are operating systems.
- **Bare metal.** Code running *directly* on the hardware, with no
  operating system underneath it. A kernel runs on bare metal —
  *it* is the operating system.
- **Hosted program.** The opposite: a program that runs *on top of*
  an operating system. Your `hello.c` is a hosted program — it
  asks the OS to print to the screen, the OS deals with the
  hardware. Bare-metal code can't do that. There's no one to ask.
- **ELF.** Short for "Executable and Linkable Format". The
  standard format Linux uses for executable files. An ELF file
  contains the program's code and data, plus an **ELF header** at
  the start that describes the rest of the file: what type of file
  it is, what CPU it's for, where the program's entry point is,
  and how each piece should be loaded into memory. Tools like GDB
  (the debugger) and the OS loader read the header to figure out
  what to do with the file.
- **Raw binary (or flat binary).** Just the bytes that go into
  memory, with no ELF header and no metadata around them. The OS
  loader can't make sense of a raw binary on its own (there's no
  header to read), but it works for us because QEMU just loads the
  bytes at a fixed address — no parsing needed.
- **Section.** Inside an ELF file, the code and data are grouped
  into named *sections*. The most common ones:
  - `.text` — the program's machine instructions.
  - `.rodata` — read-only data (like the text "Hello, world!").
  - `.data` — read/write data with starting values.
  - `.bss` — read/write data that *starts at zero*. The file
    doesn't actually store the zeros (no point), it just says "set
    aside this many bytes of zero". Someone has to actually write
    the zeros at runtime.
- **BSS.** That last section above. The name is a leftover from a
  1950s-era assembler ("Block Started by Symbol"); modern people
  just say "the BSS section, where the zero-initialised globals
  live".
- **Stack.** A region of memory the CPU uses to keep track of
  function calls and local variables. Think of a pile of plates:
  every time you call a function, you put a plate on top with that
  function's data; when the function returns, you take the plate
  off. The stack pointer (SP) is a CPU register that always
  remembers "where the top plate is right now".
- **Linker.** A tool (called `ld`) that **glues together** the
  pieces of code your compiler produced (`.o` files) into one
  executable. It also picks the *address* each piece will live at.
- **Linker script.** A small text file that tells the linker how
  you want things laid out: "put `.text` here, then `.rodata`,
  start the whole thing at address X". Most programs don't need
  one — the linker has good defaults. A bare-metal kernel **does**
  need one, because the address has to match the hardware exactly.
- **`objcopy`.** A tool that copies a file from one format to
  another. We use it to take an ELF file and shake out the raw
  bytes inside.
- **Bootloader.** A program that runs *before* the kernel and gets
  the machine into a state where the kernel can actually start.
  Real computers usually run a chain of bootloaders, each setting
  up enough of the system for the next one. QEMU has a tiny
  built-in one we'll meet shortly.
- **Boot ROM.** A small program **built into the chip itself** at
  the factory. It's the first code the CPU runs after power-on.
  Its only job is to find and load the next program — usually
  reading it from flash storage on the same board.
- **MMU (Memory Management Unit).** Hardware inside the CPU that
  translates *virtual* addresses (what programs use) into
  *physical* addresses (what RAM actually sees). It also enforces
  permissions (read-only, no-execute, etc.). When the kernel first
  starts, the MMU is **off**, which means every address is just a
  raw RAM address.
- **PMM (Physical Memory Manager).** A piece of the kernel that
  hands out RAM in 4 KiB chunks (called *pages*) to whoever needs
  it. Lives in [`core/pmm/`](../core/pmm/).
- **GIC (Generic Interrupt Controller).** Hardware on ARM systems
  that listens for interrupt signals from devices and forwards them
  to the CPU.
- **Interrupt (IRQ).** A signal from a device telling the CPU
  "stop what you're doing, something needs attention" — for
  example, "a key was pressed" or "the timer ticked". The CPU
  pauses what it was doing, runs a short handler function, and
  then resumes. IRQ stands for "Interrupt Request"; ISR stands for
  "Interrupt Service Routine" (the handler function).
- **UART / PL011.** A UART is an old, simple kind of serial port —
  a one-byte-at-a-time text channel. PL011 is the specific UART
  model in QEMU's `virt` machine. We use it as our console (so
  `kprintf` text shows up on your terminal).
- **DTB (Device Tree Blob).** A file the bootloader hands the
  kernel that describes the hardware: "this board has 1 GiB of
  RAM, a UART at address X, an interrupt controller at address Y".
  Linux uses it heavily; nonux currently ignores it and hardcodes
  what it needs.
- **Exception level (EL0–EL3).** ARM64's way of organizing
  privilege. Higher number = more powerful:
  - **EL0** is for normal user programs.
  - **EL1** is for the kernel.
  - **EL2** is for hypervisors (programs that run kernels).
  - **EL3** is for the most trusted firmware.
  nonux runs at EL1.
- **Symbol.** A *name* in an executable. Every function and global
  variable is a symbol. The linker can also create symbols out of
  thin air from its script (we'll do this for `__bss_start`,
  `__stack_top`, and friends).

If a term shows up later that we didn't list here, we'll define it
the first time it appears.

---

## Compiler, linker, loader, and sections

Before we can talk about what's special about a kernel, it helps to
see how a normal program gets built and run on a regular computer.
**Three tools** do the work, in order: the compiler, the linker, and
the loader. They produce and consume files that are organized into
**sections**.

### 1. The compiler — source code to object files

The **compiler** reads your `.c` source files and produces **object
files** (`.o`). For each source file, you get one object file. Each
object file contains:

- Machine code for the functions defined in that source file.
- Space for global variables defined there.
- A list of names this file *provides* (the functions and globals
  it defines).
- A list of names this file *uses but doesn't define* — calls to
  functions defined elsewhere, like `printf`.

The compiler doesn't yet know *where* in memory anything will live.
It leaves placeholders that say "I need to call `printf` here, but
I don't know `printf`'s address yet — fill it in later".

Each `.o` is incomplete on its own. You can't run one.

### 2. The linker — object files to one executable

The **linker** (a tool called `ld`) takes a bunch of object files
(plus any libraries you depend on) and **combines them into a
single executable file**. Its main jobs are:

- **Match every "use" with a "definition".** If `hello.o` uses
  `printf`, the linker finds `printf` in the C library and
  connects the two.
- **Pick an address for each function and variable.** This is
  where "what address will `main` end up at?" finally gets
  answered.
- **Fill in every placeholder** the compiler left behind with real
  addresses.
- **Group everything by section** (covered just below) and produce
  one ELF file as output.

If some name is *used* but never *defined* anywhere, the linker
gives up with the famous error `undefined reference to ...`.

The linker has built-in default rules for picking addresses, and
for most programs those defaults are fine. For a bare-metal kernel
we override the defaults with a **linker script** (covered later in
this doc).

### 3. The loader — executable to running program

When you type `./hello` and press Enter, *something* has to read
the executable file from disk and turn it into a running program.
That something is the **loader**, and on Linux it lives inside the
OS kernel itself.

The loader:

- Opens the executable file.
- Reads the ELF header to find out what's inside.
- Copies each piece of the file into memory at the address the
  linker picked.
- Sets the right memory permissions on each region (code is
  read+execute, read-only data is read-only, etc.).
- Allocates a stack.
- Fills in `argc`, `argv`, and environment variables.
- Zeros the BSS section.
- Jumps to the program's entry point.

**This is where a kernel is different.** A kernel runs on bare
metal — no operating system underneath. **So there is no loader.**
Whoever puts the kernel into memory (on real hardware: a bootloader;
in our case: QEMU's `-kernel` mode) does the bare minimum — drops
the bytes at a fixed address and jumps in. There's nobody to read
ELF headers, nobody to set up a stack, nobody to zero the BSS. The
kernel has to do all of that itself, which is exactly what
`start.S` does.

That's also why we strip the ELF away with `objcopy` and feed QEMU
a raw binary: there's nothing reading ELF metadata, so carrying it
around is just dead weight.

### Sections

The linker groups code and data into named **sections**. Each
section has a conventional name and a clear purpose. The four
you'll see most often:

| Section   | What's in it                                       | Read | Write | Execute |
|-----------|----------------------------------------------------|------|-------|---------|
| `.text`   | The compiled code (machine instructions).          | yes  | no    | yes     |
| `.rodata` | Read-only data. String literals, `const` arrays.   | yes  | no    | no      |
| `.data`   | Read/write globals with starting values.           | yes  | yes   | no      |
| `.bss`    | Read/write globals that start at zero.             | yes  | yes   | no      |

**Why split things up?** Because once memory permissions get set,
they apply to chunks of memory at a time (typically 4 KiB chunks
called *pages*). By keeping code in its own section separate from
data, we can give each chunk just the right permissions:

- Code pages: **read+execute, no-write** — so a bug can't
  overwrite running code.
- Read-only data pages: **read-only** — so a bug can't modify a
  string literal or a `const` table.
- Read/write data pages: **read+write, no-execute** — so an
  attacker who tricks the program into writing data into the wrong
  place can't trick the CPU into running that data as code.

Without the section split, every chunk would have to use the most
permissive permissions, and these protections wouldn't be possible.

A few more facts about sections that'll matter later:

- **The compiler decides which section each piece goes into.**
  Functions go to `.text`, string literals to `.rodata`,
  initialised globals to `.data`, zero-initialised globals to
  `.bss`. You can override this with
  `__attribute__((section("name")))` in C — and we do exactly that
  for the kernel's `nx_components` section, which is how
  components register themselves automatically.
- **The linker collects same-named sections from every object
  file** into one continuous block. So all of `.text` from across
  the whole kernel ends up as one big contiguous run, same for
  `.rodata`, `.data`, and `.bss`.
- **The linker script** (later in this doc) controls *where in
  memory* each section ends up and what's aligned to what.

With this background, the rest of the doc should be a much easier
read.

---

## Why a kernel can't just "run"

When you write a normal program — let's say a tiny `hello.c`:

```c
#include <stdio.h>
int main(void) { printf("Hello!\n"); return 0; }
```

…you compile it, you run it, and "Hello!" appears. Easy.

But behind the scenes, **a huge amount of work happened before
your `main()` was called**. Most of it was the OS's loader (which
we just met in the previous section) with help from the C library:

1. It opened the executable file from disk.
2. It read the **ELF header** to find your program's entry point
   and to figure out which parts of the file to load into memory.
3. It loaded each piece of your program into memory at the right
   addresses, with the right permissions (code can be run,
   read-only data can't be written, etc.).
4. It set up a **stack** for you.
5. It put `argc` and `argv` (the command-line arguments) on that
   stack.
6. It zeroed your BSS section.
7. It loaded any libraries your program needs (like the C
   library — `libc` — which contains `printf`).
8. It ran the C library's setup code, which got `printf` ready.
9. *Finally*, after all that, it called your `main()`.

That's the **hosted world**. Easy because someone else (the OS) did
the hard parts.

A kernel is different. **A kernel runs on bare metal.** When the
kernel starts, **none of those steps have happened yet**. There's
no operating system underneath to do them — *the kernel is the
operating system*.

When the CPU first wakes up:

- There's **no filesystem** to load programs from.
- There's **no loader** to read ELF files.
- There's **no stack** ready for us. The stack pointer holds
  garbage.
- The **BSS isn't zero**. It's whatever leftover values were in RAM.
- The **memory controller might not even be running yet**, meaning
  RAM may not work until something configures it.
- The CPU is fetching instructions from a hardware-fixed address.
  We don't get to choose where it starts looking.

This is what a **bootloader** is for. A bootloader's job is to
turn this raw, useless state into something a kernel can actually
work with. Specifically:

1. **Wake up the basics.** Get RAM working. Start the clocks. Bring
   up enough of the system to read storage.
2. **Find the kernel.** Pull it from flash, an SD card, the
   network — wherever it lives.
3. **Place it in memory.** Copy it to the address the kernel was
   built for. (And maybe decompress it on the way — kernel images
   are often stored squished.)
4. **Pass useful info.** Hand the kernel a device tree (so it
   knows what hardware is around), and maybe a command line.
5. **Jump to the kernel** in the right CPU mode.

On real ARM64 hardware, this is usually a *chain* of small
bootloaders, each doing a little more than the last. A simplified
chain looks like:

```
  Boot ROM in the chip
       ↓ loads
  SPL or BL2 (small first-stage loader)
       ↓ loads
  ATF / BL31 (ARM Trusted Firmware)
       ↓ loads
  U-Boot or EDK2 (full-featured loader)
       ↓ loads
  The kernel
```

Each step exists because the previous one was too small or too
limited. The boot ROM might only have a few kilobytes of working
memory — barely enough to load the next stage. The next stage gets
DRAM working, so it has *megabytes* to play with, and can load a
bigger and smarter loader. And so on.

For nonux on QEMU, we don't deal with any of that chain. **QEMU
plays the part of the bootloader for us**, in one trivial step.
We'll see how next.

---

## How QEMU hands the kernel a "ready" machine

The command we use to run the kernel (the `make run` rule) is:

```
qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a53 \
    -nographic -kernel kernel.bin -m 1G
```

The important bit is `-kernel kernel.bin`. This tells QEMU **"act
as the bootloader for me"**. Here's what QEMU does when it sees
that:

1. **Creates 1 GiB of RAM** (because of `-m 1G`) starting at
   address `0x40000000`. Why that exact address? Because the QEMU
   `virt` machine — and most real ARM64 boards — happen to put RAM
   there. It's a convention.

2. **Drops a tiny built-in stub** at the bottom of RAM (around
   `0x40000000`). This is sometimes called the "QEMU
   mini-bootloader". Its only job is to set up the CPU's registers
   the way the kernel expects, then jump to the kernel.

3. **Loads our `kernel.bin` at address `0x40080000`** — that is,
   `RAM_BASE + 0x80000`. The 512 KiB gap between RAM start and
   kernel start is reserved by the ARM64 boot protocol — it leaves
   room for the mini-bootloader, the device tree blob, and a small
   header.

4. **Jumps to the kernel.** The boot CPU's program counter (PC)
   gets set to `0x40080000`, register `x0` gets set to the address
   of the device tree blob, and the CPU starts running.

That's the whole "bootloader" for nonux. From byte 0 of
`kernel.bin` onward — that is, from address `0x40080000` onward —
**every instruction is ours**.

You can double-check the entry address from outside the kernel
using `readelf`, a tool that prints information from ELF files:

```
$ aarch64-linux-gnu-readelf -h kernel.elf | grep Entry
  Entry point address:               0x40080000
```

That number, `0x40080000`, is not a coincidence. We chose it on
purpose in our linker script (next section).

---

## Privilege levels: EL0, EL1, EL2, and EL3

Before we look at what `start.S` does, we need to understand one
piece of ARM64 background: **exception levels**. The very first
thing `start.S` does involves switching between two of them, so
let's make sure we know what they are.

### Why CPUs have privilege levels at all

The CPU runs all code by executing instructions one at a time.
Some of those instructions are *dangerous*: writing to a register
that controls the MMU, masking interrupts so nothing can preempt
you, or talking directly to a hardware device. If any random
program could run those instructions, one buggy or malicious app
could take over the whole machine.

The fix that every modern CPU uses: **mark some instructions as
only working in a special trusted mode.** Code in user mode
either can't run those instructions at all, or the CPU catches
the attempt and reports it. Code in kernel mode can run them.

ARM64 calls these modes **exception levels** — abbreviated EL —
and defines **four** of them, numbered 0 through 3.

### What each EL is for

| EL  | Who runs here                                       | What's allowed |
|-----|-----------------------------------------------------|----------------|
| EL0 | User programs (shells, editors, the apps you write) | Normal arithmetic and memory access. **Not** allowed to touch system registers, MMU controls, or talk directly to hardware. |
| EL1 | The kernel                                          | Full control of memory mapping, peripherals, and interrupt handling. |
| EL2 | Hypervisors (KVM, Xen, …)                           | Can virtualize EL1 itself: run a whole kernel as a "guest" and intercept what it does. |
| EL3 | Secure firmware (TrustZone)                         | The most trusted code on the chip. Decides which code can run at the lower levels. |

Higher number = more powerful. Code at a higher level can do
anything code at a lower level can do, plus more.

### Why four levels?

x86 calls these **rings** and historically used rings 0 (kernel)
and 3 (user) — two levels was enough. ARM64 has four because
modern ARM chips support a wider mix of use cases:

- **EL3** is the supervisor for the chip's secure side, used for
  things like crypto keys, biometric data, or DRM.
- **EL2** is for virtualization. A hypervisor at EL2 can run
  multiple guest kernels at EL1 underneath, transparently.
- **EL1** is "kernel mode" in the traditional sense. nonux lives
  here.
- **EL0** is "user mode" in the traditional sense.

A simple system can stay at EL1 and EL0 and ignore the others.
The hardware just makes EL2 and EL3 *available* for systems that
need them.

### Where nonux fits

**nonux runs at EL1.** It's a kernel — not a hypervisor, not
firmware.

When QEMU starts our kernel on the `virt` machine, though, it
drops us in at **EL2** by default. That's because QEMU is set up
to allow hypervisor experiments. Since we're not a hypervisor,
the very first thing [`core/boot/start.S`](../core/boot/start.S)
does is **drop down from EL2 to EL1**.

(If QEMU was started with `-machine virtualization=off`, we'd
land directly at EL1 and the drop-down step wouldn't be needed.
`start.S` handles both — it reads the `CurrentEL` system register
to see where it actually is, and acts accordingly.)

### How transitions between ELs happen

**Higher to lower is voluntary, one instruction.** The
instruction is `eret` ("exception return"). Despite the name,
it's not just for returning from an exception — it's the official
way to switch from a higher EL to a lower one. We'll see this in
`start.S`: the code sets up where it wants to land, then runs
`eret` to drop from EL2 into EL1.

**Lower to higher is not voluntary.** Code at EL0 can't just
decide to start running at EL1. The only way up is through an
**exception** — an interrupt, a system call (the `svc`
instruction), a memory fault, a divide-by-zero, or similar. The
CPU automatically jumps to a fixed handler address at the higher
EL, and the handler decides what to do.

This asymmetry is the security model. Programs at EL0 can't
escalate themselves on their own; they can only ask the kernel
(at EL1) to do things on their behalf, via syscalls. The kernel
decides what to allow.

### What this means for nonux in practice

Three things follow from the EL design:

1. **`start.S` has to drop from EL2 to EL1** if it starts at EL2.
   That's the first half of `start.S`'s job, which we'll see in
   detail next.
2. **Privileged instructions only work at EL1 (or higher).** The
   kernel can program the MMU; user programs can't even attempt
   to. The CPU enforces this.
3. **User programs run at EL0.** When nonux launches a process,
   it sets the CPU to EL0 before jumping to the program's entry
   point. The program can only do things that work at EL0;
   anything that needs the kernel (open a file, allocate memory,
   …) goes through a syscall, which traps up to EL1, runs the
   kernel's syscall handler, and returns to EL0.

Future chapters will cover the syscall round-trip in detail.

---

## What the CPU is doing the moment our code starts

When QEMU jumps to `0x40080000`, the CPU is in a particular state.
Knowing this state matters, because our first instructions have to
*react* to it.

| Thing                  | What it is at boot                                     |
|------------------------|--------------------------------------------------------|
| Exception level        | EL2 (or EL1 if QEMU was started with virt off)         |
| CPU mode               | AArch64 (64-bit ARM)                                   |
| MMU                    | Off — every address is a raw RAM address               |
| Caches                 | Off                                                    |
| Interrupts             | Disabled                                               |
| Register `x0`          | Address of the DTB                                     |
| Registers `x1`–`x3`    | Reserved (zero)                                        |
| Stack pointer (SP)     | **Garbage — do not touch yet!**                        |

Two things to notice:

- **We start at EL2 but want to run at EL1** (covered in the
  previous section). Our first instructions will use `eret` to
  drop from EL2 to EL1.
- **We have no stack yet.** Until we set the stack pointer to a
  valid address, we **cannot call any C function**, because every
  C function call uses the stack to save its return address.

So our very first job — before any of the kernel's interesting
parts — is to set up the bare minimum so that C code can run.

This is what [`core/boot/start.S`](../core/boot/start.S) does, in
*assembly* (not C, because we have no stack yet). It does these
four things, in order:

1. **Check what exception level we're at.** Read the `CurrentEL`
   register. If it says EL2, set up some EL2 system registers and
   use `eret` to drop to EL1. If it already says EL1, just keep
   going.
2. **Set up a stack.** Load the stack pointer with the address of
   `__stack_top`. (`__stack_top` is a name that the linker script
   will fill in for us — more on that very soon.) ARM64 stacks
   *grow downward*, so the SP starts at the top and shrinks as
   things get pushed.
3. **Zero the BSS.** Loop from `__bss_start` to `__bss_end` (also
   linker-script names) and store zero into every word. The C
   language requires global variables to start at zero; on bare
   metal, *we* are the only one who can make that promise true.
4. **Call the C entry point.** Branch to `boot_main`. From here on,
   we're running C code.

After step 4, we're in C land. `boot_main()` then brings up the
MMU, the PMM, the GIC, the timer, the component framework, and
finally hands off to either an idle loop, the in-kernel test
runner, or an interactive shell — depending on which build we
chose.

> **A small but important detail: `.text.boot`.** `_start` lives in
> a section called `.text.boot`, separate from the regular `.text`
> section that holds the rest of the kernel's code. Why? Because
> the linker script puts `.text.boot` *first* — before any other
> code. That guarantees the very first byte of `kernel.bin` is the
> first instruction of `_start`. So when QEMU jumps to
> `0x40080000`, it lands exactly on "check the exception level".

---

## The linker script — telling the linker "put this here"

Time to talk about the file that ties all of this together:
[`core/boot/linker.ld`](../core/boot/linker.ld).

A *linker script* is a text file written in a small mini-language
that tells the linker how to lay out your program. You don't need
one for normal programs — the linker has built-in defaults that
work fine. But for a kernel, **the address has to match the
hardware exactly**, and that's exactly what a linker script lets
us control.

Why does the address matter so much? Because once the linker
chooses an address for, say, the function `boot_main`, that
address gets *baked into every place that calls it*. If our linker
guessed `boot_main` would live at `0x10000`, but QEMU loads us at
`0x40080000`, then every "call `boot_main`" instruction in the
kernel jumps to `0x10000` — into nothing — and the kernel crashes
on its very first step.

There's no fixing this at runtime. There's no operating system
under us to patch addresses. So the linker has to get it right
the first time.

Here's the heart of our `linker.ld`:

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
        __start_nx_components = .;
        KEEP(*(nx_components))
        __stop_nx_components = .;
    } > RAM

    .data : ALIGN(4096) { *(.data .data.*) } > RAM

    .bss  : ALIGN(4096) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    } > RAM

    . = ALIGN(4096);
    __kernel_end = .;

    . = . + 0x40000;        /* 256 KiB of stack */
    __stack_top = .;

    . = ALIGN(4096);
    __free_mem_start = .;
}
```

Let's go through it like a beginner.

- **`ENTRY(_start)`.** "The program starts at the function called
  `_start`." This goes into the ELF header so debuggers know
  where to look. (For our raw `kernel.bin`, this line is just
  informational — QEMU jumps to a hardcoded address regardless.)

- **`MEMORY { RAM (rwx) : ORIGIN = 0x40080000, LENGTH = 64M }`.**
  "There's one region of memory called RAM. It's readable,
  writable, and executable, it starts at address `0x40080000`, and
  it's 64 MiB long." Later, when we say `> RAM`, we mean "place
  this section into that region". The linker checks at link time
  that we don't overflow.

  Wait — QEMU has 1 GiB of RAM, so why only 64 MiB here? Because
  the linker only needs to know about the part that holds the
  *static* kernel image (code, data, kernel stack). The rest is
  free RAM that the PMM hands out at runtime. 64 MiB is way more
  than the kernel image needs.

- **`. = 0x40080000`.** This is the **location counter**. The dot
  `.` is a special variable in linker scripts that means "the
  current address as I lay things out". Setting it to
  `0x40080000` tells the linker: "from here on, place things at
  this address and grow upward". So the first byte of the first
  section ends up at `0x40080000`, the next byte at `0x40080001`,
  and so on. The actual addresses of all our symbols come from
  this number.

- **The `.text` block.** "Pile up code here. First, anything from
  the `.text.boot` section (that's our `_start`). Then everything
  from `.text` and `.text.*` (that's all the rest of the code)."
  The `KEEP(...)` wrapper says "don't ever delete this section,
  even if dead-code elimination thinks it's unused" — important
  insurance, since `_start` isn't called by name from any C code.

- **The `.rodata` block.** Read-only data — string literals,
  `const` arrays, lookup tables. The `ALIGN(4096)` rounds the
  location counter up to the next 4 KiB boundary before placing
  the section. Why 4 KiB? Because that's the page size, and once
  the MMU is on, page permissions can only be set per page. We
  want all of `.rodata` to be a clean run of "read-only,
  no-execute" pages, with no `.text` (executable) or `.data`
  (writable) bytes squeezed into the same page.

- **`__start_nx_components` and `__stop_nx_components`.** These
  are linker-defined symbols. They aren't real variables — they
  don't take up any bytes in the output. They're just **names the
  linker assigns to the current address** when it reaches that
  point in the script.

  In between them, `KEEP(*(nx_components))` collects every input
  byte that was tagged for the `nx_components` section. The
  result is one contiguous range, with `__start_nx_components`
  pointing at the first byte and `__stop_nx_components` pointing
  just past the last byte.

  Why does this matter? Because every component in the kernel
  uses a macro called `NX_COMPONENT_REGISTER(...)` that emits a
  small `struct` describing itself, **tagged for the
  `nx_components` section**. So when the kernel boots, it walks
  from `__start_nx_components` to `__stop_nx_components` and
  finds *every component automatically*. No "init function that
  calls `register(...)` for each one" — the registry is **built
  by the linker itself**. That's a really neat trick used in lots
  of bare-metal and embedded code.

- **The `.bss` block.** "Reserve room for zero-initialised
  globals." `__bss_start` and `__bss_end` are linker-defined
  symbols pointing at the beginning and end of the range — those
  are the symbols `start.S` reads when it zeroes the BSS.

  Remember: BSS bytes are *not* stored in the file. The file just
  says "leave this much room". On a hosted system the OS would
  zero it; on bare metal `start.S` does it.

- **`__kernel_end`, the 256 KiB gap, and `__stack_top`.** After
  `.bss`, the script reserves 256 KiB of memory for the kernel
  stack, then puts `__stack_top` at the top end of that gap.
  `start.S` loads the stack pointer with `__stack_top` so the
  stack starts at the high end and grows downward into the
  reserved gap.

- **`__free_mem_start`.** Right after the stack. This is where
  free RAM begins, as far as the kernel is concerned. The PMM
  (physical memory manager) starts handing out pages from here.

So the linker script is doing two related jobs at once:

1. **Where does each section go?** (Code first, read-only next,
   writable, then BSS, then space for stack and free memory.)
2. **What special addresses should be marked as named symbols, so
   C code can refer to them?** (`__bss_start`, `__stack_top`,
   `__free_mem_start`, etc.)

C code reads these as plain `extern char[]` declarations:

```c
extern char __bss_start[];
extern char __bss_end[];
extern char __free_mem_start[];
```

The C compiler doesn't care what those addresses are — they get
filled in at link time, after the linker script runs.

---

## Two file formats: ELF and raw binary

When the linker finishes, it produces `kernel.elf`. ELF is a
**structured** format — it has a header, a section table, a
symbol table, optional debug info, and so on. Lots of metadata.

But QEMU's `-kernel` mode for ARM64 doesn't want ELF. It wants a
**raw binary** — just the bytes that go into RAM, in order, with
no header and no metadata.

So we run `objcopy` to convert:

```
kernel.bin: kernel.elf
	$(OBJCOPY) -O binary $< $@
```

Just one line in the Makefile. `objcopy -O binary` strips the ELF
wrapping and leaves the raw bytes.

You can check the difference with the `file` command:

```
$ file kernel.elf kernel.bin
kernel.elf: ELF 64-bit LSB executable, ARM aarch64, ...
kernel.bin: data
```

Mental model:

- **`kernel.elf` is for tools.** Debuggers, `readelf`, `objdump`,
  `nm` — they all want sections and symbols, and they get them
  from the ELF.
- **`kernel.bin` is for QEMU.** Byte 0 of the file lands at
  address `0x40080000` in guest RAM. Byte N lands at
  `0x40080000 + N`. Nothing more, nothing less.

When you run `make debug`, QEMU starts with `-s -S` (`-s` opens a
GDB stub on TCP port 1234, `-S` freezes the CPU at boot). You then
attach GDB to `kernel.elf` for the symbol info — but QEMU itself
is running `kernel.bin`. Both files describe the **same code at
the same addresses** — only the wrapping differs.

---

## The full timeline

Putting it all together, here's what happens from "you typed
`make run`" to "the kernel is fully booted":

```
QEMU launches
    │
    ▼
QEMU mini-bootloader runs at 0x40000000:
    sets x0 = address of DTB, jumps to 0x40080000
    │
    ▼
0x40080000: _start  (start.S — assembly)
    │
    ├── EL2?  → drop to EL1 via eret
    ├── set sp = __stack_top
    ├── zero [.bss_start, .bss_end)
    └── bl boot_main      ← stack works now → C is OK
        │
        ▼
boot.c: boot_main()  (C)
    │
    ├── uart_init()                     — first console output
    ├── mmu_init()                      — turn on the MMU
    ├── pmm_init(__free_mem_start, …)   — register free RAM
    ├── vectors_install()               — exception handlers
    ├── gic_init()                      — interrupt controller
    ├── timer_init(10)                  — 10 Hz tick
    ├── nx_console_init()               — keyboard input
    ├── nx_framework_bootstrap()        — walk nx_components,
    │                                     init/enable each
    ├── sched_start()                   — turn boot context into
    │                                     the idle task
    ├── irq_enable_local()              — interrupts on
    │
    └── one of:
         - idle loop (production)
         - ktest_main() (test build)
         - busybox shell (interactive build)
```

By the time `nx_framework_bootstrap()` runs, we're in normal
C-land: the MMU is on, the allocator works, the registry is
populated from objects the *linker itself* placed in the
`nx_components` section, and the framework can move from "image of
bytes in RAM" to "running kernel with seven components plugged in".

---

## A few extra things to know

- **Three kernel binaries from one linker script.** The same
  `linker.ld` is used for `kernel.bin` (production),
  `kernel-test.bin` (in-kernel tests), and `kernel-busybox.bin`
  (interactive shell). They differ only in *which `.o` files get
  linked in* and which preprocessor flag is set
  (`-DNX_KTEST` for the test build, `-DNX_INIT_BUSYBOX` for the
  shell build). The `boot_main()` function checks those flags and
  picks the right post-bring-up path.

- **No PIE, no relocations.** The kernel is linked at exact
  addresses. There's no list of "patch this address at load time"
  entries. **What you build is what runs**, byte for byte. (This
  is unlike a normal Linux program, where the dynamic loader
  patches addresses on the fly because the program could be
  loaded anywhere.)

- **Why does `_start` never return?** `start.S` calls `boot_main`
  with `bl` (branch-and-link), which saves the return address in
  the link register `LR`. If `boot_main` *did* return, control
  would go back into `start.S` to a label called `hang:`, which
  is `wfe; b hang` — an infinite "wait for an event" loop. In
  practice, `boot_main` has its *own* infinite loop at the end
  (`wfi`, "wait for interrupt"), so we never come back.

- **Components register themselves through a section.**
  `NX_COMPONENT_REGISTER(...)` is a macro that expands to
  something like:

  ```c
  static const struct nx_component_descriptor my_desc
      __attribute__((section("nx_components"))) = { ... };
  ```

  The `__attribute__((section("nx_components")))` is what tells
  GCC to put this `struct` in the `nx_components` section
  (instead of the default `.rodata`). The linker then collects
  every such struct across every `.o` file into one continuous
  run. **No "register-on-init" function call ever happens** — the
  registry is built at link time. This pattern shows up in many
  bare-metal and Linux-kernel codebases.

---

## Where to read more

- [`docs/framework-bootstrap.md`](../docs/framework-bootstrap.md) —
  what `nx_framework_bootstrap()` actually does with the components
  it finds in the `nx_components` section.
- [`../core/README.md`](../core/README.md) — what the rest of
  `core/` (mmu, pmm, irq, timer, sched) does, in the same order
  `boot_main()` brings them up.
- [`../README.md`](../README.md) — project overview and
  quick-start.
- [Linux Documentation/arm64/booting.rst](https://www.kernel.org/doc/html/latest/arch/arm64/booting.html)
  — the official AArch64 boot protocol that QEMU's `-kernel`
  mode follows. Reads more like a checklist than a story; come
  back to it once the basics in this doc feel comfortable.
- ARM Architecture Reference Manual, "Exception model" chapter —
  the deep technical reference for exception levels, `CurrentEL`,
  `eret`, and the system registers `start.S` programs to drop
  from EL2 to EL1. Heavy reading; the pieces of it relevant to
  `start.S` are quoted directly in the comments of that file.
