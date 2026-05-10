# The timer and ticks

Chapter 3 ended with a piece of machinery that can deliver any
IRQ from any device to any C function. The first IRQ we wired up
in earnest was the PL011's, fired by a human pressing a key. But
a kernel can't sit and wait for a human to do something — most of
what a kernel does is *unprompted*, and one of the things it does
most often is **measure time**. How long has this task been
running? Has its quantum expired? Has a timeout fired? Is anyone
still alive on this socket?

For all of that, the kernel needs a steady, predictable
**heartbeat** — an IRQ that fires on a fixed interval, on its
own, regardless of what userspace is doing. That heartbeat is
called the **timer tick**, and it's what this chapter walks
through.

By the end you should know what hardware on an ARM64 SoC
generates the tick, how it's programmed, what the kernel does
each time one fires, and how the same tick that updates a
counter also drives the scheduler to **preempt** a running task.

The relevant files in this repo:

- [`core/timer/timer.h`](../core/timer/timer.h),
  [`core/timer/timer.c`](../core/timer/timer.c) — the whole
  driver: `timer_init`, the tick ISR `on_tick`, `timer_ticks`,
  and the pause/resume helpers used during runtime
  recomposition.
- [`core/sched/sched.c`](../core/sched/sched.c) — `sched_tick`
  and `sched_check_resched`, the two functions every tick
  reaches into. We'll only touch the surface of the scheduler
  here; a later chapter does the deep dive.
- [`core/cpu/vectors.S`](../core/cpu/vectors.S) — the IRQ stub
  from chapter 3, which calls `sched_check_resched` between
  `on_irq` and `eret`.
- [`core/boot/boot.c`](../core/boot/boot.c) — the line
  `timer_init(10);` in `boot_main`.

---

## Terms you'll see

- **Timer tick (or just "tick").** A regularly-spaced IRQ the
  kernel arranges for itself. Every tick is one heartbeat: the
  ISR runs, updates the kernel's notion of "elapsed time", and
  drives the scheduler. nonux ticks at 10 Hz — once every
  100 milliseconds — by default.
- **ARM Generic Timer.** A clock-and-countdown built into every
  ARMv8 CPU. It's not a separate chip on the board; it's part
  of the CPU itself. Each CPU has its own copy. The kernel
  reads its frequency from a system register, programs a
  countdown into another, and gets an IRQ when the countdown
  hits zero.
- **EL1 physical timer.** One of several timers exposed by the
  ARM Generic Timer. The "physical" part means it counts real
  wall-clock time (not virtual time mediated by a hypervisor);
  "EL1" means it's programmed using EL1-accessible system
  registers and fires at EL1. nonux uses this one.
- **`CNTFRQ_EL0`.** The system register that holds the Generic
  Timer's tick frequency in Hz. It's set by firmware before the
  kernel starts and is read-only at EL1. On QEMU's `virt`
  machine it's typically 62.5 MHz.
- **`CNTP_TVAL_EL0`.** The EL1 physical timer's "down-counter
  reload" register. Writing a positive value `N` arms the
  timer to fire after `N` of its own ticks. The hardware
  decrements `TVAL` on every Generic-Timer tick; when it
  reaches zero, it raises the timer's IRQ.
- **`CNTP_CTL_EL0`.** The EL1 physical timer's control
  register. Bit 0 enables the timer; bit 1 masks the IRQ;
  bit 2 reads as 1 once the countdown has hit zero. nonux
  writes the value `1` (enabled, IRQ unmasked) once at boot
  and never touches it again.
- **PPI 30.** The IRQ number assigned to the EL1 physical
  timer on the QEMU `virt` machine. Recall from chapter 3
  that PPIs (16–31) are *per-CPU* IRQs — each CPU gets its
  own copy of the same number. That's what the timer wants:
  every CPU has its own Generic Timer, and each one's
  countdown should fire on the same CPU that programmed it.
- **Preemption.** Forcibly taking the CPU away from one
  running task and giving it to another, without the first
  task asking. This is what "multitasking" means at the
  hardest level. The opposite is **cooperative scheduling**,
  where a task only yields when it explicitly chooses to.
  nonux preempts. The decision happens at every IRQ-return.
