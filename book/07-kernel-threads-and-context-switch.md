# Kernel threads and context switching

Chapter 4 left a thread hanging. The timer ISR sets a flag on the
running task called `need_resched`. On the way out of the IRQ
handler, somebody is supposed to notice the flag and switch to a
different task. That somebody calls a function named
`cpu_switch_to`. We sketched the chain, named the function, and
said "the full story is a later chapter".

This is that chapter. By the end you should know what a **task**
is in nonux, what its **kernel stack** looks like, what state a
**context switch** has to save and restore, exactly which CPU
registers move and which don't, how the kernel creates a new
**kernel thread** ("kthread") and arranges for it to start
running the right C function, and where the running task's
identity actually lives on the CPU (spoiler: a register called
`TPIDR_EL1`).

We will deliberately leave one big question for the next chapter:
*who decides which task runs next?* That is the **scheduler**,
and it gets its own chapter. Here we cover the mechanism — the
plumbing that lets two tasks coexist on one CPU. The policy that
picks between them is chapter 8.

The relevant files in this repo:

- [`core/sched/task.h`](../core/sched/task.h),
  [`core/sched/task.c`](../core/sched/task.c) — `struct nx_task`,
  the saved-context layout, `nx_task_create`, `nx_task_current`.
- [`core/cpu/context.S`](../core/cpu/context.S) — the ARM64
  context switch itself: `cpu_switch_to` and the first-switch
  thunk `nx_task_bootstrap`.
- [`core/sched/sched.c`](../core/sched/sched.c) —
  `sched_check_resched`, `nx_task_yield`,
  `sched_spawn_kthread`, the idle task. We use these here only
  as far as they involve switching; the policy side is chapter 8.
- [`framework/dispatcher.c`](../framework/dispatcher.c) — the
  framework's IPC kthread (`nx_disp`). One real, in-tree user
  of `sched_spawn_kthread`.
- [`core/boot/boot.c`](../core/boot/boot.c) — the call to
  `sched_start()` that turns the boot context itself into the
  **idle task**, plus (in the `NX_INIT_BUSYBOX` build) the call
  to `sched_spawn_kthread("init", …)`.

---

## Terms you'll see

- **Task.** nonux's name for "a thing the scheduler can run".
  Equivalent to Linux's "task" or "thread". A task has its own
  saved CPU state and its own **kernel stack**; everything else
  (the address space, file handles, signals) is shared through
  the task's enclosing **process** structure. In this chapter
  every task is a **kthread** — a task that lives entirely in
  the kernel, never drops to EL0, and is created by code calling
  `sched_spawn_kthread`.
- **Kernel thread / kthread.** A task whose entry point is a C
  function in kernel space. The framework's IPC dispatcher and
  the boot-time `init` runner are both kthreads. They are
  preempted, yield, and switch the same way as user-process
  tasks; the difference is they never `eret` to EL0.
- **Kernel stack ("kstack").** A private stack the kernel uses
  while executing on behalf of one specific task. Every task has
  its own. nonux allocates one page (4 KiB) per kthread from the
  PMM (chapter 5). The stack pointer (`sp`) is loaded from the
  task's saved context every time the task is switched in.
- **Context.** Everything about the CPU's state that distinguishes
  "task A running" from "task B running": general-purpose
  registers, the stack pointer, the exception mask bits, and the
  per-task pointer registers. On nonux this is captured by
  `struct nx_cpu_ctx`.
- **Context switch.** The act of saving one task's context,
  loading another's, and resuming the CPU as if the second task
  had never been interrupted. On ARM64 this is a small assembly
  routine — about thirty instructions — implemented in
  [`core/cpu/context.S`](../core/cpu/context.S).
- **Callee-saved register.** A register that the **AAPCS** (the
  ARM C calling convention) requires a function to preserve
  across a call. If your function uses one, you save it on entry
  and restore it on exit. On ARM64 the callee-saved general-
  purpose registers are `x19`-`x29` plus the link register `x30`.
  Everything else (`x0`-`x18`) is caller-saved.
- **Caller-saved register.** A register the AAPCS does *not*
  require a callee to preserve. If you care about its value
  across a call, *you* save it. The compiler spills these
  automatically at the call site.
- **AAPCS.** The Arm Architecture Procedure Call Standard.
  Defines which registers are caller- vs callee-saved, how
  arguments are passed (first eight in `x0`-`x7`), how returns
  work, and which register holds the return address. nonux,
  like every other ARM64 OS, follows it for every C call.
- **`TPIDR_EL1`.** A 64-bit system register on every ARM64 CPU
  named "Thread Pointer ID Register, EL1". The architecture
  reserves it for kernel use and never touches its contents
  itself. nonux uses it as "the pointer to the currently-running
  task on this CPU". Reading it tells you who you are; writing
  it during a context switch publishes the new identity.
- **`TPIDR_EL0`.** `TPIDR_EL1`'s userspace sibling. Per-CPU,
  saved/restored on switch, used by EL0 libcs (notably musl) to
  point at thread-local storage. We mention it once; the full
  story is in a later chapter.
- **First-switch thunk.** A small assembly stub
  (`nx_task_bootstrap`) that runs the *first* time a freshly-
  created task is switched into. It pulls the task's entry
  function and argument out of stashed registers and branches to
  `entry(arg)`. Every subsequent switch into the same task
  resumes inside `cpu_switch_to` itself, not the thunk.
- **`need_resched`.** A per-task flag meaning "this task wants
  to be replaced as soon as the kernel can safely do so". Set
  by the timer ISR (chapter 4) when a quantum expires, by
  `nx_task_yield`, and by wake/wait logic that's coming later.
  Read by `sched_check_resched`.
- **`preempt_count`.** A per-task counter meaning "do not
  preempt me right now". When greater than zero, the reschedule
  shim returns without switching. Nestable — `nx_preempt_disable`
  bumps the count, `nx_preempt_enable` drops it. Used by short
  critical sections that need to atomically observe scheduler
  state without losing the CPU.
- **Voluntary yield.** A task asking to give up the CPU. nonux's
  `nx_task_yield` sets `need_resched` on the current task and
  calls `sched_check_resched` directly — no IRQ needed.
- **Involuntary preemption.** A task losing the CPU because the
  timer ISR set `need_resched` while it was running. The switch
  happens on the way *out* of the timer IRQ, not inside the ISR.

