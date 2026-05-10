# Exceptions, the GIC, and IRQs

Chapter 2 ended with a diagram that skipped over a step. When a
key was pressed in the QEMU terminal, we said:

> GIC delivers IRQ 33 to the CPU
>     ↓
> CPU jumps to EL1 IRQ vector (`core/cpu/exception.S`)
>     ↓
> `irq_dispatch()` → `nx_console_rx_isr(NULL)`

Three lines of arrow. In reality the CPU doesn't *know* which C
function to run when an IRQ arrives. There is no "IRQ 33 means
`nx_console_rx_isr`" wired into the silicon. The CPU only knows
how to do one thing on an IRQ: jump to a fixed address. Getting
from "fixed address" to "the right C function" is a small pipeline
of code we have to write, and a small pipeline of hardware we have
to talk to.

This chapter walks that pipeline. By the end you should be able to
read the **vector table** (the table of fixed addresses the CPU
jumps to), follow what the kernel saves and restores around each
exception, understand what the **GIC** (the system's interrupt
controller) is actually doing on either side of an IRQ, and know
why `boot_main` brings these things up in exactly the order it
does.

The same machinery handles more than just IRQs. Every **exception**
goes through this path — a system call from a user program, a
memory fault from a buggy pointer dereference, a CPU error. We'll
see what each kind looks like and where they fork off.

The relevant files in this repo:

- [`core/cpu/vectors.S`](../core/cpu/vectors.S) — the vector
  table itself, plus the assembly macros that save and restore
  the trap frame around every exception.
- [`core/cpu/exception.h`](../core/cpu/exception.h),
  [`core/cpu/exception.c`](../core/cpu/exception.c) — the C-level
  side: `vectors_install`, the four `on_*` handlers
  (`on_sync`, `on_irq`, `on_fiq`, `on_serror`), and the local
  IRQ mask helpers.
- [`core/irq/irq.h`](../core/irq/irq.h),
  [`core/irq/irq.c`](../core/irq/irq.c) — the per-IRQ dispatch
  table and the `irq_register` / `irq_dispatch` API.
- [`core/irq/gic.c`](../core/irq/gic.c) — the GICv2 driver:
  `gic_init`, `gic_enable`, `gic_disable`, `gic_ack`, `gic_eoi`.
- [`core/boot/boot.c`](../core/boot/boot.c) — the section of
  `boot_main` that brings all of the above up, in order.

---

## Terms you'll see

- **Exception.** ARM's umbrella name for any event that pulls the
  CPU off its current path and jumps it to a known handler. There
  are four kinds on ARM64: synchronous exceptions, IRQs, FIQs,
  and SErrors. Other architectures call the same idea different
  things — x86 says "interrupt" for some, "trap" or "fault" for
  others — but on ARM it's all "exception".
- **Synchronous exception (sync).** An exception caused *by the
  instruction the CPU just executed*. Examples: a memory fault
  (the instruction touched an unmapped address), a system call
  (the instruction was `svc`), an undefined instruction. "Sync"
  because the exception is tied to a specific instruction in the
  program — re-running the program would hit it at the same
  spot.
- **Asynchronous exception.** An exception triggered by something
  *outside* the current instruction stream — typically a device
  signaling the CPU. IRQs, FIQs, and SErrors are all asynchronous.
  "Async" because the timing isn't tied to any particular
  instruction; it can land between any two.
- **IRQ (Interrupt Request).** An asynchronous exception used for
  ordinary device interrupts — UART, timer, network card, disk.
  This is the one chapter 2 set up for the PL011's RX FIFO.
- **FIQ (Fast Interrupt Request).** A higher-priority sibling of
  IRQ, intended for latency-critical interrupts. Some systems
  reserve FIQ for one specific high-priority device (a real-time
  audio chip, say). nonux doesn't use FIQ; we treat any FIQ that
  arrives as an unexpected-state error.
- **SError (System Error).** An exception for asynchronous
  hardware errors — a memory bus fault that arrives later than
  the instruction that caused it, an external abort from a device.
  Rare in practice. nonux halts on SError; a real system would
  log and try to recover.
- **Vector.** A fixed address the CPU jumps to when a particular
  kind of exception happens. "Vector" here is the old computing
  sense of "a pointer to a handler" — the CPU's vector for an IRQ
  is "where to go when an IRQ fires".
- **Vector table.** A block of memory containing every vector the
  CPU might use, laid out at fixed offsets. ARM64's vector table
  has 16 slots, one for each combination of exception type and
  origin context. We give the CPU the *base* address of the table;
  the CPU figures out the right offset on its own from the kind of
  exception that occurred.
- **`VBAR_EL1`.** The system register that stores the base address
  of the vector table for exceptions taken at EL1. We write our
  table's address to `VBAR_EL1` once at boot; from that point on,
  every exception consults it.
- **DAIF.** A 4-bit field in the CPU's status register that masks
  the four kinds of exceptions. The bits stand for **D**ebug,
  **S**Error (the "A" is a leftover from older ARM names),
  **I**RQ, and **F**IQ. Setting a bit *masks* (blocks) that
  exception kind on this CPU; clearing it lets the exception
  through. The kernel toggles the I bit specifically to enable
  or disable IRQs.
- **Trap frame.** A struct describing the CPU's saved state at
  the moment an exception was taken — every general-purpose
  register, the saved program counter, the saved status register,
  the EL0 stack pointer. The vector entry pushes a trap frame on
  the kernel stack so the C handler can read it (and so the
  return path can put the CPU back exactly where it was).
- **`ELR_EL1`** (**Exception Link Register**). A system register
  the CPU automatically loads on an exception with the address of
  the instruction that should run next when we return — usually
  the instruction *after* the one that took the exception (for
  syscalls and IRQs) or the faulting instruction itself (for
  faults to be retried).
- **`SPSR_EL1`** (**Saved Program Status Register**). A system
  register the CPU automatically loads on an exception with a
  snapshot of the status register from before the exception. It
  records which exception level we came from, the DAIF mask state
  at the time, the condition flags, and so on. The return path
  uses it to restore the prior state.