- **Time slice / quantum.** The maximum length of time a
  scheduler will let a task run before considering switching
  to a different one. Measured in ticks; "two ticks" at
  10 Hz means 200 ms. When the quantum expires, the
  scheduler flags the current task for preemption.
- **`need_resched`.** A flag on every task ("does this task
  need to be replaced?"). The timer ISR sets it when the
  quantum runs out; the IRQ-return shim consults it before
  `eret`. If set, the kernel switches tasks before returning;
  if clear, the same task resumes.
- **Drift.** The accumulated error in a periodic timer that
  re-arms itself a little late each cycle. If we re-arm 1 µs
  after the desired moment, ten thousand ticks later we're
  10 ms behind reality. Avoiding drift is why nonux's tick
  ISR re-arms the countdown *before* doing anything else.
- **Recomposition.** A nonux-specific term for "swap a running
  component with a different implementation, live." Some
  recomposition steps need the timer to stop firing for a
  brief window — that's what `timer_pause` / `timer_resume`
  are for.

---

## Why a kernel needs a steady heartbeat

It's worth spending a moment on what changes when the kernel can
count on a regular tick.

Without a tick, the only way the kernel ever runs is when a
device IRQ fires (a key was pressed, a network packet arrived) or
when a userspace program makes a system call. In between, the CPU
just runs whatever task it's running. There's no way to say
"after 100 ms, please come back and check on me" — there's
nothing to *cause* the come-back.

That kills three things:

1. **Preemption.** If a task spins in a tight loop — `for(;;);`
   — without making any syscalls, *nothing* will take the CPU
   away from it. No other task will ever run. The system hangs
   until you reboot.
2. **Timeouts.** If a task asks "give me a byte of input, but
   bail after 5 seconds", the kernel needs *something* to fire
   at the 5-second mark and wake the task. Without a clock, the
   kernel doesn't know when 5 seconds have passed.
3. **Bookkeeping.** Things like "expire entries in a cache
   older than a minute", "wake up a sleeping task at 2:00 PM",
   or "log CPU usage statistics every second" all need a way
   to say *when*.

So every kernel sets up a periodic IRQ early in boot. POSIX
tradition called that IRQ the **clock interrupt**; ARM
documentation says **timer interrupt** or **tick**. They're the
same thing.

In nonux, the heartbeat fires 10 times per second. Every 100 ms,
the CPU stops what it was doing, runs `on_tick`, and resumes.
That's our handle on time.

> **Side note: why 10 Hz?** It's a compromise. Higher
> frequencies (100 Hz, 250 Hz, 1000 Hz) react faster to events
> at the cost of more interrupt overhead — every tick is a
> small tax that scales linearly. Lower frequencies (1 Hz)
> have less overhead but coarser timeout granularity and
> sluggish preemption. Linux defaults vary across builds (100,
> 250, 300, 1000); BSDs typically use 100. nonux uses 10
> because it's enough to demonstrate preemption clearly while
> keeping IRQ-rate noise low in test traces. The number is one
> argument to `timer_init`, so changing it is a one-line edit.

---

## The ARM Generic Timer

We need *some* hardware that can count down to zero on its own.
ARM's answer, baked into every ARMv8 CPU, is the **Generic
Timer**. It's not a peripheral on the chip's bus the way the
PL011 is. It lives inside the CPU core itself, alongside the
arithmetic units and the page-table walker.

What the Generic Timer gives us:

- **A free-running counter.** A 64-bit value that increments at
  a fixed frequency. The frequency is set by firmware and the
  kernel can only read it. On QEMU's `virt` machine it's
  62.5 MHz, meaning the counter ticks 62 500 000 times per
  second.
- **A countdown timer.** A separate register the kernel writes a
  number into. The hardware decrements that number on every
  Generic-Timer tick. When it reaches zero, it raises an IRQ.
- **Several views of both.** The same physical counter is
  accessible at EL0, EL1, and EL2, each with slightly different
  rules about what the lower exception levels can or can't see.
  We use the **EL1 physical timer**: programmed at EL1, fires
  at EL1, counts unmediated wall-clock time.

For us, three system registers are enough:

| Register | What it holds |
|----------|---------------|
| `CNTFRQ_EL0`     | The counter's frequency in Hz (read-only) |
| `CNTP_TVAL_EL0`  | The EL1 physical timer's down-counter reload |
| `CNTP_CTL_EL0`   | The EL1 physical timer's enable / mask bit |

(The "EL0" suffix on these names is misleading — it just
indicates the lowest EL that can read each register. EL0 can
read `CNTFRQ_EL0` because reading the frequency is harmless;
it can also read `CNTP_TVAL_EL0` and `CNTP_CTL_EL0` *if* an EL1
control bit allows. We don't enable that, so for us all three
are EL1-only.)

### How a countdown works

The model is simple. There's a counter inside the CPU,
incrementing at 62.5 MHz. Separately there's a register —
`CNTP_TVAL_EL0` — that the kernel can write a 32-bit value into.
Once written, that register decrements *automatically* on every
Generic-Timer tick. When it hits zero (and the timer is
enabled), the CPU's GIC line for "EL1 physical timer fired" goes
high.

The math, then: if we want a tick every 100 ms, and the counter
ticks at 62.5 MHz, we want to count down `62 500 000 / 10 =
6 250 000` Generic-Timer ticks per kernel-tick. So we write
`6 250 000` into `CNTP_TVAL_EL0` and the IRQ fires 100 ms later.

When the IRQ fires, our ISR has to write `CNTP_TVAL_EL0` *again*
to set up the next interval. The hardware doesn't auto-rearm.

> **Side note: why no auto-rearm?** Because some kernels want
> *aperiodic* timers: "next tick in 173 ms; the one after that
> in 4.2 ms; then 800 ms" (a "tickless" or "high-resolution
> timer" design). With manual re-arm each time, the kernel has
> total control. nonux uses a steady periodic interval today,
> but the ISR is small enough that an aperiodic strategy would
> be a one-function rewrite.

---

## `timer_init`: arming the heartbeat

Putting that all together, here's what happens when `boot_main`
calls `timer_init(10)`:

```c
#define TIMER_PPI 30   /* EL1 physical timer, QEMU virt */

static uint64_t         g_interval;
static _Atomic uint64_t g_ticks;

void timer_init(unsigned int hz)
{
    uint64_t freq = read_cntfrq();
    if (hz == 0) hz = 1;
    g_interval = freq / hz;

    irq_register(TIMER_PPI, on_tick, 0);
    gic_enable(TIMER_PPI);

    write_cntp_tval(g_interval);
    write_cntp_ctl(1);          /* enable, unmasked */
    kprintf("[timer] cntfrq=%lu Hz, interval=%lu ticks, rate=%u Hz\n",
            freq, g_interval, hz);
}
```

The helpers around the system registers are one-line wrappers:

```c
static inline uint64_t read_cntfrq(void)
{
    uint64_t v;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline void write_cntp_tval(uint64_t v)
{
    asm volatile("msr cntp_tval_el0, %0" :: "r"(v));
}

static inline void write_cntp_ctl(uint64_t v)
{
    asm volatile("msr cntp_ctl_el0, %0" :: "r"(v) : "memory");
}
```

`mrs` and `msr` are the same system-register move instructions
chapter 3 introduced for `VBAR_EL1`. Reading `CNTFRQ_EL0` lifts
the firmware-set frequency into a C variable; writing
`CNTP_TVAL_EL0` arms the countdown; writing `CNTP_CTL_EL0` flips
the enable bit.

What `timer_init` does, line by line:

1. **Read the timer's frequency.** Whatever firmware
   programmed, we now know.
2. **Compute the down-counter reload.** Dividing `freq` by `hz`
   tells us how many Generic-Timer ticks fit in one
   kernel-tick. The `if (hz == 0)` guard is paranoid — passing
   `0` would divide by zero. Defaults to `hz = 1`, the slowest
   safe rate.
3. **Register the IRQ handler.** `irq_register(30, on_tick, 0)`
   stores `on_tick` in slot 30 of the per-IRQ table chapter 3
   walked through. The `0` is the cookie; `on_tick` ignores it.
4. **Enable IRQ 30 at the GIC.** `gic_enable(30)` flips the
   per-IRQ enable bit on the GIC distributor and sets its
   priority. After this, the GIC will deliver any IRQ 30 to
   the CPU.
5. **Arm the countdown.** Writing the computed interval into
   `CNTP_TVAL_EL0` starts the count.
6. **Enable the timer.** Writing `1` to `CNTP_CTL_EL0` says
   "timer enabled, IRQ unmasked at the chip". From this moment
   on, in `freq / hz` Generic-Timer ticks, the IRQ will fire.

But it doesn't fire *yet* — DAIF.I (the local IRQ mask, also
from chapter 3) is still set. The very last thing `boot_main`
does is `irq_enable_local()`. The first IRQ that arrives after
that is almost always the timer's first tick, because by then
its countdown has already expired.