---

## What is a task?

Before we look at the assembly, here is the picture in plain
English.

The CPU at any moment has one set of registers and one stack
pointer. It is executing one stream of instructions, on top of
one stack of saved-return-addresses and local variables. From
the CPU's point of view there is no such thing as "multiple
tasks running" — it can only do one thing at a time. The illusion
of two tasks running side by side comes from the kernel
*pausing* one of them — copying every relevant CPU register into
a memory structure tied to that task — and then loading another
task's saved registers off some other memory structure and
running for a while.

The memory structure that holds one task's pause-state is
`struct nx_task`. Every task has exactly one. When the scheduler
"runs task A", it loads the registers in A's `struct nx_task`
into the CPU. When it "switches from A to B", it copies the CPU's
current state back into A's struct and loads B's into the CPU.
That copy is the **context switch**.

The whole game is making that copy

- **correct** — every register the new task expected to see
  is restored to its value, every register the old task expected
  to keep is saved, nothing is dropped on the floor;
- **fast** — context switches happen on every timer tick that
  rotates the runqueue, plus every yield, plus every IRQ that
  wakes a higher-priority task. A bloated switch path is a
  scheduler-wide tax;
- **safe** — between "saving A" and "loading B" there's a brief
  window where the CPU's view of "which task is current" is
  inconsistent. Anything that runs in that window — a
  re-entrant IRQ, a memory access through a per-task register —
  has to be either masked or made not care.

The rest of the chapter is the cost of making all three true on
ARM64.

---

## `struct nx_task`

Open
[`core/sched/task.h`](../core/sched/task.h) and look at the
definition. We will trim it for now and rebuild it field-by-
field as each becomes relevant:

```c
struct nx_task {
    struct nx_cpu_ctx   cpu_ctx;          /* saved registers */
    uint32_t            id;
    char                name[NX_TASK_NAME_MAX];
    enum nx_task_state  state;
    int                 preempt_count;
    int                 need_resched;
    void               *kstack_base;
    size_t              kstack_size;
    /* … wait-queue, IPC, process pointers; covered in later chapters … */
};
```

Reading top to bottom:

- **`cpu_ctx`** — the saved general-purpose registers, stack
  pointer, and exception mask. The whole next section is about
  this. Note its position: *first field, offset zero*. That is
  deliberate; we'll explain why when we get to `cpu_switch_to`.
- **`id`** — a small integer name for the task, monotonically
  assigned by `nx_task_create`. Zero is reserved for the idle
  task; everything else starts at 1.
- **`name`** — a short human-readable label
  (`"idle"`, `"nx_disp"`, `"init"`). Carried around for log
  messages and for `ps`. Capped at 16 bytes including the NUL.
- **`state`** — one of `NX_TASK_READY` (on a runqueue,
  could run), `NX_TASK_RUNNING` (currently on a CPU),
  `NX_TASK_BLOCKED` (waiting on a wait queue), or
  `NX_TASK_ZOMBIE` (exited, waiting for a parent to reap it).
  The scheduler reads this when deciding what to do with the
  task.
- **`preempt_count`** — covered in §"Voluntary and involuntary
  switches" below. A counter, not a flag, so nested critical
  sections compose.
- **`need_resched`** — same section. A flag the ISRs set; the
  reschedule shim consumes.
- **`kstack_base`** and **`kstack_size`** — the kernel stack the
  task is currently running on. Set by `nx_task_create` to the
  PMM page allocated for it; freed by `nx_task_destroy`.

`struct nx_task` lives on the kernel heap (we'll talk about how
in §"Creating a task"). It has nothing to do with the kernel
stack it owns — the stack is a separate, page-sized PMM
allocation that the task pointer just remembers.

> **Side note: "task" vs "thread" vs "process".** Different
> systems carve up the same idea differently. POSIX talks about
> "processes" (an address space + at least one thread) and
> "threads" (a unit of scheduling inside a process). Linux's
> internal API calls everything a `task_struct` and lets threads
> share most of their parent's state. nonux follows Linux's
> naming: every schedulable thing is an `nx_task`, and what we'd
> traditionally call a "process" is a separate `nx_process`
> struct that one or more tasks can point at. A kthread points
> at the kernel's bookkeeping `g_kernel_process`; a busybox
> instance running at EL0 points at its own per-process struct.
> This chapter only touches the task half. Processes are
> chapter 15.

---

## The saved context: `struct nx_cpu_ctx`

A context switch has to save *some* CPU state. The question is
which. Save too much and every switch is slow. Save too little
and the switched-out task corrupts when it resumes. nonux's
answer is in
[`core/sched/task.h`](../core/sched/task.h):

```c
struct nx_cpu_ctx {
    uint64_t x19, x20;
    uint64_t x21, x22;
    uint64_t x23, x24;
    uint64_t x25, x26;
    uint64_t x27, x28;
    uint64_t x29, x30;   /* fp, lr */
    uint64_t sp;
    uint64_t daif;
} __attribute__((aligned(16)));
```

Eleven general-purpose registers, the stack pointer, and the
DAIF mask. That's it. No floating-point. No exception link
register. No `x0`-`x18`. Each choice has a reason.

### Why only the callee-saved registers

`cpu_switch_to` is called like a regular C function. The
scheduler driver dispatches to it with a normal `bl` (branch-
and-link) instruction. From the compiler's perspective, the
*caller* — the C code that just called `cpu_switch_to(prev,
next)` — has already done all the spilling required for a
function call. If the caller had a value in `x5` it wanted to
keep, the compiler has already moved that value somewhere safe
*before* the `bl`. That is what "caller-saved" means: it is the
caller's problem.

So when `cpu_switch_to` starts executing, the only registers
the *task being switched out* still cares about are the ones
the AAPCS says callees must preserve across calls. On ARM64
those are:

- `x19`-`x28` — general-purpose callee-saved.
- `x29` — the frame pointer (also callee-saved).
- `x30` — the link register: where the C function will return
  to. Also callee-saved by convention.

Eleven 64-bit values. Save those, restore those, and as far as
the C compiler is concerned the task has not lost a single bit
of useful state.

