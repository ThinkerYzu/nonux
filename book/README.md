# A Hands-On Tour of Building a Kernel

A book in progress, aimed at people who want to **understand how a
kernel works by building one**. We use [nonux](../README.md) as the
running example — a small, composable microkernel for ARM64 / QEMU
written from scratch — and walk through what each piece does, why
it exists, and how it connects to the next one.

This is not a reference manual. It's a guided tour. Each chapter
explains a concept, then shows it working in nonux. By the end you
should be able to **read the source of a real kernel** without
feeling lost.

## Who this is for

- People comfortable with C who have never looked inside a kernel
  before.
- Computer-science students taking an OS course who want to see a
  small, complete kernel they can actually build and run.
- Hobbyists writing their own kernel and looking for a worked
  example to compare against.

You **don't** need to know ARM assembly, the ELF file format, or
how a linker works ahead of time — we'll introduce each as it
becomes relevant. You **do** need to be able to read C and follow
along when we point at source files.

## How to read this book

Each chapter is self-contained but assumes the chapters before it.
If a term you don't recognize shows up, the chapter that introduces
it is usually one or two earlier in the book.

You'll get the most out of each chapter by **building and running**
the kernel as you read. See the top-level
[`../README.md`](../README.md) for build instructions
(`make && make run`).

## Chapters

The book is planned as **18 chapters in 9 parts**. **Shipped
chapters are linked**; planned chapters are listed without links.
Chapters arrive when they're written, not on a schedule.

### Part I — Coming up

1. [Boot and linker — how nonux comes to life](01-boot-and-linker.md)
2. [The console: kprintf and the UART](02-console-and-uart.md)

### Part II — Time and interrupts

3. [Exceptions, the GIC, and IRQs](03-exceptions-gic-and-irqs.md)
4. [The timer and ticks](04-timer-and-ticks.md)

### Part III — Memory

5. [Physical memory and the page allocator (PMM)](05-physical-memory-and-pmm.md)
6. [Virtual memory and the MMU](06-virtual-memory-and-mmu.md)

### Part IV — Tasks

7. [Kernel threads and context switching](07-kernel-threads-and-context-switch.md)
8. [The scheduler](08-the-scheduler.md)

### Part V — Framework basics

9. Slots, components, and the registry
10. Interface Definition Language (IDL)

### Part VI — IPC and storage

11. Channels and IPC
12. Filesystems (VFS, ramfs, procfs)

### Part VII — Framework, advanced

13. Hooks: intercepting and overriding
14. Recomposition and runtime config

### Part VIII — Userspace

15. Processes and the user/kernel boundary
16. System calls: EL0 ↔ EL1

### Part IX — Userspace runtime and build

17. POSIX shim, libnxlibc, and busybox
18. Build, test, AI operability

### Appendices (planned)

- A. ARM64 cheat sheet
- B. Glossary
- C. Source map
- D. Where to read more

## Conventions used in the book

- **Bold** introduces a new term the first time it appears in a
  chapter. From then on, the term is used without ceremony.
- `Monospace` for code, file paths, function names, register
  names, addresses, command-line arguments.
- Block quotes set off side notes — the kind of thing that's
  interesting but not on the main thread of the chapter.
- Cross-references use relative links so you can navigate between
  chapters and into the source code without leaving your editor.

## See also

- [`../README.md`](../README.md) — project overview, build, run.
- [`../docs/`](../docs/) — reference docs for the framework
  modules. The book explains *concepts*; `docs/` is the *manual*.
- [`../core/README.md`](../core/README.md),
  [`../framework/README.md`](../framework/README.md), … —
  per-directory READMEs that map the source tree.