### Why the timer is a PPI

Chapter 3 said most device IRQs are SPIs (shared peripheral
interrupts, numbered 32+) and per-CPU peripherals are PPIs
(numbered 16–31). The timer is a textbook PPI. Here's why:

The Generic Timer is *inside the CPU*. Each CPU has its own.
When CPU 0's countdown expires, the IRQ should land on CPU 0 —
the one that programmed it, the one whose counter just hit
zero — and *not* on CPU 1, which has its own (probably very
different) countdown going.

PPIs solve exactly that. The GIC reserves IRQ numbers 16–31 to
mean "this IRQ is delivered to the CPU that triggered it". So
on a four-CPU system, all four CPUs would `gic_enable(30)`,
but each CPU's IRQ 30 is a separate logical interrupt that
fires on whichever CPU armed it.

For nonux on QEMU virt — single CPU — the distinction is moot.
But the design is forward-portable to multi-core kernels for
free.

---

## The tick: what `on_tick` actually does

Here's the entire tick handler:

```c
static void on_tick(void *data)
{
    (void)data;
    /* Rearm the countdown before anything else so we don't drift. */
    write_cntp_tval(g_interval);
    __atomic_add_fetch(&g_ticks, 1, __ATOMIC_RELAXED);
    /* Drive the scheduler.  No-op until sched_init has stashed a
     * policy pointer (early boot, or NX_KTEST builds that exit
     * before calling sched_init). */
    sched_tick();
}
```