> **Side note: x0-x18 really are gone.** This is worth saying
> out loud because it feels suspicious the first time. When
> `cpu_switch_to` returns to the outgoing task, that task is
> *resuming inside a function call* — specifically inside the
> body of `sched_check_resched`. The compiler treats every
> function call as a barrier across which the caller-saved
> registers are clobbered. If the compiler had a meaningful
> value in `x5` before calling `cpu_switch_to`, it spilled
> `x5` to the stack first. After the switch returns, the
> compiler reloads it from the stack. We save nothing for `x5`
> because nobody is asking us to.

### Why the stack pointer

Each task has its own kstack. While task A is running, `sp` is
some address inside A's kstack page. While B is running, `sp`
is inside B's. Switching from A to B without restoring `sp`
would have B pushing values into A's stack — corrupting it,
and faulting later when B unwinds back through frames it never
pushed.

`sp` is its own register on ARM64 (not part of the `x*` file).
The context-save code reads it with `mov x2, sp`, the restore
writes it with `mov sp, x2`. We allocate one 8-byte slot at
offset `0x60` to hold it.

### Why DAIF

DAIF is a 4-bit mask in a 64-bit system register named
`DAIF`. The bits stand for **D**ebug, **S**Error, **I**RQ, and
**F**IQ — when a bit is set, that exception kind is masked
(blocked) at the CPU. From chapter 3 you'll recognise `I`: it
is what `irq_enable_local` / `irq_disable_local` flip.

Why does the *task* care about its own DAIF state? Because the
two paths that enter `cpu_switch_to` arrive with different
masks set:

- **An involuntary switch** happens inside the IRQ-return shim
  (`_irq_stub` from chapter 3, calling `sched_check_resched`
  between `on_irq` and `eret`). The shim runs with DAIF.I
  *masked* — that is the architectural default while the CPU
  is handling an exception. The task that gets switched out
  expected its IRQ mask to stay where it was, which is masked.
- **A voluntary yield** happens in plain C, with IRQs enabled.
  DAIF.I is clear. The task that yields expected to come back
  in the same state — IRQs still enabled.