- **`ESR_EL1`** (**Exception Syndrome Register**). For
  *synchronous* exceptions, the CPU sets this register to describe
  *why* the exception happened. It contains a 6-bit "exception
  class" (EC) field that says "this was a syscall" or "this was
  a data abort", plus a smaller "instruction-specific syndrome"
  field with extra detail.
- **`FAR_EL1`** (**Fault Address Register**). For memory-related
  synchronous exceptions, this register holds the *address that
  the program tried to access*. Together with `ESR_EL1` it gives
  the kernel enough information to either fix the fault (e.g.
  demand-page) or terminate the offending program.
- **`eret`** (already met in chapter 1). The instruction that
  returns from an exception. It restores `pstate` from `SPSR_EL1`,
  restores the program counter from `ELR_EL1`, and switches back
  to whichever exception level was running before.
- **GIC (Generic Interrupt Controller).** The chip on an ARM
  system that takes IRQ signals from every device on the board
  and decides which one(s) to deliver to which CPU. ARM has
  published a few generations: GICv1, GICv2, GICv3, GICv4. The
  QEMU `virt` machine includes a **GICv2**, which is what we
  drive.
- **Distributor (GICD).** The half of the GIC that talks to the
  *devices*. It collects all incoming IRQ lines, holds the per-IRQ
  enable bits and priorities, and decides which IRQ should fire
  next. The QEMU `virt` machine puts the distributor at
  `0x08000000`.
- **CPU interface (GICC).** The half of the GIC that talks to the
  *CPU*. The CPU asks "what IRQ is pending?" through this
  interface and tells it "I'm done" through this interface. The
  QEMU `virt` machine puts the CPU interface at `0x08010000`.
- **SPI (Shared Peripheral Interrupt).** An IRQ that can be
  delivered to *any* CPU on the system. Numbered 32 and up. Most
  device interrupts (UART, network, disk) are SPIs.
- **PPI (Private Peripheral Interrupt).** An IRQ specific to one
  CPU. Numbered 16–31. The per-CPU timer is a PPI.
- **SGI (Software Generated Interrupt).** An IRQ one CPU sends to
  another CPU on purpose. Numbered 0–15. Used for cross-CPU
  notifications on multi-core systems. nonux is single-CPU and
  doesn't use SGIs.
- **Acknowledge.** Reading the GIC's `IAR` (Interrupt Acknowledge
  Register) tells the GIC "I've started handling this IRQ" and
  returns the IRQ number that fired. After acknowledging, no
  other CPU can pick the same IRQ.
- **EOI (End Of Interrupt).** Writing to the GIC's `EOIR` (End Of
  Interrupt Register) tells the GIC "I'm done handling that IRQ —
  you're free to deliver another one".

---

## A quick recap of where chapter 2 left off

By the end of the last chapter, we had four pieces of an input
pipeline:

1. The PL011 was set up to raise an IRQ whenever its RX FIFO had
   at least one byte (`nx_console_init`).
2. We told the GIC to deliver IRQ 33 to the CPU
   (`gic_enable(33)`).
3. We registered our handler `nx_console_rx_isr` against IRQ 33
   (`irq_register(33, nx_console_rx_isr, 0)`).
4. We trusted that "somehow" the CPU would land in
   `nx_console_rx_isr` whenever IRQ 33 fired.