Three lines of work. We'll take them in order, because the order
*is the design*.

### 1. Rearm the countdown first

The very first thing is `write_cntp_tval(g_interval)`. This sets
the down-counter back to its full interval, so the *next* tick
will fire `g_interval` Generic-Timer ticks from now.

Why first? Because the next tick's deadline is "*now* +
`g_interval`". Every microsecond we delay the rearm pushes the
next tick deadline that microsecond later. If we did the rearm
*after* the rest of the work, the time `on_tick` itself spends
running would steadily fall behind a perfect schedule. Five
microseconds of work × 10 ticks per second × 1 hour = 0.18
seconds of drift per hour. Not catastrophic, but enough to
matter for any code that uses tick counts as wall-clock time.

The fix is "rearm immediately, then do everything else":

```
   IRQ fires here
       │
       ▼
       ┌──── rearm: TVAL = interval ─── (next tick is now scheduled)
       ├──── bump g_ticks
       ├──── sched_tick() ── drives scheduler
       │
       ▼
   handler returns; eret
```

Whatever comes after the rearm doesn't shift the next tick. The
schedule stays *aligned to a fixed offset from boot*, no matter
how long the handler takes.

This pattern — "stop the bleeding before doing anything else" —
shows up in lots of ISRs. The PL011 RX ISR from chapter 2 follows
the same idea: drain the FIFO before doing anything that could
take a while.

### 2. Bump the tick counter

```c
__atomic_add_fetch(&g_ticks, 1, __ATOMIC_RELAXED);
```

`g_ticks` is a 64-bit counter for "how many ticks have happened
since boot". It's the kernel's notion of monotonic time, in
units of "tenths of a second" at 10 Hz.

The atomic add is mostly for tidiness on a single CPU — there's
only one writer (the ISR) and the IRQ is masked while the ISR
runs, so no other code can preempt it. But on a hypothetical
multi-core build, two CPUs reading `g_ticks` while a third is
incrementing it could observe a torn value without the atomic.
Cheap insurance, free at single-core, correct at any core count.

The relaxed memory ordering is fine here: nobody synchronizes
*on* the tick value. Code that wants to wake at "100 ticks from
now" just samples `timer_ticks()` and adds 100; if it reads a
slightly stale value, it sleeps a tick longer.