If we did not save DAIF per task, an involuntary switch out of
A (masks set) followed by a future involuntary switch back into
A (which would inherit B's mask state, whatever that was) could
flip A's view of "are IRQs on?" between save and resume. The
worst case: a task voluntarily yields with IRQs enabled, comes
back with IRQs masked, and runs forever — the next tick will
fire and the GIC will queue it, but the CPU will never observe
it.

Saving DAIF makes the switch *transparent* to whoever was
running. They see the same DAIF on resume that they had when
they were paused.

### Why no floating-point

nonux's `Makefile` compiles the kernel with
`-mgeneral-regs-only`. The compiler is forbidden from emitting
NEON or floating-point instructions in kernel code. As a
consequence the kernel never *holds* a floating-point value
across any function call, including `cpu_switch_to`. The FP/SIMD
register file (`v0`-`v31`) may contain whatever a previously-
running EL0 program left there, but no kernel code is reading
it.

EL0 code does use FP/SIMD. nonux deals with that by saving and
restoring the FP/SIMD file at *EL0-to-EL1* transitions, not at
context-switch time — the saved frame lives inside the
trap-frame from chapter 3, which is on the task's kstack between
the entry and the eventual `eret`. Two tasks that never reach
EL0 (every kthread) never touch the FP file at all.

> **Side note: real-world kernels often punt FP/SIMD until
> userspace asks for it ("lazy FP").** Linux on ARM64 marks
> the FP file as "owned" by the task that last touched it; the
> first FP instruction by a different task takes a fault that
> the kernel handles by spilling the previous owner's state
> and loading the new one's. nonux doesn't need that
> sophistication — our EL0 surface is small enough that
> saving the file on every EL0↔EL1 round-trip stays cheap.

### Why 16-byte alignment

`__attribute__((aligned(16)))` makes sure `nx_cpu_ctx` is laid
out so every `stp` / `ldp` pair in `cpu_switch_to` lands on a
16-byte boundary. ARM64's load-pair and store-pair instructions
require 16-byte alignment when the memory type is "Device" or
when `-mstrict-align` is in effect. The kernel runs with the
MMU on and Normal-memory mapped for RAM (chapter 6), so the
alignment is technically permissive, but the alignment also
satisfies AAPCS's stack-pointer rule (also 16). It's free
correctness.

---

## `cpu_switch_to`: the assembly

Here is the whole function from
[`core/cpu/context.S`](../core/cpu/context.S):

```asm
.global cpu_switch_to
cpu_switch_to:
    /* Save prev's context. */
    stp     x19, x20, [x0, #0x00]
    stp     x21, x22, [x0, #0x10]
    stp     x23, x24, [x0, #0x20]
    stp     x25, x26, [x0, #0x30]
    stp     x27, x28, [x0, #0x40]
    stp     x29, x30, [x0, #0x50]
    mov     x2, sp
    str     x2,       [x0, #0x60]
    mrs     x2, daif
    str     x2,       [x0, #0x68]

    /* Load next's context. */
    ldp     x19, x20, [x1, #0x00]
    ldp     x21, x22, [x1, #0x10]
    ldp     x23, x24, [x1, #0x20]
    ldp     x25, x26, [x1, #0x30]
    ldp     x27, x28, [x1, #0x40]
    ldp     x29, x30, [x1, #0x50]
    ldr     x2,       [x1, #0x60]
    mov     sp, x2
    ldr     x2,       [x1, #0x68]
    msr     daif, x2

    /* Publish new "current task" via TPIDR_EL1. */
    msr     tpidr_el1, x1

    ret
```

Twenty-two instructions. Walk it.

### The arguments

The function signature in C is

```c
void cpu_switch_to(struct nx_task *prev, struct nx_task *next);
```

By AAPCS, the first two arguments arrive in `x0` and `x1`. So
when `cpu_switch_to` starts executing:

- `x0` = pointer to the outgoing task.
- `x1` = pointer to the incoming task.

### Why the cpu_ctx is at offset zero

Recall from `task.h`:

```c
struct nx_task {
    struct nx_cpu_ctx   cpu_ctx;          /* offset 0 — pinned */
    /* … */
};

_Static_assert(offsetof(struct nx_task, cpu_ctx) == 0,
               "nx_task.cpu_ctx must be at offset 0 — "
               "cpu_switch_to depends on it");
```

That pin is so the assembly can treat `x0` (a `struct nx_task *`)
and `x0 + 0` (a `struct nx_cpu_ctx *`) interchangeably. Every
`stp ... [x0, #0xNN]` is indexing *into the embedded cpu_ctx*,
not adding any offset for "first walk past task metadata". If
we ever added a field *before* `cpu_ctx`, every `stp`/`ldp` in
this file would need patching. The `_Static_assert` catches
that at compile time.

### The save half

```asm
    stp     x19, x20, [x0, #0x00]
    stp     x21, x22, [x0, #0x10]
    ...
    stp     x29, x30, [x0, #0x50]
```

`stp` is "store pair". `stp x19, x20, [x0, #0x00]` stores the
64-bit values of `x19` and `x20` to consecutive 8-byte slots
starting at `x0 + 0`. The six `stp` instructions cover all
eleven callee-saved registers (`x19`-`x30`) in twelve slots.

```asm
    mov     x2, sp
    str     x2,       [x0, #0x60]
```

`mov x2, sp` reads the stack pointer into `x2`, because `str`
can't take `sp` as a source directly. Then `str` writes it to
the `sp` slot in `cpu_ctx`. Note we are scribbling `x2` here —
`x2` is caller-saved, so the AAPCS allows us to use it as
scratch.

```asm
    mrs     x2, daif
    str     x2,       [x0, #0x68]
```

`mrs x2, daif` reads the DAIF mask system register into `x2`.
`str` parks it in the DAIF slot. After these eight slots are
populated, `*x0` is a complete snapshot of the outgoing task's
CPU state.

### The load half

```asm
    ldp     x19, x20, [x1, #0x00]
    ...
    ldp     x29, x30, [x1, #0x50]
    ldr     x2,       [x1, #0x60]
    mov     sp, x2
    ldr     x2,       [x1, #0x68]
    msr     daif, x2
```

Same shape, opposite direction. `ldp` is "load pair" — load
two consecutive 8-byte values into the named register pair.
After this block, the CPU's callee-saved registers, SP, and
DAIF are exactly what `next` had when it was last saved.

There is a subtle moment between `mov sp, x2` and the next
load. SP just changed: we are now executing on `next`'s kstack
instead of `prev`'s. *No one notices*, because in between we
read no values from the stack — `x2` is the only thing the
load half scribbles, and it is a scratch register. The trick is
working only because the function is small enough that everything
lives in registers.

### Publishing the new identity

```asm
    msr     tpidr_el1, x1
```

`TPIDR_EL1` is the per-CPU "current task" pointer. `msr` writes
`x1` (the incoming task pointer) into it. From this instruction
on, any code on this CPU that calls `nx_task_current()` —
including the timer ISR on the very next tick — will see
`next`, not `prev`. We make the publication after the register/
SP load (so by the time anyone observes it, the CPU is actually
running on `next`'s state) but before the `ret` (so it lands
on `next`'s kstack, not `prev`'s).

### The return

```asm
    ret
```

`ret` is "branch to the value in the link register `x30`".
There are two values `x30` could hold at this point, and they
correspond to two very different histories of the incoming
task.

**Case 1 — `next` has run before.** Some earlier call to
`cpu_switch_to` saved `next` while *it* was the `prev` argument
to that call. The saved `x30` was the return address from that
call — i.e., the instruction after the `bl cpu_switch_to` in
whatever C code called the switch. The current `ret` therefore
returns into the body of `sched_check_resched` at the line right
after its `cpu_switch_to(curr, next)` call, on `next`'s kstack.
The illusion is complete: from `next`'s perspective, the call
to `cpu_switch_to` it made some-time-ago has finally returned.

**Case 2 — `next` is brand new and has never run before.** Its
`cpu_ctx` was hand-crafted by `nx_task_create` so that `x30`
points at a special thunk called `nx_task_bootstrap`. The `ret`
branches there instead. We will see what the thunk does in the
next two sections.

---

## `TPIDR_EL1`: who am I?

We have been using `nx_task_current()` since chapter 4 without
quite saying what backs it. Here is the kernel implementation:

```c
struct nx_task *nx_task_current(void)
{
    struct nx_task *t;
    asm volatile("mrs %0, tpidr_el1" : "=r"(t));
    return t;
}
```

That is the whole function. `mrs %0, tpidr_el1` reads the
`TPIDR_EL1` system register into a general-purpose register
that the compiler chose for us.

ARM64 reserves `TPIDR_EL1` for software use. The CPU itself
never reads or writes it; the kernel can put whatever 64-bit
value it likes there. nonux uses it for a single purpose:
"pointer to the currently-running task struct on this CPU".

There are two reasons for the register-rather-than-global
choice:

1. **Per-CPU for free.** On multi-core machines each CPU has
   its own copy of every system register. CPU 0 and CPU 1 each
   hold their own `TPIDR_EL1` value, so "the current task on
   *this* CPU" works without any locking, list lookup, or per-
   CPU indexing array. The kernel just reads `TPIDR_EL1`. nonux
   today runs single-core on QEMU `virt`, but the design ports
   forward to SMP unchanged.

2. **Cheap.** `mrs` is one cycle, in-register, with no memory
   access. A global "current task pointer" in DRAM would cost
   a load on every read, which adds up — `nx_task_current()`
   is called from `sched_tick`, `sched_check_resched`,
   `nx_preempt_disable`, every IPC call, and a dozen other
   places. The `mrs` path stays out of the cache hierarchy
   entirely.

`TPIDR_EL1` gets written in exactly two places in nonux:

- Inside `cpu_switch_to`, with the new task pointer
  (`msr tpidr_el1, x1` — we just saw it).
- Inside `sched_start`, where the boot CPU adopts the static
  idle task as its initial identity:
  ```c
  asm volatile("msr tpidr_el1, %0" :: "r"(&g_idle_task));
  ```
  That call lands in `boot_main`, right after the framework
  bootstrap and right before `irq_enable_local`. From that
  moment on, `nx_task_current()` returns *something* on every
  CPU that has booted. Before that moment, it can return zero
  or garbage; the code that calls it early guards on
  `if (!curr) return;`.

---

## Creating a task: `nx_task_create`

Now we can read
[`nx_task_create`](../core/sched/task.c) the way it deserves to
be read. The function takes a name, an entry function, an
argument, and a kstack size in pages. It returns a fully-formed
`struct nx_task *` that the scheduler can enqueue. Let's walk
it.

```c
struct nx_task *nx_task_create(const char *name,
                               void (*entry)(void *),
                               void *arg,
                               size_t kstack_pages)
{
    if (!entry) return NULL;

    struct nx_task *t = alloc_task_struct();
    if (!t) return NULL;

    void *stack = alloc_kstack(kstack_pages);
    if (!stack) {
        free_task_struct(t);
        return NULL;
    }
    /* … */
```

`alloc_task_struct` is a `calloc(1, sizeof *t)` from the kernel
heap (chapter 5's `kheap`). `alloc_kstack` is
`pmm_alloc_pages(kstack_pages)` — straight from the PMM, the
same way chapter 5 ended. We end up with a task struct
somewhere in the heap and a kstack starting at some PMM page.

Skipping past the bookkeeping fields (id, name, state, the
list-node pointers), the interesting part is the cpu_ctx
fabrication:

```c
    uintptr_t sp_top = (uintptr_t)stack + t->kstack_size;
    sp_top &= ~(uintptr_t)0xf;        /* 16-byte align */

    t->cpu_ctx.x19 = (uint64_t)(uintptr_t)entry;
    t->cpu_ctx.x20 = (uint64_t)(uintptr_t)arg;
    t->cpu_ctx.x29 = 0;               /* FP */
    t->cpu_ctx.x30 = (uint64_t)(uintptr_t)&nx_task_bootstrap;
    t->cpu_ctx.sp   = (uint64_t)sp_top;
    t->cpu_ctx.daif = 0;              /* IRQs enabled */
```

Three things are happening.

### 1. Pick a starting stack pointer

`sp_top = stack + kstack_size`, then mask off the bottom four
bits to keep AAPCS's 16-byte alignment. ARM64 stacks grow
downward — every push subtracts from `sp` — so `sp_top` is the
*top* (highest address) of the new stack, which is where new
pushes should start.

### 2. Stash the entry function and argument

We do not put them in `x0`. `x0` is caller-saved; `cpu_switch_to`
will not restore it. We park them in `x19` and `x20`, which are
callee-saved and *will* be restored.

That's the whole reason `nx_task_bootstrap` exists. The C
function we are spawning expects its argument in `x0`. But
`cpu_switch_to` only restores `x19`-`x30`. We can't reach `x0`
through cpu_ctx. So we put the argument in a register that
`cpu_switch_to` *will* hand to us, and then the bootstrap thunk
moves it into `x0` once we're inside it. The thunk is a
register-shuffle adapter between "what the switch path delivers"
and "what AAPCS expects at a function call site".

### 3. Set the link register to the bootstrap thunk

`x30` is the return address. When `cpu_switch_to` finishes its
load half and executes `ret`, it branches to wherever `x30`
points. We want it to point at the thunk, so we put
`&nx_task_bootstrap` there.

`x29` (the frame pointer) is zero. The thunk does not use it,
and starting the call chain with `fp = 0` is the AAPCS-blessed
"this is the bottom of a stack" sentinel that walks (debuggers,
unwinders) recognise as "stop here". Every backtrace stops at
this task's bottom frame because `fp` is zero.

`daif = 0` means IRQs are enabled when the task first starts
running. That is what we want for a fresh kthread: it should be
preemptible by the timer.

The function returns `t`. The task is now allocated, has a
kstack, has a hand-crafted cpu_ctx — but it is not yet running
anywhere. Nothing has enqueued it on the scheduler. That is
`sched_spawn_kthread`'s job, two sections down.

---

## The first-switch thunk: `nx_task_bootstrap`

The bottom of
[`core/cpu/context.S`](../core/cpu/context.S) holds:

```asm
.global nx_task_bootstrap
nx_task_bootstrap:
    mov     x0, x20
    blr     x19

1:  wfe
    b       1b
```

Four meaningful instructions. Walk them:

- `mov x0, x20` — move the saved argument (which was loaded into
  `x20` by the last `ldp` of `cpu_switch_to`) into `x0`, the
  AAPCS first-argument register. Now we are AAPCS-correct.
- `blr x19` — "branch with link, register". Branch to the
  address in `x19` (which is the entry function), saving the
  return address (the instruction after `blr`) in `x30`. From
  the C code's perspective, it has just been called with `entry`
  as the function and `arg` as the first parameter.

If the C entry function ever *returns*, we fall through to:

- `wfe` / `b 1b` — wait-for-event in a loop. nonux's slice 4.1
  did not yet have an exit-this-thread primitive; a kthread that
  returned from its entry would simply park here. Later slices
  added `nx_task_exit` and the scheduler's reap path, but the
  thunk still backstops the case where a buggy or pre-exit
  kthread does return.

The thunk runs *once per task*. Every subsequent context switch
into the same task lands on whatever `x30` was at its last
*save* — which is always inside `cpu_switch_to`'s call site in
`sched_check_resched`, because that is the only path that ever
saves a `cpu_ctx`. So the thunk is, by construction, only ever
visited on the very first switch in.

Putting it together, the first ten or so events in a brand new
kthread's life look like this:

```
  nx_task_create:
    - alloc task struct
    - alloc kstack
    - cpu_ctx.x19 = entry
    - cpu_ctx.x20 = arg
    - cpu_ctx.x30 = &nx_task_bootstrap
    - cpu_ctx.sp  = top of new kstack
       │
       ▼
  (some time later, scheduler picks this task)
       │
       ▼
  cpu_switch_to(prev, new_task):
    - save prev's regs into prev->cpu_ctx
    - load new_task->cpu_ctx into the CPU
    - msr tpidr_el1, new_task
    - ret  →  jumps to x30 == &nx_task_bootstrap
       │
       ▼
  nx_task_bootstrap:
    - mov x0, x20         (arg into AAPCS slot)
    - blr x19             (call entry(arg))
       │
       ▼
  entry(arg):
    - the C function the caller asked for, now running
      on its own kstack with the right argument
```

If `entry(arg)` later does a `nx_task_yield`, the chain runs in
reverse: `yield` calls `sched_check_resched`, which calls
`cpu_switch_to(curr, next)`, which saves `curr`'s context
(including the return address into `entry`, somewhere after the
`yield` call) and loads `next`'s. The yielding task's `x30`
now points back into its own C code. Whenever some later
switch selects it again, `cpu_switch_to`'s `ret` will resume
inside `entry`, not the thunk. The thunk is invisible from
here on.

---

## `sched_spawn_kthread`: the public door

The actual API the rest of the kernel uses is in
[`core/sched/sched.h`](../core/sched/sched.h):

```c
struct nx_task *sched_spawn_kthread(const char *name,
                                    void (*entry)(void *),
                                    void *arg,
                                    struct nx_process *process);
```

The implementation:

```c
struct nx_task *sched_spawn_kthread(const char *name,
                                    void (*entry)(void *),
                                    void *arg,
                                    struct nx_process *process)
{
    struct nx_task *t = nx_task_create(name, entry, arg, 1);
    if (!t) return NULL;
    if (process) t->process = process;
    if (g_sched_ops) {
        int rc = g_sched_ops->enqueue(g_sched_self, t);
        if (rc != 0) {
            nx_task_destroy(t);
            return NULL;
        }
    }
    return t;
}
```

Three steps:

1. **Create the task.** `nx_task_create` with `kstack_pages = 1`
   (one 4 KiB page is plenty for any in-tree kthread).
2. **Set the owning process if the caller specified one.**
   `process` is `NULL` for tasks that should inherit the
   caller's process (every framework kthread); non-`NULL` for
   tasks that will later drop to EL0 in a specific address
   space (the `init` runner). This is mostly a chapter-15
   concern; for now treat `NULL` as "fine, use defaults".
3. **Hand it to the scheduler.** `g_sched_ops->enqueue` adds the
   task to the runqueue. As soon as the scheduler picks it,
   it runs.

What ends up calling this in real builds:

- [`framework/dispatcher.c`](../framework/dispatcher.c) calls
  `sched_spawn_kthread("nx_disp", nx_dispatcher_kthread_entry, …)`
  during framework bootstrap. That kthread becomes the central
  IPC pump (chapter 11).
- [`core/boot/boot.c`](../core/boot/boot.c), in the
  `NX_INIT_BUSYBOX` build, calls
  `sched_spawn_kthread("init", nx_init_busybox_kthread, …)`.
  The init kthread eventually drops to EL0 and becomes the
  busybox shell.
- Many kernel tests spawn helper kthreads to exercise
  concurrency edge cases — see
  [`test/kernel/ktest_waitq.c`](../test/kernel/ktest_waitq.c)
  for half a dozen examples.

There is one kthread you never see spawned through
`sched_spawn_kthread`: the **idle task**. It is a special case.

---

## The idle task: a switch *out of*, never *into* first

The idle task lives in
[`core/sched/sched.c`](../core/sched/sched.c) as a static
global:

```c
struct nx_task g_idle_task;
```

It is not heap-allocated. It does not have its own kstack. It
*is* the boot CPU's context. By the time `sched_start` runs,
the kernel has already booted, set up the MMU, registered
interrupts, brought up the framework — it is running on the
boot stack, in a single thread of execution that has no name.

`sched_start` gives that nameless thread a name:

```c
void sched_start(void)
{
    if (g_sched_started) return;
    g_sched_started = true;

    idle_task_init();

    asm volatile("msr tpidr_el1, %0" :: "r"(&g_idle_task));

    if (g_sched_ops)
        g_sched_ops->enqueue(g_sched_self, &g_idle_task);
}
```

Three things:

1. **Initialise the static struct.** Name it `"idle"`, mark it
   `NX_TASK_RUNNING`, give it `id = 0`, zero the wait/deadline
   links. Notably, do *not* allocate a kstack — the kstack is
   the one boot started on, owned by the linker script.
2. **Adopt it as `TPIDR_EL1`.** From this `msr` on, any code
   that calls `nx_task_current()` returns `&g_idle_task`. The
   thread of execution that boot started in now has an
   identity, retroactively.
3. **Enqueue it.** The scheduler now considers `idle` as a
   runnable task. It will become the fallback `pick_next`
   returns when no other task is ready (chapter 8 explains the
   policy half).

The very first context switch the kernel ever performs goes
`idle → some_kthread`. `idle`'s cpu_ctx gets populated at that
switch's save half — that is the first time anyone writes
anything to `g_idle_task.cpu_ctx`. The fields are garbage
before that, but no one ever reads them before the save, so
"garbage" is fine.

The reverse switch — `some_kthread → idle` — lands at whatever
`g_idle_task.cpu_ctx.x30` holds, which is *the return address
from the original `cpu_switch_to` call that switched away from
idle*. In practice that means `idle` "resumes" at the line right
after the `cpu_switch_to` call in `sched_check_resched`,
returns out of it, and ends up back in `boot_main`'s
`for (;;) { asm volatile("wfi"); …}` loop or in `ktest_main`
(depending on build).

Idle is the only task that uses the boot kstack. Every other
task `sched_spawn_kthread` makes gets its own PMM page. If you
want a clean mental model: idle is the kernel's *boot thread*
in disguise, scheduled the same way as any other task, but with
the kstack it inherited from `start.S`.

---

## Voluntary and involuntary switches

We can finally answer chapter 4's loose end: *who calls
`cpu_switch_to`?* The answer is two places, both routed through
`sched_check_resched`.

### Involuntary preemption: the IRQ-return path

Chapter 3's `_irq_stub` looks like:

```asm
_irq_stub:
    SAVE_TRAPFRAME
    mov     x0, sp
    bl      on_irq
    bl      sched_check_resched
    RESTORE_TRAPFRAME
    eret
```

Between `on_irq` (which dispatched whatever IRQ fired) and the
`RESTORE_TRAPFRAME / eret` pair (which returns to the
interrupted task), the stub calls `sched_check_resched`. If the
ISR set `need_resched` on the current task — the timer ISR is
the classic case from chapter 4 — `sched_check_resched` decides
to switch. The relevant slice of it:

```c
void sched_check_resched(void)
{
    struct nx_task *curr = nx_task_current();
    if (!curr) return;
    if (!g_sched_ops) return;

    /* … signal-delivery checks omitted, covered later … */

    if (!curr->need_resched) return;
    if (curr->preempt_count > 0) return;

    curr->need_resched = 0;
    g_sched_ops->yield(g_sched_self);
    struct nx_task *next = g_sched_ops->pick_next(g_sched_self);
    if (!next || next == curr) {
        asm volatile("wfi");
        return;
    }

    /* … context-switch hook, TTBR0 flip, TPIDR_EL0 swap; later chapters … */

    cpu_switch_to(curr, next);
}
```

Four guards, then the work. Two guards we have already named:
`need_resched` is the flag and `preempt_count > 0` is the
"don't touch me right now" suppression. If both clear the way,
we ask the policy to rotate and pick, and then we switch. The
`wfi` branch is for the case where the only "runnable" task is
the one we're already on — sleeping until the next IRQ is
better than spinning. The omitted-for-later code (hook
dispatch, address-space flip, EL0 thread-pointer swap) does not
affect the basic mechanism; it adds work *around* the switch.

After `cpu_switch_to(curr, next)` returns — which it will, when
something switches back into `curr` later — control flows back
through `bl sched_check_resched`, `RESTORE_TRAPFRAME`, `eret`,
and the original interrupted task resumes wherever it was. Just
in two real-world-time chunks separated by however long the
other tasks needed.

### Voluntary yield

The other entry to `cpu_switch_to` is the explicit yield:

```c
void nx_task_yield(void)
{
    struct nx_task *curr = nx_task_current();
    if (!curr) return;
    curr->need_resched = 1;
    sched_check_resched();
}
```

Set the flag, call the same shim. From the shim's perspective
there is no difference between "the timer set the flag and the
IRQ-return path called me" and "the task itself set the flag
and called me". Both paths funnel through the same
`cpu_switch_to`.

What changes is *DAIF on entry*:

- The IRQ-return path arrives with DAIF.I masked (the CPU is
  still in IRQ-handling state). The saved `cpu_ctx.daif`
  reflects that. Whenever this task next runs, the restore
  half loads "I masked" into DAIF, and the `RESTORE_TRAPFRAME
  / eret` that eventually completes the resumption clears
  DAIF.I as part of restoring the trap frame's saved PSTATE.
- The voluntary path arrives in plain C, with DAIF.I clear.
  The saved `cpu_ctx.daif` carries that. Whenever this task
  next runs, the restore half loads "I clear" — but that's
  fine, because `cpu_switch_to`'s `ret` returns directly into
  the body of `nx_task_yield`, which expects IRQs enabled
  exactly as the task left them.

Saving DAIF per task is what makes those two paths coexist.

### `preempt_count`: don't switch right now

Sometimes a piece of kernel code needs to atomically observe
two scheduler fields, or to walk a list the scheduler also
walks, or to publish a value that another task might
immediately consume. In any of those windows, an involuntary
switch in the middle would corrupt the observation. The fix is
a per-task counter, not a global lock:

```c
void nx_preempt_disable(void)
{
    struct nx_task *t = nx_task_current();
    if (!t) return;
    t->preempt_count++;
}

void nx_preempt_enable(void)
{
    struct nx_task *t = nx_task_current();
    if (!t) return;
    if (t->preempt_count > 0)
        t->preempt_count--;
}
```

And, in the reschedule shim:

```c
if (curr->preempt_count > 0) return;
```

While `preempt_count > 0`, the shim returns without switching.
The timer ISR can still fire — it sets `need_resched` and
walks back out — but the switch is deferred. As soon as the
caller drops the count to zero, the next time it crosses a
reschedule point (an IRQ return, an explicit
`sched_check_resched`), the deferred switch finally happens.

Nesting works because it is a counter: A disables, B disables,
B enables (count goes back to 1, switch still suppressed), A
enables (count to 0, switch now allowed). No coordination
between A and B needed.

---

## A worked example: two tasks taking turns

Here is the full picture for two kthreads, A and B, running on
nonux with a round-robin scheduler (chapter 8) and a 10 Hz
tick (chapter 4).

Setup: both kthreads were created by `sched_spawn_kthread` and
enqueued. Idle is also on the runqueue. The scheduler's
`pick_next` returns them in rotation.

```
t=0       boot CPU = idle (after sched_start)
          runqueue: [idle, A, B]
          tick fires →
            sched_tick → policy decrements idle's remaining → 0
            policy sets idle.need_resched
            IRQ-return → sched_check_resched
                pick_next → A
                cpu_switch_to(idle, A)
                   save: idle.cpu_ctx ← current registers
                   load: A.cpu_ctx → CPU registers
                   tpidr_el1 ← A
                   ret → A's x30 == &nx_task_bootstrap (first switch)
                bootstrap → mov x0, x20 ; blr x19 → A_entry(arg)

t=100ms   A is running its entry body (some loop)
          tick fires →
            ISR runs on A's kstack
            sched_tick → policy decrements A's remaining → 0
            policy sets A.need_resched
            IRQ-return → sched_check_resched
                pick_next → B
                cpu_switch_to(A, B)
                   save: A.cpu_ctx ← current registers
                        (A's x30 points just past sched_check_resched's
                         cpu_switch_to call site)
                   load: B.cpu_ctx → CPU registers
                   tpidr_el1 ← B
                   ret → B's x30 == &nx_task_bootstrap (first switch for B)
                bootstrap → B_entry(arg)

t=200ms   B is running
          tick fires; same shape; switches to idle (next pick)

t=300ms   idle is running (wfi loop)
          tick fires; switches back to A
                pick_next → A
                cpu_switch_to(idle, A)
                   save: idle.cpu_ctx ← current registers
                   load: A.cpu_ctx → CPU registers
                   tpidr_el1 ← A
                   ret → A's x30 == return into sched_check_resched
                         (NOT the bootstrap thunk this time!)
            A resumes inside sched_check_resched, returns from it,
            returns from IRQ stub, eret restores the trap frame
            A took at t=100ms, A's instruction stream picks up
            exactly where the timer interrupted it.
```

After the first switch each, the bootstrap thunk never runs
again. The thunk is a one-time bridge from "fresh cpu_ctx
fabricated in `nx_task_create`" to "C code calling other C
code". Once the task has been saved at least once, every
resume goes back into its own C body.

---

## A few extra things to know

- **The kstack is per-task, not per-CPU.** Some kernels keep a
  single per-CPU "kernel stack" that every entry into EL1
  reuses (Linux on x86 in fixmap-stacks mode is a textbook
  example). nonux gives every task its own. Trade-offs: per-CPU
  stacks save memory but force kernel code to never block on
  the stack (a `wait` call would mean another task running on
  the same CPU now has to walk a different stack); per-task
  stacks use one page per task but let any kernel code in any
  task block safely. nonux opts for per-task because it makes
  the IPC story (chapter 11) much simpler.

- **One page is small.** 4 KiB is enough for ten or twenty C
  call frames in nonux's actual workloads, which is plenty for
  the dispatcher and the syscall path but would overflow on
  any recursive structure. The choice is deliberate: a stack
  overflow becomes a fault we can investigate (the next page
  is unmapped or kernel data), and the lesson is "don't
  recurse in the kernel". When a real-world kthread needs more
  (the busybox `init` runner is borderline), we can grow it to
  two pages by passing `kstack_pages = 2`.

- **`cpu_switch_to(curr, curr)` is legal but wasteful.** Saving
  a task's context into itself and then loading it back from
  itself is a 22-instruction no-op. nonux's shim guards
  against it explicitly (`if (!next || next == curr) { wfi;
  return; }`) so the case never reaches the assembly. The
  function would tolerate it if it did — every store has a
  matching load to the same memory — but the wfi path
  preserves battery and reduces tick-rate noise.

- **The trap frame and the cpu_ctx are two different things.**
  When an EL0 program is interrupted into EL1, chapter 3's
  `SAVE_TRAPFRAME` pushes 31 general-purpose registers plus
  PSTATE/ELR onto the *kernel* stack — that's the **trap
  frame**. When *that* task is then context-switched out, the
  switch saves only the 11 callee-saved registers (plus SP and
  DAIF) into the *task's* `cpu_ctx`. Both saves can coexist:
  the trap frame sits at the top of the task's kstack, and the
  cpu_ctx (a separate struct) sits on the heap inside the task
  struct. When the task is later switched back in, the cpu_ctx
  restore reactivates the kstack pointer, which still has the
  trap frame on top; the eventual `RESTORE_TRAPFRAME / eret`
  walks that frame back to EL0. Two layers, two scopes.

- **First field at offset zero is a load-bearing invariant.**
  The `_Static_assert` in `task.h` exists because someone
  *will* add a field to `struct nx_task` someday. If they put
  it before `cpu_ctx` without thinking, every offset in
  `context.S` becomes wrong, every save and load shifts, and
  the kernel breaks in spectacular ways — the kind of bug
  where TPIDR_EL1 ends up pointing into the middle of an
  unrelated field and the next call to `nx_task_current()`
  returns garbage. The assert catches it at compile time.

- **Host tests stub the assembly.** nonux runs much of its test
  suite on x86-64 hosts, where `cpu_switch_to` and
  `nx_task_bootstrap` cannot link (they are ARM64 assembly).
  [`core/sched/task.c`](../core/sched/task.c) defines a host
  stub:
  ```c
  void cpu_switch_to(struct nx_task *prev, struct nx_task *next)
  { (void)prev; (void)next; abort(); }
  ```
  Host tests never reach this — the precondition in
  `sched_check_resched` (`need_resched && !preempt_count`)
  isn't triggered from any host test path. The stub exists so
  the linker is happy. Anything that actually exercises
  context switching runs in QEMU as a kernel test.

- **There is no "thread of execution" before sched_start.** All
  the boot code before `sched_start` runs without `TPIDR_EL1`
  set. `nx_task_current()` returns whatever `TPIDR_EL1` happens
  to hold (typically zero on a cold boot). Every kernel
  function that calls it must therefore handle a `NULL`
  return — `nx_preempt_disable`, `nx_task_yield`,
  `sched_check_resched` all start with `if (!t) return;`. This
  is one of those small invariants that, if you forget,
  produces a fault somewhere baffling thousands of lines away.

- **The DAIF save/restore catches a subtle bug class.** Without
  it, the very first involuntary switch in the kernel's life
  would carry the ISR's masked-IRQ state into whatever task
  the scheduler picked. That task would run with IRQs disabled
  for a tick interval — not visibly catastrophic, but the
  scheduler would never preempt it, so a `for(;;);` in that
  task would hang the kernel forever. The fix is one register
  field. The bug class was fixed before it ever appeared in a
  shipped slice; the comment in `task.h` is what kept it
  fixed.

---

## Where to read more

- [`core/sched/task.h`](../core/sched/task.h) — `struct
  nx_task` and `struct nx_cpu_ctx`. Read the inline comments
  on register-slot offsets alongside `context.S`.
- [`core/sched/task.c`](../core/sched/task.c) —
  `nx_task_create`, `nx_task_destroy`. The `wire_caller_slot`
  block is IPC machinery — skip on first read; it's chapter 11.
- [`core/cpu/context.S`](../core/cpu/context.S) — the
  ARM64 switch. Worth reading the assembly twice: once for
  shape, once with the cpu_ctx offsets open in another window.
- [`core/sched/sched.c`](../core/sched/sched.c) — the
  reschedule shim. The TTBR0 flip and TPIDR_EL0 swap are EL0
  features covered in later chapters; for now `cpu_switch_to`
  itself is the load-bearing line.
- [Chapter 4 §"From flag to switch"](04-timer-and-ticks.md#from-flag-to-switch-where-the-actual-preemption-happens)
  — the timer-tick half of the involuntary-preemption story,
  which this chapter completes.
- [Chapter 3 §"The IRQ pipeline"](03-exceptions-gic-and-irqs.md#the-dispatcher-and-the-per-irq-table)
  — the IRQ-return path that calls `sched_check_resched`
  between `on_irq` and `eret`.
- ARM Architecture Reference Manual for ARMv8-A (DDI0487),
  section "AArch64 System Register Descriptions" — the
  canonical reference for `TPIDR_EL1`, `TPIDR_EL0`, and
  `DAIF`. The AAPCS document (Arm IHI 0055) is the canonical
  callee-vs-caller-saved register reference.
- [Linux `arch/arm64/kernel/entry.S`](https://elixir.bootlin.com/linux/latest/source/arch/arm64/kernel/entry.S)
  — Linux's ARM64 context switch, for comparison. nonux's
  switch is the same shape with fewer features (no
  speculation hardening, no kernel-mode FP).