That fourth step is the gap this chapter fills. There are two
sides to it: a **hardware side** (how does the GIC tell the CPU?
how does the CPU jump anywhere at all?) and a **software side**
(what's the table of "which C function for which IRQ number"?).

We'll start from the CPU side and work outward to the GIC.

---

## Exceptions: the four kinds

The CPU runs one instruction after another. Sometimes, between
two instructions, the CPU stops what it was doing and jumps to a
*kernel-supplied* address instead. The thing that interrupted it
can be one of four kinds:

| Kind | Stands for | Caused by | Example |
|------|-----------|-----------|---------|
| **Synchronous exception** | "sync" | The instruction the CPU just ran | A memory fault, an `svc` (system call), an undefined instruction |
| **IRQ** | Interrupt Request | A device raising its IRQ line | A UART RX FIFO has a byte; a timer counted down |
| **FIQ** | Fast Interrupt Request | A device raising a higher-priority FIQ line | A real-time chip in some embedded systems; not used in nonux |
| **SError** | System Error | An asynchronous hardware error | A delayed memory bus fault |

The split between **synchronous** and the rest is the key
distinction. A sync exception is *the program's fault*: it ran an
instruction that the CPU couldn't complete normally. An IRQ, FIQ,
or SError doesn't care what the CPU was doing — they show up
because *something else* needed attention.

In nonux's source, you'll see each kind handled by a separate C
function:

- `on_sync` — system calls and faults from EL0, kernel bugs at
  EL1.
- `on_irq` — every device IRQ. This is by far the busiest one.
- `on_fiq` — never expected; we panic if it fires.
- `on_serror` — never expected; we panic if it fires.

All four live in [`core/cpu/exception.c`](../core/cpu/exception.c).
We'll come back to them after we see how the CPU finds them.

---

## The vector table

When an exception happens, the CPU has to jump *somewhere*. ARM64
gives the kernel one job: provide a **vector table** — a 2-KiB
block of memory laid out in a specific way — and tell the CPU
where the table is. From then on, the CPU figures out the right
offset into the table on its own.

### What's in the table

The table has **16 entries**. They fall into four groups of four.
The four groups correspond to *contexts* — combinations of "where
was the CPU when the exception happened":

| Group | Context |
|-------|---------|
| 1 (entries 0–3)  | Current EL using `SP_EL0` (legacy mode — we don't use this) |
| 2 (entries 4–7)  | Current EL using `SP_EL1` — the kernel interrupted itself |
| 3 (entries 8–11) | Lower EL running AArch64 — userspace (EL0) interrupted |
| 4 (entries 12–15)| Lower EL running AArch32 — 32-bit userspace (we don't support this) |

Within each group, the four entries are the four exception kinds
in fixed order: **sync, IRQ, FIQ, SError**.

So entry 5 is "IRQ taken with the kernel running on its own
stack". Entry 8 is "sync exception taken from userspace" — that's
the one the CPU uses for system calls.

Each entry is **128 bytes** of code. ARM picked 128 bytes so a
short handler can fit *directly* in the entry; the CPU will jump
straight to entry 5 + 128*1 = entry 5 and start executing
instructions there. There's no "look up the address of the
handler and call it" step — the entry *is* the handler.

For us, 128 bytes isn't enough — saving every register is more
than 32 instructions. So each entry is a single `b` (branch)
instruction to a longer stub out of line:

```asm
.balign 2048
.global vectors
vectors:
    /* --- Current EL with SP_EL0 (unused — we run on SP_EL1) --- */
    .balign 0x80
    b       _unimpl_stub
    .balign 0x80
    b       _unimpl_stub
    .balign 0x80
    b       _unimpl_stub
    .balign 0x80
    b       _unimpl_stub

    /* --- Current EL with SP_ELx (EL1h — this is us) --- */
    .balign 0x80
    b       _sync_stub
    .balign 0x80
    b       _irq_stub
    .balign 0x80
    b       _fiq_stub
    .balign 0x80
    b       _serror_stub
    /* …two more groups of four… */
```

The `.balign 0x80` directives force each entry to start exactly
128 bytes apart. The `.balign 2048` at the top forces the whole
table to start on a 2-KiB boundary, which `VBAR_EL1` requires.

(`0x80` is hexadecimal for 128. `2048` is `16 * 128` — the size
of the whole table.)

For each entry we need, the body is exactly one instruction: a
branch to the matching stub (`_sync_stub`, `_irq_stub`, etc.).
For the entries we don't expect to fire — entries from the unused
contexts, or the FIQ entries since we don't use FIQ — we point at
`_unimpl_stub`, which prints a message and halts the CPU.

> **Side note: why 128-byte entries?** Other architectures use
> different sizes. x86's interrupt descriptor table uses 8-byte
> entries, each containing a pointer to the handler. ARM's
> 128-byte entries trade a little ROM/RAM for skipping a memory
> indirection: the CPU jumps directly into code, no extra fetch.
> The decision shows up in benchmarks of *very* short interrupt
> handlers; for ordinary handlers the difference is invisible.

### Telling the CPU about the table

Just defining the table isn't enough — the CPU has no way to know
*we* defined it. We have to write the table's address into a
system register called `VBAR_EL1` (Vector Base Address Register
for EL1). One line of assembly does it:

```c
extern char vectors[];

void vectors_install(void)
{
    asm volatile("msr vbar_el1, %0" :: "r"((uint64_t)vectors) : "memory");
    asm volatile("isb" ::: "memory");
}
```

The `msr` ("move to system register") instruction copies a
general-purpose register's value into a system register. The
trailing `isb` ("instruction synchronization barrier") tells the
CPU to flush its instruction-fetch pipeline before it executes any
more instructions, so a wrong vector base can't sneak through on a
prefetched-and-not-yet-executed instruction.

The `extern char vectors[]` line gives C a way to refer to the
symbol `vectors`, which the linker resolves to the address where
the assembler placed the table. (Chapter 1 walked through how the
linker assigns addresses; the same machinery works here.)

`vectors_install` is called once early in `boot_main`, right after
the PMM and printer are up. From that point on, any exception
taken at EL1 will land in our table.

---

## What gets saved on an exception

The CPU jumps to one of our 16 entries. A few things have already
happened automatically:

1. **`ELR_EL1`** has been set to the address of the instruction
   the CPU was about to execute (or the faulting instruction, for
   faults).
2. **`SPSR_EL1`** has been set to a snapshot of the status
   register — the previous exception level, the DAIF mask state,
   the condition flags.
3. **`ESR_EL1`** (for synchronous exceptions) and **`FAR_EL1`**
   (for memory-related ones) have been set with diagnostic info.
4. **The CPU is at EL1**, with IRQs masked (`I` bit set in DAIF).

Everything else — every general-purpose register, the EL0 stack
pointer if we came from EL0 — is *exactly as the interrupted code
left it*. If we step on any of those registers before saving them,
the interrupted code will resume in a corrupted state and
malfunction in obscure ways.

So the very first thing every stub does is save them. The
`SAVE_TRAPFRAME` macro in
[`core/cpu/vectors.S`](../core/cpu/vectors.S) does this:

```asm
.macro SAVE_TRAPFRAME
    sub     sp, sp, #272
    stp     x0,  x1,  [sp, #0x000]
    stp     x2,  x3,  [sp, #0x010]
    stp     x4,  x5,  [sp, #0x020]
    /* …x6 through x29… */
    stp     x28, x29, [sp, #0x0E0]
    str     x30,      [sp, #0x0F0]

    /* Save the EL0 stack pointer */
    mrs     x2, sp_el0
    str     x2,       [sp, #0x0F8]

    /* Save the auto-loaded ELR_EL1 + SPSR_EL1 */
    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x0,  x1,  [sp, #0x100]
.endm
```

Three things happen:

1. **Reserve 272 bytes on the stack.** That's enough for 31
   general-purpose registers (`x0`–`x30`), the EL0 stack pointer,
   the saved PC (`ELR_EL1`), and the saved status register
   (`SPSR_EL1`), with a little padding for 16-byte alignment.
2. **Push every general-purpose register.** `stp` ("store pair")
   stores two adjacent registers into 16 bytes of memory, which
   is the most efficient way to save them on ARM64. We walk
   `x0..x29` in pairs, then `x30` (the link register) on its
   own.
3. **Push the four CPU-supplied values.** `mrs` ("move from system
   register") reads a system register into a general-purpose
   register, which we then store. We grab the EL0 stack pointer
   (`SP_EL0`), then `ELR_EL1` and `SPSR_EL1` from their
   auto-saved spots and stash all three.

After `SAVE_TRAPFRAME` runs, the kernel stack looks like this:

```
                      ┌─────────────────────┐  ← higher address
                      │  …caller frame…     │
   sp_before_excep ──▶├─────────────────────┤
                      │  x0, x1             │  offset 0x000
                      │  x2, x3             │  offset 0x010
                      │  …                  │
                      │  x28, x29           │  offset 0x0E0
                      │  x30, (pad)         │  offset 0x0F0
                      │  SP_EL0, (pad)      │  offset 0x0F8
                      │  ELR_EL1, SPSR_EL1  │  offset 0x100
   sp_after_save  ───▶└─────────────────────┘  ← lower address
```

That whole 272-byte region is the **trap frame**. The C handler
gets a pointer to it (`mov x0, sp` before the `bl` to the C
function), and reads it as a `struct trap_frame`:

```c
struct trap_frame {
    uint64_t x[31];      /* x0..x30 */
    uint64_t sp_el0;
    uint64_t pc;         /* ELR_EL1 at time of exception */
    uint64_t pstate;     /* SPSR_EL1 */
};
```

Same offsets, same layout — just a C view of what the assembly
just laid out by hand. The handler can read any field. It can
also *write* a field — we'll see syscalls do exactly that to
deliver a return value to the user program.

When the handler returns, `RESTORE_TRAPFRAME` does the inverse:
load each saved value back into its register (or system register),
add 272 to the stack pointer to free the frame, and the stub then
runs `eret`. Chapter 1 introduced `eret` as "the way to drop from
a higher EL to a lower one"; here it's the way to *return* from an
exception. Same instruction, same effect: `pstate` is restored
from `SPSR_EL1`, the program counter is restored from `ELR_EL1`,
and the CPU resumes the interrupted code as if nothing had
happened.

> **Side note: why we don't lean on the CPU's auto-save more.**
> Some architectures (x86 included) push much more state to the
> stack on an exception automatically — the CPU itself does the
> work. ARM64's design is "we'll save the smallest necessary set;
> kernels can save the rest at their own pace". The bargain is
> that an empty IRQ handler can be very cheap, and a complex
> handler decides for itself how much state it needs. We always
> save everything, because our handlers are written in C and can
> touch any register.

---

## The four C handlers

After `SAVE_TRAPFRAME`, each stub calls one of four C functions:

```asm
_sync_stub:
    SAVE_TRAPFRAME
    mov     x0, sp
    bl      on_sync
    RESTORE_TRAPFRAME
    eret

_irq_stub:
    SAVE_TRAPFRAME
    mov     x0, sp
    bl      on_irq
    bl      sched_check_resched
    RESTORE_TRAPFRAME
    eret
```

(The other two — `_fiq_stub` and `_serror_stub` — follow the same
pattern; we won't dwell on them since they don't fire in nonux.)

`mov x0, sp` puts the address of the trap frame into `x0`, which
is the first-argument register on ARM64. Then `bl` ("branch with
link") calls the C function. The C function gets a
`struct trap_frame *` to inspect.

The IRQ stub has one extra step before restoring: a call to
`sched_check_resched`. That's the scheduler's hook for
"interrupted task should be preempted now". On an IRQ that woke up
a higher-priority task, the scheduler can swap to that task here,
and only the eventual return-to-original-task will run the
matching `RESTORE_TRAPFRAME`. We'll cover that fully when we get
to the scheduler chapter; for now, just know that the line is
intentional.

### `on_irq`: the trivial one

```c
void on_irq(struct trap_frame *tf)
{
    (void)tf;
    irq_dispatch();
}
```

That's the entire IRQ handler. Every IRQ goes through `irq_dispatch`,
which talks to the GIC and calls the right per-IRQ handler. We'll
look at it next, in the GIC section.

The trap frame is unused here. The handler doesn't care *what*
was running when the IRQ fired — it just runs the device's ISR
and returns.

### `on_sync`: the busy one

```c
void on_sync(struct trap_frame *tf)
{
    uint64_t esr, far;
    asm volatile("mrs %0, esr_el1" : "=r"(esr));
    uint32_t ec = (uint32_t)((esr >> 26) & 0x3fu);

    if (ec == ESR_EC_SVC64) {
        nx_syscall_dispatch(tf);
        return;
    }

    asm volatile("mrs %0, far_el1" : "=r"(far));

    switch (ec) {
    case ESR_EC_INST_ABORT_EL0:
        deliver_el0_fault_signal(NX_FAULT_SIGSEGV, "inst_abort",
                                 esr, far, tf->pc);
    case ESR_EC_DATA_ABORT_EL0:
        deliver_el0_fault_signal(NX_FAULT_SIGSEGV, "data_abort",
                                 esr, far, tf->pc);
    case ESR_EC_UNKNOWN:
        if (saved_pstate_was_el0(tf->pstate)) {
            deliver_el0_fault_signal(NX_FAULT_SIGILL, "undef",
                                     esr, far, tf->pc);
        }
        break;
    default:
        break;
    }

    kprintf("\n[EXC] sync  ESR=%lx FAR=%lx ELR=%lx SPSR=%lx\n",
            esr, far, tf->pc, tf->pstate);
    halt_forever();
}
```

This one earns its complexity. Synchronous exceptions cover three
very different cases — system calls, page faults, and "the kernel
itself ran an illegal instruction" — and `on_sync` has to tell
them apart.

The trick is to read **`ESR_EL1`** and look at its **EC**
(Exception Class) field. The EC is a 6-bit number that says
exactly what kind of synchronous exception this was. Bits 31–26
of `ESR_EL1` hold it; the line `(esr >> 26) & 0x3f` extracts
those bits.

A few EC values we care about:

| EC value | Meaning |
|----------|---------|
| `0x15` | `SVC` from AArch64 — a system call from EL0 |
| `0x20` | Instruction abort from a lower EL — EL0 jumped somewhere it can't execute |
| `0x21` | Instruction abort from current EL — kernel jumped somewhere it can't execute (kernel bug) |
| `0x24` | Data abort from a lower EL — EL0 dereferenced a bad pointer |
| `0x25` | Data abort from current EL — kernel dereferenced a bad pointer (kernel bug) |
| `0x00` | "Unknown reason" — usually an undefined instruction; need to check which EL we came from |

The ARM Architecture Reference Manual lists about 60 EC values
total. We handle the ones an EL0 program could realistically
trigger; everything else is treated as a kernel bug and panics.

The first branch handles syscalls: if the EC is `0x15` (a 64-bit
`svc`), forward to the syscall dispatcher and we're done. Faults
from EL0 turn into a `SIGSEGV` or `SIGILL`-shaped process exit.
Faults from EL1 (the kernel itself) drop through to `halt_forever`
— we want a loud failure with full diagnostic information, not a
quiet recovery, because the kernel just did something it
shouldn't have.

> **Side note: why fault registers exist.** When a load
> dereferences a bad pointer, the CPU has two pieces of useful
> information: *which* instruction caused it (`ELR_EL1`) and
> *what address* the instruction was trying to access
> (`FAR_EL1`). Both go into the trap-frame walk above. A panic
> message that prints both is much easier to debug than one that
> only prints the program counter.

### `on_fiq` and `on_serror`: the unused ones

```c
void on_fiq(struct trap_frame *tf)
{
    (void)tf;
    kprintf("\n[EXC] unexpected FIQ\n");
    halt_forever();
}

void on_serror(struct trap_frame *tf)
{
    (void)tf;
    kprintf("\n[EXC] SError\n");
    halt_forever();
}
```

Both are stubs. nonux doesn't configure any device to use FIQ;
QEMU's `virt` machine doesn't generate SErrors in normal use. If
either fires, something is wrong, and the safest thing to do is
print a message and stop — silently ignoring an SError can mask
hardware corruption.

---

## The GIC: from a wire to a CPU

Now we know how the CPU dispatches an exception, including an IRQ.
But where does the IRQ *come from*? An ARM SoC has dozens of
devices, each able to raise its own IRQ line at any time. Without
help, the CPU would have to listen to every line at once and
prioritize between them. That's a lot for the CPU to manage.

ARM's solution is the **GIC** — Generic Interrupt Controller. The
GIC sits *between* the devices and the CPU. Devices raise their
IRQ lines on the GIC; the GIC accepts, prioritizes, and forwards
exactly one IRQ at a time to the CPU.

For nonux on the QEMU `virt` machine, the relevant pieces of the
GICv2 sit at fixed addresses:

| Address      | What's there        |
|--------------|---------------------|
| `0x08000000` | GIC distributor     |
| `0x08010000` | GIC CPU interface   |

(That table also showed up in chapter 2's MMIO section.)

### Two halves: distributor and CPU interface

The GIC is split into two halves on purpose:

- The **distributor** (GICD) is the *device-facing* half. It has
  one input wire per IRQ line on the system. It holds the per-IRQ
  enable bit, the per-IRQ priority, and the routing decision
  ("which CPU should see this IRQ?"). All the system-wide state
  lives here.
- The **CPU interface** (GICC) is the *CPU-facing* half. It's
  what the CPU itself reads and writes: "tell me which IRQ
  fired", "I'm done with this IRQ". Each CPU has its own CPU
  interface, even though there's only one distributor.

For a single-CPU system like nonux on QEMU, the split feels
overkill — there's only one CPU, so why separate the halves? The
answer is that GIC was designed for multi-core ARM systems from
the start. Once you have four CPUs, you need a way to say "this
IRQ goes to CPU 2 specifically", "this IRQ can go to any CPU that
isn't busy", "CPU 0 should ignore it for now". Splitting the
distributor (one shared piece) from the CPU interface (one per
CPU) is the natural shape for that. nonux just uses a small
corner of it.

```
   ┌────────┐  ┌────────┐  ┌────────┐
   │ device │  │ device │  │ device │   …all the devices on the SoC
   │  UART  │  │ timer  │  │  net   │
   └───┬────┘  └───┬────┘  └───┬────┘
       │ IRQ 33   │ IRQ 30   │ IRQ 47
       └─────┬────┴────┬─────┘
             ▼         ▼
        ┌──────────────────┐
        │   GIC distributor│  enables, priorities, routing
        │   (0x08000000)   │
        └────────┬─────────┘
                 ▼
        ┌──────────────────┐
        │  GIC CPU iface   │  ack, EOI; per-CPU
        │   (0x08010000)   │
        └────────┬─────────┘
                 │ IRQ
                 ▼
            ┌─────────┐
            │   CPU   │
            └─────────┘
```

### IRQ numbering: SGI, PPI, SPI

The GIC names IRQs by number, in three ranges:

| Range | Name | What it's for |
|-------|------|---------------|
| 0–15  | SGI (Software Generated Interrupt) | One CPU pokes another |
| 16–31 | PPI (Private Peripheral Interrupt) | A device that's wired *into one specific CPU* |
| 32+   | SPI (Shared Peripheral Interrupt) | A device that *any* CPU can handle |

The PL011's IRQ on the QEMU `virt` machine is **SPI #1**. Adding
the SPI base of 32, that's IRQ **33**. Chapter 2 showed us
hardcoding 33; now we know what the number means: "the second
shared peripheral interrupt the GIC knows about".

The per-CPU timer (covered in a later chapter) is a PPI: it's
wired into each CPU's own GIC slot, so each CPU has its own copy.
SGIs only matter for multi-core — one CPU writes a register that
makes the GIC raise an IRQ on a different CPU.

### Programming the GIC: enabling an IRQ

Bringing the GIC up takes three or four lines:

```c
void gic_init(void)
{
    wr32(GICD_CTLR, 0x3);   /* enable distributor */
    wr32(GICC_PMR,  0xFF);  /* accept any priority on the CPU interface */
    wr32(GICC_CTLR, 0x3);   /* enable CPU interface */
}
```

`GICD_CTLR` (control register on the distributor) and `GICC_CTLR`
(control register on the CPU interface) are simple on/off
switches. Bit 0 of each enables the corresponding half.

`GICC_PMR` is the **priority mask register**. The GIC supports
priorities — every IRQ has a priority value, and the CPU interface
will *only* deliver IRQs whose priority is below the current mask.
Setting the mask to `0xFF` (the highest possible value) tells the
CPU interface "let everything through". Lower values would mask
out lower-priority IRQs, which is sometimes useful for selectively
silencing chatty devices, but not something we need at this point.

After `gic_init`, the GIC is *generally* on, but every individual
IRQ is still disabled. Enabling a specific IRQ takes one more
write:

```c
void gic_enable(unsigned int irq)
{
    wr8(GICD_IPRIORITYR + irq, 0x00);                      /* priority 0 */
    wr32(GICD_ISENABLER(irq / 32), 1U << (irq % 32));      /* enable bit */
}
```

The first write sets the IRQ's priority to 0 (the highest in
ARM's "lower number = higher priority" scheme — yes, opposite of
the priority mask). Each IRQ has its own one-byte priority slot
in the `GICD_IPRIORITYR` array.

The second write sets the enable bit. The GIC has 32 IRQ enables
per 32-bit word — IRQ 0 is bit 0 of `ISENABLER(0)`, IRQ 33 is
bit 1 of `ISENABLER(1)`, and so on. The arithmetic
`(irq / 32, irq % 32)` picks the right word and bit.

A useful detail: `GICD_ISENABLER` ("Interrupt Set Enable
Register") is a *write-1-to-set* register. Writing a 1 to a bit
enables that IRQ; writing a 0 to a bit does **nothing**. To
disable the IRQ, you have to write a 1 to the *separate*
`GICD_ICENABLER` ("Interrupt Clear Enable Register") at offset
0x180. ARM uses set/clear pairs everywhere in the GIC, so two
CPUs can independently enable and disable IRQs without a
read-modify-write race.

```c
void gic_disable(unsigned int irq)
{
    wr32(GICD_ICENABLER(irq / 32), 1U << (irq % 32));
}
```

After `gic_init` and `gic_enable(33)`, the GIC is set up to
deliver one specific IRQ — the PL011's — to the (only) CPU.

### Acknowledging and ending

Once an IRQ fires, the CPU runs `_irq_stub`, which calls
`on_irq`, which calls `irq_dispatch`. `irq_dispatch` has to ask
the GIC two things:

1. **Which IRQ fired?** Read `GICC_IAR` (Interrupt Acknowledge
   Register). The bottom 10 bits hold the IRQ number. Reading the
   register also tells the GIC "I've started — don't deliver this
   to anyone else".
2. **I'm done.** After the handler runs, write the same IRQ
   number to `GICC_EOIR` (End Of Interrupt Register). That tells
   the GIC "this IRQ is done — re-enable delivery".

The pair of reads/writes:

```c
unsigned int gic_ack(void)
{
    return rd32(GICC_IAR) & 0x3FFU;
}

void gic_eoi(unsigned int irq)
{
    wr32(GICC_EOIR, irq);
}
```

The `& 0x3FFU` masks off the upper bits, which on multi-core GICs
also encode "which CPU sent this SGI". For us, the bottom 10 bits
are all that matter.

If we forgot to write `EOIR`, the GIC would think we're still busy
with this IRQ and would never deliver another one — the system
would silently freeze on the second key press. Conversely, calling
`EOIR` twice for the same IRQ corrupts the GIC's accounting.
Exactly one ack and exactly one EOI per IRQ, in that order. That
discipline is the dispatcher's job.

---

## The dispatcher and the per-IRQ table

We've covered "how an IRQ becomes a function call into `on_irq`"
and "how the GIC tells `on_irq` which IRQ fired". The last step
is **routing**: there are dozens of possible IRQs, each owned by
a different driver. How does `on_irq` find the right driver's
handler?

A small lookup table:

```c
struct irq_entry {
    irq_handler_t fn;
    void         *data;
};

static struct irq_entry g_table[IRQ_TABLE_SIZE];
```

`IRQ_TABLE_SIZE` is 128. Each slot is a function pointer plus an
optional `void *` cookie — the same shape every C-style callback
API uses.

A driver populates one entry through `irq_register`:

```c
int irq_register(unsigned int irq, irq_handler_t fn, void *data)
{
    if (irq >= IRQ_TABLE_SIZE) return -1;
    if (g_table[irq].fn)       return -1;
    g_table[irq].data = data;
    __atomic_store_n((uintptr_t *)&g_table[irq].fn, (uintptr_t)fn,
                     __ATOMIC_RELEASE);
    return 0;
}
```

This is what chapter 2's `nx_console_init` called. Two checks
upfront — IRQ in range, slot not already taken — then two
writes: the cookie first, then the function pointer with a
release store. The release ordering matters because the
dispatcher loads the function pointer with an acquire and then
reads the cookie; without the release/acquire pairing, the
dispatcher could see a new function pointer paired with a stale
cookie. Same memory-ordering rule chapter 2 explained for the
RX ring, applied here.

The dispatcher itself:

```c
void irq_dispatch(void)
{
    unsigned int irq = gic_ack();
    /* IAR returns 1022 for spurious and 1023 for special group —
     * neither requires EOI. */
    if (irq >= 1020)
        return;

    irq_handler_t fn = NULL;
    if (irq < IRQ_TABLE_SIZE)
        fn = (irq_handler_t)__atomic_load_n(
                 (uintptr_t *)&g_table[irq].fn, __ATOMIC_ACQUIRE);

    if (fn) {
        fn(g_table[irq].data);
    } else {
        kprintf("[irq] unhandled IRQ %u — masking\n", irq);
        gic_disable(irq);
    }

    gic_eoi(irq);
}
```

Five steps:

1. **Acknowledge.** Ask the GIC which IRQ it's offering us. The
   GIC returns the IRQ number and switches its internal state to
   "this IRQ is being handled".
2. **Filter spurious.** IRQ numbers 1020–1023 are reserved by the
   GIC for special signals (mostly "nothing was actually
   pending"). They don't need an EOI; we just bail.
3. **Look up the handler.** Acquire-load the function pointer for
   this IRQ from the table.
4. **Run the handler — or warn.** If a driver registered one,
   call it with the cookie. If nothing's registered, log and
   *disable* this IRQ at the GIC so it doesn't keep firing into
   a missing handler.
5. **EOI.** Tell the GIC we're done. The GIC is now free to
   deliver another IRQ.

That's the whole dispatcher: 20 lines. The `(void)data` argument
becomes whatever the driver passed at register time — for the
PL011 we passed 0, so the ISR ignores it.

---

## Masking IRQs with DAIF

There's one more piece: how the kernel **prevents** IRQs from
firing for stretches of code that aren't ready to handle them.

ARM64 has a 4-bit field in the CPU's status register called
**DAIF**. Each letter is one mask bit:

| Letter | Masks |
|--------|-------|
| **D** | Debug exceptions |
| **A** | SErrors (the "A" stands for "Asynchronous abort", an old name) |
| **I** | IRQs |
| **F** | FIQs |

Setting a bit *blocks* that exception kind on the current CPU.
Clearing it lets the exception through. The bits are independent;
you can mask IRQs while still letting FIQs fire, for example.

We mostly toggle the **I** bit. nonux provides two one-line
helpers in [`core/cpu/exception.h`](../core/cpu/exception.h):

```c
static inline void irq_enable_local(void)
{
    asm volatile("msr daifclr, #2" ::: "memory");
}

static inline void irq_disable_local(void)
{
    asm volatile("msr daifset, #2" ::: "memory");
}
```

`daifclr` and `daifset` are special "system register operands"
that take a 4-bit immediate — one bit per DAIF letter. The value
`2` is binary `0010`, which is the **I** position (DAIF letters
are arranged so `D=8, A=4, I=2, F=1`). So `msr daifclr, #2`
*clears* the I bit (enables IRQs), and `msr daifset, #2` *sets*
it (disables IRQs).

The `"memory"` clobber tells the C compiler "this instruction
might affect any memory" — without it, the compiler might reorder
loads and stores across the mask change, which would defeat the
purpose. (The same compiler hint shows up everywhere we want a
barrier between an MMIO operation and surrounding C code.)

### When IRQs are masked automatically

The CPU itself masks IRQs the moment an exception is taken. While
a stub is running and the C handler is executing, the **I** bit
is set; further IRQs are pending but not delivered. When `eret`
runs at the end of the stub, it restores the prior status from
`SPSR_EL1` — including the prior **I** state, which is whatever
the interrupted code had. So if the interrupted code had IRQs
enabled, they come back on automatically when we return.

That's why an ISR doesn't need to call `irq_disable_local`
manually: it's already in a no-IRQs region by virtue of being a
handler. (It also can't *afford* to enable IRQs mid-handler,
because the same line would re-fire the same interrupt
immediately and we'd recurse forever.)

### When the kernel masks IRQs by hand

Two situations:

- **During boot, before the IRQ-handling machinery is ready.** If
  an IRQ fired before `vectors_install` had run, the CPU would
  jump to whatever garbage was at the (still-zero) `VBAR_EL1`
  base. The bootloader leaves IRQs masked precisely so we can set
  things up safely. We only call `irq_enable_local` after
  everything else is in place.
- **Around critical sections that touch IRQ-shared state.** Any
  data that an ISR can also modify needs to be accessed with
  IRQs disabled, otherwise a partial update could be observed
  by the ISR. nonux uses this sparingly; the lock-free ring
  buffer in `framework/console.c` (chapter 2) avoids the need
  altogether for that case.

---

## The boot order

Let's put all these pieces in the order `boot_main` brings them
up, so the dependency graph is clear:

```c
vectors_install();
kprintf("[cpu]  exception vectors installed at %p\n", vectors);

gic_init();
kprintf("[gic]  distributor + CPU interface enabled\n");

timer_init(10);          /* sets up the per-CPU timer (later chapter) */
nx_console_init();       /* registers the PL011 IRQ — chapter 2 */

/* …framework + scheduler bring-up… */

irq_enable_local();      /* finally, let IRQs through */
```

The order matters:

1. **`vectors_install`** has to run before any IRQ could fire.
   With `VBAR_EL1` set, even an IRQ that arrives during the next
   line will at least land in our table and be saved properly —
   though it can't actually fire yet because IRQs are still
   masked locally.
2. **`gic_init`** wakes up the GIC. Until this runs, the GIC
   isn't accepting IRQs from any device.
3. **`timer_init`** and **`nx_console_init`** each call
   `irq_register(...)` for their own IRQ number and then
   `gic_enable(...)` for it. After this, the GIC will *try* to
   deliver the IRQs to the CPU, but they're still blocked at the
   CPU itself by the **I** mask.
4. **`irq_enable_local`** drops the **I** mask. From this point
   on, any pending IRQ flows through: GIC → CPU → vector table →
   stub → `on_irq` → `irq_dispatch` → registered handler.

Notice the funnel shape: we set up the table, the GIC, the
specific IRQs, and only at the very end do we open the gate.
That order means the first IRQ that fires lands in fully-built
machinery. It's deliberately the *last* thing `boot_main` does
before handing off to either the idle loop or the test runner.

---

## End-to-end: IRQ 33 once more

We can now redo chapter 2's "user types a key" diagram with no
hand-waving:

```
QEMU terminal: user presses 'a'
        │
        ▼
PL011 RX FIFO fills, chip raises its IRQ line
        │
        ▼
GIC distributor sees IRQ 33, sees enable=1, priority=0;
forwards to GIC CPU interface
        │
        ▼
GIC CPU interface signals IRQ to the CPU
        │
        ▼
CPU finishes the current instruction, takes the IRQ:
  - sets ELR_EL1 = address of the next instruction
  - sets SPSR_EL1 = current pstate
  - masks IRQs (DAIF.I = 1)
  - jumps to VBAR_EL1 + 0x280  (entry 5: current EL with SP_ELx, IRQ)
        │
        ▼
.balign 0x80 ⇒ b _irq_stub
        │
        ▼
SAVE_TRAPFRAME:    push x0..x30, SP_EL0, ELR_EL1, SPSR_EL1
mov x0, sp;        bl on_irq
        │
        ▼
on_irq(tf) → irq_dispatch()
        │
        ├── irq = gic_ack();           ← GICC_IAR returns 33
        ├── fn  = g_table[33].fn;      ← nx_console_rx_isr
        ├── fn(g_table[33].data);      ← drain RX FIFO into ring
        └── gic_eoi(33);               ← GICC_EOIR ← 33
        │
        ▼
return into _irq_stub
bl sched_check_resched   (no-op here unless a higher-pri task is ready)
RESTORE_TRAPFRAME        pops everything
eret                     restores ELR_EL1 → PC, SPSR_EL1 → pstate
        │
        ▼
CPU resumes the instruction it was about to run before the IRQ
```

Every step is now under the kernel's control. The arrow chapter 2
glossed over — "GIC delivers IRQ 33 to the CPU" — is really the
small chain in the middle: distributor enable + priority routing
on the way in, ack/EOI handshake on the way out, and a vector
table entry that knows how to save state and call into C.

---

## A few extra things to know

- **The "current EL with SP_EL0" group is dead code.** Entries
  0–3 of the vector table cover the case where the CPU is at EL1
  but using `SP_EL0` as its stack — the so-called "EL1t" mode.
  nonux always runs at EL1 with `SP_EL1` (mode "EL1h"), so those
  entries should never fire. We pointed them at `_unimpl_stub`
  out of paranoia; if one ever does fire, we'll know about it
  loudly instead of silently.

- **Sync-from-EL1 is also a kernel bug.** A data abort or an
  undefined instruction taken at EL1 means the kernel itself did
  something the CPU can't carry out — probably dereferenced a
  bad pointer or jumped through an uninitialized function
  pointer. `on_sync`'s default branch (which `halt_forever`s
  with a diagnostic) handles those. EL0-origin faults are
  recoverable: the *user program* misbehaved; the kernel kills
  it and keeps running. EL1-origin faults are not: the *kernel*
  misbehaved, and there's no caller to return an error to.

- **Hardware actually doesn't *have* a "vector table size"
  register.** ARM architectures with similar mechanisms vary on
  this — x86's IDT has an explicit length register, MIPS has a
  small fixed handler at one address. ARM64 makes the deal
  simple: 16 entries, fixed offsets, 2-KiB-aligned. The fixed
  layout means the CPU can compute the right offset arithmetically
  ("which group × 4 + which kind × 0x80") with no bounds check
  at all.

- **The unhandled-IRQ self-defense in `irq_dispatch`.** If an IRQ
  fires for which no driver registered, the dispatcher logs once
  and *disables* the IRQ at the GIC. Without that step, the GIC
  would keep delivering the same un-handle-able IRQ over and
  over, and `irq_dispatch` would burn the CPU on a loop of
  "nothing-to-do, EOI, immediate re-fire". This is the
  dispatcher's only self-protective behavior; everything else
  trusts the registration to be correct.

- **Why we chose GICv2 specifically.** The QEMU `virt` machine
  comes up with GICv2 by default for backwards compatibility —
  that's why we wrote a GICv2 driver. Newer ARM systems use
  GICv3, which uses *system registers* for the CPU interface
  instead of MMIO (better for many-CPU systems). The conceptual
  model — distributor + CPU interface, ack/EOI handshake, SGI/
  PPI/SPI numbering — is the same; the registers are different.
  Porting the driver to GICv3 is possible without changing
  anything in `core/cpu/` or `core/irq/irq.c`.

- **The "spurious" IRQ values 1022 and 1023.** When a CPU reads
  `IAR` and the GIC has nothing to deliver (a race between
  acknowledging and the device de-asserting its line), the GIC
  returns 1022 ("CPU not selected") or 1023 ("special group" /
  "no pending IRQ"). The dispatcher's `irq >= 1020` test covers
  these. The real-world rate is microscopic, but if it does
  happen we can't EOI a non-IRQ, so the early return is
  important.

- **`mov x0, sp` is more than convenience.** ARM64's calling
  convention puts the first argument in `x0`. By moving the
  current stack pointer (which now points at the freshly-saved
  trap frame) into `x0`, we hand the C function a typed pointer
  to the frame for free. No allocation, no copy. The same trap
  frame is read by the C handler and rewritten by
  `RESTORE_TRAPFRAME` from the same memory.

---

## Where to read more

- [`core/cpu/vectors.S`](../core/cpu/vectors.S) — the vector
  table, `SAVE_TRAPFRAME` / `RESTORE_TRAPFRAME`, and all four
  stubs.
- [`core/cpu/exception.c`](../core/cpu/exception.c) — the four
  C handlers, `vectors_install`, the local IRQ-mask helpers.
- [`core/irq/irq.c`](../core/irq/irq.c) — the dispatcher and the
  per-IRQ table.
- [`core/irq/gic.c`](../core/irq/gic.c) — the GICv2 driver.
- [`core/boot/boot.c`](../core/boot/boot.c) — the part of
  `boot_main` that brings these up in order.
- [Chapter 2 §"Setting up the IRQ — `nx_console_init`"](02-console-and-uart.md#setting-up-the-irq--nx_console_init)
  — the original use of `irq_register` and `gic_enable`, which
  this chapter explained the inside of.
- [Chapter 1 §"Privilege levels: EL0, EL1, EL2, and EL3"](01-boot-and-linker.md#privilege-levels-el0-el1-el2-and-el3)
  — exception levels and `eret`. This chapter assumed both.
- ARM Architecture Reference Manual for ARMv8-A (`developer.arm.com`,
  document DDI0487) — the deep technical reference for the vector
  table, every `ESR_EL1.EC` value, the exact behavior of `eret`,
  and DAIF semantics. Heavy reading, but the only authoritative
  source.
- ARM Generic Interrupt Controller v2 Architecture Specification
  (`developer.arm.com`, document IHI0048) — the canonical manual
  for everything in `core/irq/gic.c`. Lists every register, every
  bit, the exact ack/EOI rules, and the meaning of every
  spurious value.
- [QEMU's `virt` machine source](https://gitlab.com/qemu-project/qemu)
  (`hw/arm/virt.c` and `hw/intc/arm_gic.c`) — the GICv2 model in
  QEMU and the device-to-IRQ-number assignments. The PL011's
  SPI #1 → IRQ 33 mapping comes from there.