### 3. Drive the scheduler

```c
sched_tick();
```

This is where the timer connects to multitasking. `sched_tick()`
calls into the active scheduler policy and gives it a chance to
account for the elapsed tick. The full scheduler story comes in a
later chapter; here we'll cover just the slice that matters to the
timer.

```c
void sched_tick(void)
{
    if (!g_sched_ops) return;
    g_sched_ops->tick(g_sched_self);
    nx_waitq_tick_deadlines();
}
```

The first guard handles early boot — if `sched_init` hasn't
stashed a scheduler policy yet, do nothing. The interesting line
is the call into the policy's `tick` op. nonux's default
policy is round-robin; here's its tick implementation:

```c
static void sched_rr_tick(void *self)
{
    struct sched_rr_state *s = self;
    struct nx_task *curr = nx_task_current();
    if (!curr) return;
    if (s->remaining > 0) s->remaining--;
    if (s->remaining == 0) {
        curr->need_resched = 1;
        s->remaining = s->quantum_ticks;
    }
}
```

Three things:

1. **Get the currently-running task.** `nx_task_current()` reads
   a per-CPU pointer (stored in a system register called
   `TPIDR_EL1`). The full task model is a later chapter; for now
   "the task we just interrupted" is enough.
2. **Decrement the remaining quantum.** The scheduler policy
   keeps a counter `remaining`, initialised to a default
   quantum (a small number of ticks). Each tick chips one off.
3. **If the quantum hits zero, flag the task and reset.** The
   *policy* doesn't switch tasks itself — it just sets
   `current->need_resched = 1`. Whoever runs next on this CPU
   will notice the flag and act on it.

That's the whole interaction between the timer ISR and the
scheduler. The timer doesn't switch tasks; it doesn't pick what
runs next; it doesn't even know what tasks exist. Its only job is
to fire on a steady schedule and give the scheduler one tick of
information.

---

## From flag to switch: where the actual preemption happens

If the timer ISR sets a flag, *someone* has to consume the flag
and do the actual context switch. That someone is the IRQ-return
shim from chapter 3. Recall the assembly:

```asm
_irq_stub:
    SAVE_TRAPFRAME
    mov     x0, sp
    bl      on_irq
    bl      sched_check_resched
    RESTORE_TRAPFRAME
    eret
```

Right between `on_irq` (which dispatched our `on_tick`, which
called `sched_tick`, which set `need_resched`) and the
`RESTORE_TRAPFRAME / eret` pair (which would normally return to
the interrupted task), the stub calls
**`sched_check_resched`**. Here's the part of it that matters:

```c
void sched_check_resched(void)
{
    struct nx_task *curr = nx_task_current();
    if (!curr) return;
    if (!g_sched_ops) return;

    /* …signal-delivery checks omitted… */

    if (!curr->need_resched) return;
    if (curr->preempt_count > 0) return;

    curr->need_resched = 0;

    g_sched_ops->yield(g_sched_self);
    struct nx_task *next = g_sched_ops->pick_next(g_sched_self);
    if (!next || next == curr) {
        asm volatile("wfi");
        return;
    }

    /* …context-switch hooks + TTBR0 + TPIDR_EL0 housekeeping… */

    cpu_switch_to(curr, next);
}
```

In plain language:

- **If `need_resched` isn't set, return.** No work to do.
- **If preemption is disabled** (`preempt_count > 0`), return.
  Some kernel code intentionally pins itself to "don't switch
  away from me right now" — this is what that does.
- **Otherwise, switch.** Clear the flag, ask the policy to
  rotate its runqueue, ask it to pick a next task, and
  **`cpu_switch_to`** swaps CPU state — registers, stack — to
  the new task. The old task is paused mid-flight; the new
  task resumes wherever *it* was last paused.

When the new task eventually gets selected again (the next time
`pick_next` returns it), `cpu_switch_to` returns to the *old*
task at the line right after its previous `cpu_switch_to` call,
and the old `eret` finally runs.

The thing to take away from this chapter is *not* the
context-switch mechanics (those get their own chapter). It's the
**chain of events**:

```
  Generic-Timer countdown hits zero
       │
       ▼
  PPI 30 fires; GIC delivers to CPU
       │
       ▼  (chapter 3 machinery)
  CPU jumps to vector table; _irq_stub runs;
  on_irq → irq_dispatch → on_tick
       │
       ▼  (this chapter)
  on_tick:
    - rearm CNTP_TVAL = g_interval
    - g_ticks++
    - sched_tick → policy->tick → maybe set need_resched
       │
       ▼  (back through the IRQ machinery)
  irq_dispatch → gic_eoi
  on_irq returns
       │
       ▼
  _irq_stub: bl sched_check_resched
       │
       ├── need_resched not set? → restore + eret to same task
       └── need_resched set?     → cpu_switch_to(prev, next)
                                   on resume: restore + eret to prev later
```

Every preemption in nonux follows this exact path. **The timer
is the only thing that *originates* a preemption** in the
absence of explicit yields or blocking calls. Pull the timer
out and `for(;;);` once again hangs the system, because nothing
will ever land in `sched_check_resched` to flip the task.

---

## `timer_ticks`: looking at the clock

Other code reads the tick count through one accessor:

```c
uint64_t timer_ticks(void)
{
    return __atomic_load_n(&g_ticks, __ATOMIC_RELAXED);
}
```

That's it. A monotonic, never-decreasing 64-bit counter of
ticks-since-boot. At 10 Hz it would take just under 60 billion
years to overflow, so we treat the counter as effectively
infinite.

Code that wants a deadline computes its target tick once
(`timer_ticks() + N`) and compares the live value against it
inside its wait loop. Wait queues use this to expire timed-out
waiters every `sched_tick` (the call to
`nx_waitq_tick_deadlines()` we glossed over above is exactly
that — walk a list of "waiters with deadlines" and wake any
whose target tick has been reached).

A side note about precision: `timer_ticks()` resolves to *one
tick*. At 10 Hz, that's 100 ms — enough for "wait 5 seconds" or
"timeout in a minute", not enough for "sleep 50 µs". A finer
clock (e.g. reading `CNTPCT_EL0` directly for sub-microsecond
resolution) could be added on top of this driver, but nonux
doesn't need one yet.

---

## `timer_pause` and `timer_resume`

There's one more pair of functions in the driver that we'll
mention briefly, even though their full meaning lives in a
later chapter on runtime composition:

```c
void timer_pause(void);
void timer_resume(void);
```

`timer_pause` masks the timer's PPI at the GIC. While paused,
the countdown still runs and would still raise the IRQ — but
the GIC's distributor blocks it, so the IRQ never reaches the
CPU. No tick fires; no `sched_tick` runs.

The *reason* this exists is the kernel's recomposition path.
nonux can swap a running scheduler component for a different
one, live, while tasks are running. During the brief window
where the active scheduler pointer is being updated, no tick
should be in flight. `timer_pause` closes that window;
`timer_resume` reopens it.

The pair uses an internal counter so overlapping callers
compose:

```c
static _Atomic unsigned g_pause_nest;

void timer_pause(void)
{
    unsigned prev = __atomic_fetch_add(&g_pause_nest, 1, __ATOMIC_ACQ_REL);
    if (prev == 0)
        gic_disable(TIMER_PPI);
}

void timer_resume(void)
{
    unsigned cur;
    do {
        cur = __atomic_load_n(&g_pause_nest, __ATOMIC_RELAXED);
        if (cur == 0) return;
    } while (!__atomic_compare_exchange_n(&g_pause_nest, &cur, cur - 1,
                                          true, __ATOMIC_ACQ_REL,
                                          __ATOMIC_RELAXED));
    if (cur == 1)
        gic_enable(TIMER_PPI);
}
```

Two patterns worth flagging:

- **Nestable pause.** A counter, not a flag. If A pauses, B
  pauses, then B resumes, the timer stays off because A still
  has a pause outstanding. Only when the count drops to zero
  does the IRQ get re-enabled. This composes with itself across
  unrelated callers without anyone needing to coordinate.
- **Saturate at zero.** A `resume` without a matching `pause`
  is a no-op rather than an underflow. Calling
  `__atomic_fetch_sub` blindly would silently flip the counter
  to 0xFFFFFFFF and disable the timer forever the next time
  someone *did* pause. The compare-and-swap loop guards
  against that.

That's all we'll say about them here. The full recomposition
story — what triggers a pause, how the swap is choreographed,
why it's safe — is a later chapter's problem.

---

## A few extra things to know

- **Each CPU has its own timer.** On multi-core, every CPU gets
  its own Generic Timer with its own countdown register. They
  run independently. nonux's tick rate is set from the boot
  CPU's `CNTFRQ_EL0`, which is consistent across CPUs by
  architectural rule, but the per-CPU countdowns are armed
  separately. (For us — one CPU — this is invisible.)

- **The frequency comes from firmware, not us.** `CNTFRQ_EL0`
  is set by code that ran *before* the kernel — typically an
  EL3 or EL2 boot stub. By the time we run at EL1, it's
  already programmed and read-only. If the value is wrong,
  every tick we compute is also wrong; there's no way for the
  kernel to recover. On QEMU `virt`, the value (62.5 MHz) is
  hardwired into the QEMU model, so it's predictable.

- **`CNTP_TVAL_EL0` is signed and 32-bit, even though it sits
  in a 64-bit register.** The high 32 bits read as zero; the
  low 32 are interpreted as a signed integer. The "fired"
  state is "the value is negative or zero". For any `g_interval`
  smaller than 2³¹ Generic-Timer ticks (about 34 seconds at
  62.5 MHz), the encoding works the way you'd expect; longer
  intervals would alias and behave wrong. nonux's biggest
  interval (1 second at the slowest supported `hz=1`) is far
  inside the safe range.

- **The IRQ is level-triggered, not edge-triggered.** As long
  as `CNTP_CTL_EL0`'s "fired" bit is set, the GIC sees the
  timer's IRQ line *high*. If we EOI without rearming or
  disabling the timer, the line stays asserted and the IRQ
  re-fires immediately. That's why `on_tick`'s very first act
  is `write_cntp_tval(g_interval)` — writing a positive value
  there clears the fired bit, dropping the IRQ line.

- **`timer_ticks()` is monotonic, but it's not a wall clock.**
  Wall clocks (the kind that say "Tuesday, 14:32:08 UTC") need
  a separate notion: an offset from a known epoch (typically
  POSIX's "1970-01-01 00:00:00 UTC"), maintained against an
  external time source. nonux doesn't have one yet.
  `timer_ticks()` is the *uptime* clock — never goes backward,
  but tells you nothing about what the wall clock says. Real
  kernels add NTP daemons, RTC drivers, and `clock_gettime` on
  top.

- **The Generic Timer existed long before ARMv8.** The 32-bit
  ARMv7-A architecture had a Generic Timer too (added in the
  Cortex-A15 era, 2012-ish). The system-register names changed
  for the 64-bit transition but the model is the same:
  free-running counter + countdown timers. Linux's `arm_arch_timer.c`
  driver supports both flavors.

---

## Where to read more

- [`core/timer/timer.c`](../core/timer/timer.c) — full driver,
  ~80 lines.
- [`core/sched/sched.c`](../core/sched/sched.c) — `sched_tick`
  and `sched_check_resched`. The full scheduler story is a
  later chapter; this file is the connection point.
- [Chapter 3 §"Acknowledging and ending"](03-exceptions-gic-and-irqs.md#acknowledging-and-ending)
  — `gic_ack` / `gic_eoi`, which `irq_dispatch` calls around
  every tick.
- [Chapter 3 §"The IRQ pipeline"](03-exceptions-gic-and-irqs.md#the-dispatcher-and-the-per-irq-table)
  — the path from "GIC delivers IRQ 30" to "`on_tick` runs",
  which this chapter assumed.
- ARM Architecture Reference Manual for ARMv8-A (DDI0487),
  chapter "Generic Timer" — the canonical reference for
  `CNTFRQ_EL0`, `CNTP_TVAL_EL0`, `CNTP_CTL_EL0`, and the half
  dozen other timer system registers we don't use.
- [QEMU `hw/arm/virt.c`](https://gitlab.com/qemu-project/qemu)
  — the source for the IRQ-number assignments. The timer being
  PPI 30 comes from there.
- [Linux `Documentation/arm64/booting.rst`](https://www.kernel.org/doc/Documentation/arm64/booting.rst)
  — describes what state the firmware-to-kernel boot contract
  expects from the Generic Timer, which is the same contract
  nonux relies on.
