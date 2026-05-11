# The scheduler

Chapter 7 built the mechanism: a **task**, its **kernel stack**, the
saved register frame inside `struct nx_task`, and the ARM64
assembly routine `cpu_switch_to` that swaps one task's registers
for another's. We can move from one task to another. We deliberately
did **not** answer the question *which task do we move to?* That is
the **scheduler**.

This chapter answers that question. By the end you should know how
nonux's scheduler is split into a small **core driver** (in
`core/sched/`) and a swappable **policy component** (one of two:
`sched_rr` or `sched_priority`), what the **runqueue** is and what
operations a policy must provide, how the timer tick funnels through
the core driver to the policy, what the core driver actually does
during a **reschedule** — the same `sched_check_resched` we kept
calling in chapters 4 and 7 — and how voluntary `nx_task_yield`
shares a path with involuntary timer preemption. We will look at
both shipped policies line by line and run through a worked timeline
of two **kernel threads** sharing one CPU under each.

The relevant files in this repo:

- [`core/sched/sched.h`](../core/sched/sched.h),
  [`core/sched/sched.c`](../core/sched/sched.c) — the core driver:
  `sched_init`, `sched_start`, `sched_tick`,
  `sched_check_resched`, `nx_task_yield`,
  `sched_spawn_kthread`, the **idle task**.
- [`interfaces/scheduler.h`](../interfaces/scheduler.h) — the
  interface every policy must implement: `struct
  nx_scheduler_ops` with seven function pointers
  (`pick_next`, `enqueue`, `dequeue`, `yield`, `set_priority`,
  `tick`, `runqueue_size`) plus the `reap_task` lifecycle hook.
- [`components/sched_rr/sched_rr.c`](../components/sched_rr/sched_rr.c)
  — round-robin policy: one FIFO list, rotate on yield.
- [`components/sched_priority/sched_priority.c`](../components/sched_priority/sched_priority.c)
  — eight priority buckets, each a FIFO sub-queue, scan
  high-to-low.
- [`core/sched/waitq.h`](../core/sched/waitq.h),
  [`core/sched/waitq.c`](../core/sched/waitq.c) — the **wait
  queue** primitive: how a task gets off the runqueue when it has
  nothing useful to do and back onto it when something wakes it.

---

## Terms you'll see

- **Scheduler.** The kernel subsystem that picks which task runs
  next on a CPU. nonux splits the scheduler into two pieces: a
  fixed core driver and a swappable policy.
- **Core scheduler driver.** The non-swappable half, in
  `core/sched/`. Holds the stashed policy pointer, runs the
  reschedule shim on IRQ return, owns the idle task, transitions
  the boot context into idle, and provides the `nx_task_yield`
  primitive that voluntary callers use.
- **Scheduler policy.** The swappable half, packaged as a
  framework component (chapter 9). Provides the runqueue layout
  and the decision rules — *which* task to pick next, *when* to
  flag the current one for preemption, *how* yields rotate
  competing tasks. Every policy implements `struct
  nx_scheduler_ops`. nonux ships two: round-robin and fixed
  priority.
- **Runqueue.** The data structure that holds every task ready to
  run. The shape is the policy's choice: round-robin uses one
  intrusive FIFO list, fixed-priority uses eight of them. The
  word "runqueue" is the standard term across kernels even when
  the structure is plural or not strictly a queue.
- **Quantum (time slice).** The maximum number of timer ticks a
  task is allowed to run before the scheduler considers
  preempting it. Both shipped policies default to 2 ticks at
  10 Hz, so 200 ms.
- **Preemption.** Forcibly switching away from a running task.
  Triggered when the timer ISR notices the task has used its
  quantum (or when wake-up logic flips a higher-priority task to
  runnable). The opposite is a **voluntary yield**.
- **`need_resched`.** A flag on every task. The policy sets it
  during `tick` when the quantum expires; the core driver reads
  it on every IRQ-return path. A non-zero `need_resched` means
  "switch away as soon as it is safe".
- **`preempt_count`.** A counter on every task. Non-zero means
  "do not preempt me right now, even if `need_resched` is set".
  Used to bracket short critical sections where switching would
  break invariants.
- **`g_idle_task`.** A statically allocated task struct used when
  no other task is ready. The boot CPU runs `boot_main` first
  with no task identity; `sched_start` retroactively names that
  boot context "the idle task" and publishes it through
  `TPIDR_EL1`. From then on, "no other task to run" is expressed
  by switching to (or staying on) idle.
- **WFI.** "Wait for interrupt". An ARM64 instruction that puts
  the CPU into a low-power state until any unmasked interrupt
  fires. The idle task and the empty-runqueue branch of
  `sched_check_resched` both use it.
- **Wait queue.** A separate data structure a task can park on
  when it has nothing to do until some event happens
  (incoming data, child exit, timeout). The task comes off the
  runqueue while parked, and a wake-up call puts it back.

---

## The architectural decision: core/policy split

A scheduler is two things tangled together: a **mechanism** and a
**policy**.

The mechanism is universal. Whatever rule you use to pick the next
task, you still need a runqueue of some kind, a way for the timer
tick to notify the scheduler, a path on IRQ return that performs
the actual switch, a place to record "I want to be preempted", an
idle fallback when nothing else is ready, and a primitive for
voluntary yield. Most of these have only one sensible
implementation.

The policy is where everyone disagrees. Round-robin gives every
task a turn. Fixed priority always runs the highest. Linux's CFS
maintains a red-black tree keyed by accumulated runtime. Real-time
schedulers add deadlines and admission control. The mechanism
above does not care which of these you pick — it just needs the
policy to answer two questions: *given a tick, should the current
task continue?* and *given a switch, which task is next?*

nonux splits the scheduler along that seam. The split is the same
component / interface pattern the rest of the kernel uses, applied
to one of the most often-rewritten subsystems in OS history.

```
            +----------------------------+
            |  core scheduler driver     |
            |  (core/sched/, fixed)      |
            |                            |
            |  sched_tick, sched_check_  |
            |  resched, sched_start,     |
            |  nx_task_yield, idle task  |
            +-----------+----------------+
                        |
                        |  struct nx_scheduler_ops
                        |  (interfaces/scheduler.h)
                        |
            +-----------v----------------+
            |  scheduler policy          |
            |  (components/sched_*,      |
            |   swappable)               |
            +----------------------------+
              sched_rr   sched_priority
```

The two pieces talk through a single small interface. The core
driver never reaches inside the policy's state; the policy never
reaches into the driver's globals. When chapter 14 shows you
nonux swapping a live scheduler under a running process, that
clean line is what makes the swap possible.

For now we just use it: the core driver does its job, the policy
does its job, and the interface keeps them honest.

---

## The interface: `struct nx_scheduler_ops`

[`interfaces/scheduler.h`](../interfaces/scheduler.h) defines
exactly what every scheduler policy must provide. The whole
contract is one struct of function pointers:

```c
struct nx_scheduler_ops {
    struct nx_task *(*pick_next)(void *self);
    int  (*enqueue)(void *self, struct nx_task *task);
    int  (*dequeue)(void *self, struct nx_task *task);
    void (*yield)(void *self);
    int  (*set_priority)(void *self, struct nx_task *task, int priority);
    void (*tick)(void *self);
    int  (*runqueue_size)(void *self);
    void (*reap_task)(void *self, struct nx_task *task);
};
```

Eight pointers. Each has a one-paragraph contract in the header.
The important ones:

- **`pick_next`**: return the task to run next, or `NULL` if the
  runqueue is empty. **Borrow** — the task pointer remains the
  caller's; the policy keeps it valid only while it's queued.
  Idempotent against the same queue state, so the core driver
  can call it twice and get the same answer.
- **`enqueue`** / **`dequeue`**: add or remove a task from the
  runqueue. Borrow. Errors: `NX_EEXIST` (double-enqueue),
  `NX_ENOENT` (not on the queue), `NX_EINVAL` (NULL task).
- **`yield`**: the current task voluntarily gives up the rest of
  its slice. In round-robin this rotates the queue; in
  fixed-priority it rotates the current task within its bucket.
- **`tick`**: called once per timer tick. The policy typically
  decrements the current task's remaining quantum and sets
  `need_resched` when it hits zero. Must be bounded and
  non-blocking — runs from the timer ISR.
- **`set_priority`**: change a task's priority level. Policies
  that don't honour priorities (like round-robin) return
  `NX_EINVAL` uniformly; the caller treats that as "priorities
  not supported here".
- **`runqueue_size`**: return the count of non-idle queued
  tasks. Lets callers ask "is there real work to do?" without
  walking the queue themselves.
- **`reap_task`**: free a zombie task that has already been
  dequeued. Unlike every other op in this table, `reap_task`
  *transfers* ownership: the policy is responsible for calling
  `nx_task_destroy`. The op sits here so per-policy bookkeeping
  (counters, future NUMA affinity, etc.) can run before the
  task struct is freed.

The two pieces of state the interface implies but does not
expose:

- The **runqueue itself**. Round-robin uses one
  `struct nx_list_head`; fixed-priority uses an array of eight.
  The driver never touches either directly — only the policy
  knows the shape.
- The **quantum counter**. Each policy keeps a `remaining`
  field that `tick` decrements. Whether one counter applies to
  all tasks (today's choice for simplicity) or each task gets
  its own (a future refinement) is the policy's call.

> One thing the interface does *not* require: tracking "the
> currently running task". The core driver does that — the
> running task is whoever `TPIDR_EL1` points at, per chapter 7.
> The policy never separately tracks "current"; it would just be
> a second source of truth that can diverge.

---

## How the core driver finds the policy

The core driver calls into the policy through a single stashed
pointer pair. From [`core/sched/sched.c`](../core/sched/sched.c):

```c
static const struct nx_scheduler_ops *g_sched_ops;
static void                          *g_sched_self;

void sched_init(const struct nx_scheduler_ops *ops, void *self)
{
    g_sched_ops  = ops;
    g_sched_self = self;
}
```

Every call from the driver to the policy goes through these two
globals. `g_sched_ops->tick(g_sched_self)`,
`g_sched_ops->pick_next(g_sched_self)`,
`g_sched_ops->enqueue(g_sched_self, t)`. The first argument is
always the policy's instance pointer — the `void *self` you've
been seeing in every op signature.

Who calls `sched_init`? Two places:

1. **The framework bootstrap**, when it brings the scheduler
   component up to `NX_LC_ACTIVE`. At the very end of boot,
   `framework_bootstrap` walks the component graph from
   `kernel.json`, calls each component's `init` and `enable`
   hooks, and the scheduler's `enable` hook calls
   `sched_init(&sched_rr_scheduler_ops, self)` (or
   `sched_priority_scheduler_ops`). That's how the driver
   learns which policy is bound.

2. **The same `enable` hook again** on a live policy swap. When
   chapter 14's recomposition path swaps the active scheduler
   under a running kernel, the new policy's `enable` runs and
   re-points `g_sched_ops` and `g_sched_self` at itself. The
   driver never restarts; the calls just route somewhere new
   from the next invocation onward.

In `sched_rr.c`:

```c
static int sched_rr_enable(void *self)
{
    struct sched_rr_state *s = self;
    s->enable_called++;
#if !__STDC_HOSTED__
    sched_init(&sched_rr_scheduler_ops, self);
    /* (idle-task enqueue follows — see "Live swap" below.) */
#endif
    return NX_OK;
}
```

And the symmetric thing in `sched_priority.c`. Both register
their own ops table the moment their `enable` fires.

> Two-step publish: `g_sched_ops` is assigned before
> `g_sched_self`. The comment in `sched_init` calls it out — on
> single-core ARM64 the compiler's sequencing is enough; SMP
> will need explicit release/acquire ordering so a concurrent
> reader can't see a non-NULL `ops` paired with a stale `self`.

---

## Booting into the idle task: `sched_start`

The CPU does not start out with an identity. After power-up,
`start.S` clears the BSS, sets up the C stack, branches to
`boot_main` (in `core/boot/boot.c`), and runs C code on the boot
stack. Through the whole bring-up — PMM, MMU, IRQ controller,
timer, framework bootstrap — `TPIDR_EL1` holds whatever the boot
ROM left in it, which is typically zero. `nx_task_current()`
returns `NULL`. There is no "current task" yet.

We can't keep running in that anonymous state forever. The
scheduler's whole world expects to ask "who is running now?" and
get a real `struct nx_task *` back, with a `need_resched` flag
and a `preempt_count` it can read.

`sched_start` makes that real. From `sched.c`:

```c
struct nx_task g_idle_task;
static bool    g_sched_started;

void sched_start(void)
{
    if (g_sched_started) return;
    g_sched_started = true;

    idle_task_init();

#if !__STDC_HOSTED__
    asm volatile("msr tpidr_el1, %0" :: "r"(&g_idle_task));
#endif

    if (g_sched_ops)
        g_sched_ops->enqueue(g_sched_self, &g_idle_task);
}
```

Three things happen:

1. `idle_task_init` fills in the statically allocated
   `g_idle_task` struct: name "idle", id 0 (reserved for idle),
   state `RUNNING`, empty runqueue node, no kernel stack
   tracked (the boot stack is implicitly idle's stack — the CPU
   is *already* running on it). The saved `cpu_ctx` is left
   undefined. That looks scary until you realise we will only
   ever switch *away* from this task. The first context switch
   saves the current registers *into* `g_idle_task.cpu_ctx`,
   overwriting the undefined bytes with real ones.
2. `msr tpidr_el1, ...` publishes idle as "the current task" by
   writing its pointer into the system register that
   `nx_task_current` reads on the kernel build. From this line
   forward, every kernel function that calls
   `nx_task_current()` gets a real task back.
3. `enqueue(&g_idle_task)` puts idle on the runqueue as a
   permanent fallback. `pick_next` will pass it over whenever
   anything else is ready, but it will return it when nothing
   else is — that is the property we want.

So "the idle task" is not a thing the scheduler creates. It's a
**rename**: the boot context, which was running anonymously, is
told to call itself `g_idle_task` and start playing by the
scheduler's rules.

[`core/boot/boot.c`](../core/boot/boot.c) calls `sched_start`
near the end of `boot_main`, after the framework's components
are all `ACTIVE`. From there the boot CPU either drops into the
idle loop (real builds) or branches into the kernel-test
harness (`NX_KTEST` builds). Either way the CPU is now "running
as the idle task" and the scheduler is fully in charge.

---

## The reschedule shim, line by line

This is the centre of the chapter. Every other piece exists so
that this function works correctly.

`sched_check_resched` runs in two situations:

1. **On every IRQ return.** The vector-table chain ends in an
   assembly stub (`_irq_stub` in `core/cpu/vectors.S`) that
   calls `sched_check_resched` before `eret`. Whatever
   interrupted us — timer, UART RX, GIC SPI — we now have one
   chance to switch tasks before returning to the interrupted
   code.
2. **From `nx_task_yield`**. Voluntary callers don't go through
   the IRQ machinery, but they still want the same logic.
   `nx_task_yield` flags `need_resched` on the current task and
   calls `sched_check_resched` directly.

Here is the body of the function, with the signal-delivery
sliver removed (covered in chapter 17) and the MMU details
deferred:

```c
void sched_check_resched(void)
{
    struct nx_task *curr = nx_task_current();
    if (!curr) return;
    if (!g_sched_ops) return;

    /* ... signal-delivery check ... */

    if (!curr->need_resched) return;
    if (curr->preempt_count > 0) return;

    curr->need_resched = 0;

    g_sched_ops->yield(g_sched_self);

    struct nx_task *next = g_sched_ops->pick_next(g_sched_self);
    if (!next || next == curr) {
#if !__STDC_HOSTED__
        asm volatile("wfi");
#endif
        return;
    }

    nx_preempt_disable();
    struct nx_hook_context hctx = {
        .point = NX_HOOK_CONTEXT_SWITCH,
        .u.csw = { .prev = curr, .next = next },
    };
    nx_hook_dispatch(&hctx);

    /* ... TTBR0 / TPIDR_EL0 swap (chapter 15) ... */

    cpu_switch_to(curr, next);

    nx_preempt_enable();
}
```

Twelve lines (give or take the deferred details), and they cover
every flavour of "should we switch?". Walk through them:

1. **Get the current task.** If `TPIDR_EL1` is `NULL`, we're in
   the pre-`sched_start` no-task-identity window and there is
   nothing to do.
2. **Get the policy.** If `sched_init` hasn't run yet
   (`g_sched_ops == NULL`), we have no policy to ask. Return.
3. **Check the flag.** `need_resched == 0` means nothing has
   asked for a switch. Return.
4. **Check the brake.** `preempt_count > 0` means a critical
   section explicitly disabled preemption. Even with the flag
   set, we don't switch — but `need_resched` stays set, so the
   next IRQ-return after `preempt_count` drops back to zero
   will hit this path again and switch.
5. **Clear the flag.** From here on we are committed to either
   switching or finding no one to switch to. Clearing first
   keeps reentrant calls (a hook firing during the rotation)
   from looping.
6. **Let the policy rotate.** `g_sched_ops->yield(...)` lets
   the policy reshape its queue however it likes — round-robin
   moves head to tail, priority rotates within a bucket. The
   *driver* doesn't know which.
7. **Ask the policy for the next task.** `pick_next` returns a
   task or `NULL`.
8. **Handle the empty-runqueue case.** If `pick_next` returns
   `NULL` (no tasks at all — shouldn't happen on real builds
   because idle is always queued) or returns the *same task we
   were running*, we have nothing to switch *to*. Issuing
   `wfi` puts the CPU into a low-power state until the next
   interrupt, then we return. This is the "we'd be busy-waiting"
   case and the one optimisation in the whole routine.
9. **Disable preemption for the switch.** Hooks run between
   "we know we are switching" and the actual save/load. We
   don't want a timer tick to fire inside the hook and trigger
   another rotation against the same task.
10. **Fire the hook chain.** `NX_HOOK_CONTEXT_SWITCH` lets any
    instrumented hook see `{prev, next}`. Chapter 13 explains
    hooks.
11. **The actual switch.** `cpu_switch_to(curr, next)` saves
    `curr`'s callee-saved registers + SP + DAIF into
    `curr->cpu_ctx`, loads `next`'s into the CPU, writes
    `&next` into `TPIDR_EL1`, and returns to whatever address
    `next->cpu_ctx.x30` holds. We did all of that line by line
    in chapter 7.
12. **`nx_preempt_enable` runs on the freshly-resumed task.**
    Note: not on `curr`. `curr` is suspended at the line above.
    The next time `curr` is picked, `cpu_switch_to` will return
    into `curr`'s saved frame *here*, and *then* this
    `nx_preempt_enable` runs. Two different tasks, one source
    line.

Reading the function this way — driver, then policy, then
driver again, then assembly, then back to driver on a different
task's stack — is the trick to understanding what code runs
where. Every line of `sched_check_resched` runs on the
**outgoing** task's stack except the `preempt_enable` at the
end, which always runs on the **incoming** task's stack the
next time that task is picked.

> The clear-flag-then-decide ordering matters. If we cleared
> the flag *after* the switch we'd have to do it inside
> `cpu_switch_to`, which is assembly. Clearing it before keeps
> the assembly small and keeps the invariant in C where it's
> readable.

---

## The timer half: `sched_tick`

`sched_tick` is the function chapter 4 stubbed at. The timer
ISR (`on_tick` in `core/timer/timer.c`) calls it once per
hardware tick. `sched.c`:

```c
void sched_tick(void)
{
    if (!g_sched_ops) return;
    g_sched_ops->tick(g_sched_self);
    nx_waitq_tick_deadlines();
}
```

Three things. Bail if no policy is wired yet (boot-time guard).
Hand the tick to the policy — `pick_next` is not called from
`tick`; the policy only updates accounting and (maybe) sets
`need_resched`. Then walk the global deadline list (a wait-queue
mechanism we'll see below).

What "updates accounting" means depends on the policy. In
`sched_rr_tick`:

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

A single counter, decremented each tick, with a wrap that flags
the current task for reschedule when it hits zero. The flag
won't be acted on yet — `sched_tick` is running from the timer
ISR, and the IRQ-return path will call `sched_check_resched`
shortly afterwards. That's where the actual switch happens.

`sched_priority_tick` is identical in shape — it shares the
single `remaining` counter (the priority decision is made by
`pick_next`, not by quantum accounting). Both default
`quantum_ticks = 2`, so 200 ms at 10 Hz.

The chain is therefore:

```
hardware timer interrupt
   |
   v
on_tick (core/timer/timer.c)
   |
   v
sched_tick (core/sched/sched.c)
   |
   v
g_sched_ops->tick (sched_rr_tick or sched_priority_tick)
   |
   v
if quantum expired: curr->need_resched = 1
   |
   v
[ ISR returns ]
   |
   v
sched_check_resched (called by _irq_stub before eret)
   |
   v
sees need_resched == 1; performs cpu_switch_to
```

The flag is the seam. It lets the timer ISR run as
fast as possible — just a counter decrement and maybe a one-byte
write — while pushing the heavy work (the actual switch) onto
the IRQ-return path that was going to run anyway.

---

## Voluntary yield: `nx_task_yield`

`nx_task_yield` is the cooperative half of preemption. A task
that knows it has nothing useful to do right now calls it to
give the CPU to whoever's next:

```c
void nx_task_yield(void)
{
    struct nx_task *curr = nx_task_current();
    if (!curr) return;
    curr->need_resched = 1;
    sched_check_resched();
}
```

Three lines. Flag the current task, then call the same shim the
IRQ-return path uses. Everything we walked through in the
previous section applies identically. The only difference is
context: `nx_task_yield` runs from a kthread body, not from an
ISR. The header notes that it **must not** be called from an
ISR — a thread can yield itself, but an ISR can't, because the
ISR is borrowing the interrupted thread's stack and identity.

This unification — voluntary and involuntary switching using
the same code path — is one of the small designs that keep the
scheduler comprehensible. There is exactly one place where a
switch happens, and you can find it by grepping for
`cpu_switch_to`.

The framework's IPC dispatcher uses `nx_task_yield` (or its
wait-queue cousin) to park itself when its message queue is
empty. Chapter 11 will trace that flow.

---

## Policy one: round-robin

[`components/sched_rr/sched_rr.c`](../components/sched_rr/sched_rr.c)
is 326 lines, most of which are lifecycle plumbing or
defensive checks. The actual scheduling logic is small enough
to fit on one screen.

State:

```c
struct sched_rr_state {
    struct nx_list_head runqueue;
    unsigned            quantum_ticks;
    unsigned            remaining;
    /* lifecycle counters, omitted */
};
```

One linked list. The intrusive list head sits in the policy's
state; each task carries an `nx_list_node sched_node` that
threads it onto whatever list the scheduler currently has it
on. (Chapter 7 introduced `sched_node` — it's the same field.)

Enqueue:

```c
static int sched_rr_enqueue(void *self, struct nx_task *task)
{
    struct sched_rr_state *s = self;
    if (on_queue(s, task)) return NX_EEXIST;
    nx_list_add_tail(&s->runqueue, &task->sched_node);
    /* ... idle-preemption signal: see below ... */
    return NX_OK;
}
```

New task goes on the tail of the list. The `on_queue` check is
`O(n)` but cheap — runqueues stay short, and double-enqueue is
a programming error we want to catch.

Pick next:

```c
static struct nx_task *sched_rr_pick_next(void *self)
{
    struct sched_rr_state *s = self;
    struct nx_list_node *n;
    nx_list_for_each(n, &s->runqueue) {
        struct nx_task *t = nx_list_entry(n, struct nx_task, sched_node);
        if (t->state != NX_TASK_BLOCKED) return t;
    }
    return NULL;
}
```

Walk from the head. Return the first non-blocked task. Why the
`BLOCKED` skip when blocked tasks are supposed to be off the
runqueue? Defence in depth — if some future caller forgets to
`dequeue` before flipping state, we shouldn't return that task
and let it lock the scheduler in a queue-rotates-but-nobody-wakes
state. Idle is always `READY` at the tail, so this loop always
finds something.

Yield:

```c
static void sched_rr_yield(void *self)
{
    struct sched_rr_state *s = self;
    if (nx_list_empty(&s->runqueue)) return;
    struct nx_list_node *head = s->runqueue.n.next;
    nx_list_remove(head);
    nx_list_add_tail(&s->runqueue, head);
    s->remaining = s->quantum_ticks;
}
```

Move the head to the tail. Reset the quantum so the new head
starts with a full slice. That's the **round-robin rotation**:
a circular list where each turn through, the next task in the
ring runs.

Set priority:

```c
static int sched_rr_set_priority(void *self, struct nx_task *task, int priority)
{
    return NX_EINVAL;
}
```

Round-robin doesn't know what priority means. The contract on
the interface is "policies that don't support priorities
return `NX_EINVAL`", and that's what we do. The caller — a
busybox `nice` for example — sees the error and treats this
policy as priority-blind.

That is the entire scheduler. Six small functions and one
linked list. The interface boundary is doing the work of making
this readable: nothing else in the kernel needs to know that
round-robin is "one list and a rotate".

---

## The "wake idle" trick

One small detail in `sched_rr_enqueue` is worth pulling out:

```c
#if !__STDC_HOSTED__
{
    extern struct nx_task g_idle_task;
    struct nx_task *curr = nx_task_current();
    if (curr == &g_idle_task && task != &g_idle_task)
        curr->need_resched = 1;
}
#endif
```

The setup: the CPU is sitting in idle's `wfi` loop because
nothing else was ready. A different thread runs (could be the
timer ISR, could be the GIC handing us a UART RX) and that
thread's handler enqueues a new task — say, the dispatcher
re-becoming runnable because a message arrived.

Without this branch, the IRQ-return path would call
`sched_check_resched`, see `idle->need_resched == 0` (nothing
asked idle to step aside), return without switching, and idle
would go back into `wfi`. The newly-runnable task would sit on
the runqueue until *idle's* quantum expired — up to 200 ms of
latency, completely avoidable.

The fix is one line. When enqueue notices the running task is
idle and the new task isn't, flag idle for reschedule. Now the
IRQ-return path sees the flag, calls `pick_next`, gets the new
task, and switches immediately. `sched_priority_enqueue` has
the same paragraph.

Small details like this are what separate "compiles and seems
to work" from "feels right under load".

---

## Policy two: fixed-priority

[`components/sched_priority/sched_priority.c`](../components/sched_priority/sched_priority.c)
is the second shipped policy. Same interface, different
structure.

State:

```c
#define SCHED_PRIORITY_LEVELS  8

struct sched_priority_state {
    struct nx_list_head queues[SCHED_PRIORITY_LEVELS];
    unsigned            quantum_ticks;
    unsigned            remaining;
};
```

Eight runqueues instead of one. Each bucket is a FIFO list,
indexed 0 (default, lowest) through 7 (highest). The
quantum counter and the lifecycle counters are the same.

Pick next:

```c
static struct nx_task *sched_priority_pick_next(void *self)
{
    struct sched_priority_state *s = self;
    for (int p = SCHED_PRIORITY_LEVELS - 1; p >= 0; p--) {
        struct nx_list_node *n;
        nx_list_for_each(n, &s->queues[p]) {
            struct nx_task *t = nx_list_entry(n, struct nx_task, sched_node);
            if (t->state != NX_TASK_BLOCKED) return t;
        }
    }
    return NULL;
}
```

Scan from the highest bucket down. Within each bucket return
the first non-blocked task. The two-level structure gives
priorities for free: a task at level 7 always beats one at
level 6, no matter how many tasks are at 6. Within a level,
FIFO order keeps things fair.

Enqueue puts new tasks in bucket 0 (default priority):

```c
static int sched_priority_enqueue(void *self, struct nx_task *task)
{
    nx_list_add_tail(&s->queues[SCHED_PRIORITY_DEFAULT], &task->sched_node);
    /* (idle-preemption signal, same as sched_rr) */
    return NX_OK;
}
```

A task moves to a different bucket via `set_priority`:

```c
static int sched_priority_set_priority(void *self, struct nx_task *task,
                                        int priority)
{
    if (priority < 0 || priority >= SCHED_PRIORITY_LEVELS) return NX_EINVAL;
    int cur_p = task_priority(s, task);
    if (cur_p < 0) return NX_ENOENT;
    if (cur_p == priority) return NX_OK;
    nx_list_remove(&task->sched_node);
    nx_list_add_tail(&s->queues[priority], &task->sched_node);
    return NX_OK;
}
```

Look up which bucket the task is currently in (`task_priority`
walks all eight in turn — `O(levels × queue length)`), validate
the new value, move it. Tasks not on any queue get
`NX_ENOENT`; out-of-range values get `NX_EINVAL`. The same
function call that round-robin rejects with `NX_EINVAL`
actually does something here.

Yield within a bucket:

```c
static void sched_priority_yield(void *self)
{
    struct nx_task *curr = nx_task_current();
    if (curr) {
        int p = task_priority(s, curr);
        if (p >= 0) {
            struct nx_list_node *node = &curr->sched_node;
            nx_list_remove(node);
            nx_list_add_tail(&s->queues[p], node);
            s->remaining = s->quantum_ticks;
            return;
        }
    }
    /* ... fallback for host tests (no current task) ... */
}
```

Move the *current* task to the tail of *its own bucket*. This
is the fairness within a priority level: two tasks at the same
priority share the CPU in round-robin order, while a higher
task — should one become runnable — preempts both regardless
of yield order.

The reason this `yield` reads `nx_task_current()` instead of
operating on the head (like `sched_rr_yield` does) is that the
*head* of the highest non-empty bucket might be a task that
just got preempted by a higher-priority task arriving. We want
to rotate the task that was actually running, which is the
current task.

The rest of the policy (`tick`, `dequeue`, `runqueue_size`)
mirrors the round-robin one almost line for line. The
fundamental shape is the same — interface contract is the
same. Only the data structure and the pick rule differ.

---

## Idle, WFI, and the empty-runqueue case

Idle sits at the bottom of every scheduler decision. We met it
in `sched_start` (the boot context renamed); we meet it again
in `pick_next` (the permanent fallback at the tail of the
runqueue). The third place is the `wfi` branch in
`sched_check_resched`:

```c
struct nx_task *next = g_sched_ops->pick_next(g_sched_self);
if (!next || next == curr) {
    asm volatile("wfi");
    return;
}
```

Two cases trigger `wfi`:

1. **`pick_next` returned `NULL`.** Empty runqueue. Shouldn't
   happen on a real build because idle is always queued, but
   the check is cheap defensive code.
2. **`pick_next` returned the same task we're already on**
   (most often: idle, with no other ready task). Switching to
   yourself would be a no-op that wastes a save/load, so we
   skip the switch and `wfi` instead.

`wfi` ("wait for interrupt") halts the CPU's clock until any
unmasked interrupt arrives — typically the next timer tick, but
also any pending IRQ from the GIC. When the IRQ fires, the CPU
wakes, the ISR runs, and on IRQ return `sched_check_resched`
runs again. If a new task became ready during the ISR,
`pick_next` returns it this time.

This is the low-power half of "idle is just a task". A naive
idle loop would burn 100% of one CPU core's energy budget for
no reason. `wfi` lets the silicon sleep in between ticks.

There is also a body to the idle task on real builds — a
`while (1) { asm volatile("wfi"); }` loop in `boot_main` (in
the `NX_KTEST == 0` branch). The `wfi` in
`sched_check_resched` covers the inside-the-scheduler case;
the loop body covers the case where idle was picked normally
and is now running.

---

## Wait queues: when tasks need to step off

Most of the time a task is either runnable (on the runqueue,
state `READY` or `RUNNING`) or done (zombie / `ZOMBIE`). But
between those there's a third state: **blocked**. The task is
alive and will eventually want to run, but right now there's
nothing for it to do because some event hasn't happened yet —
the UART hasn't received a byte, the child process hasn't
exited, the deadline hasn't elapsed.

The mechanism is the **wait queue**, in
[`core/sched/waitq.h`](../core/sched/waitq.h) and
[`core/sched/waitq.c`](../core/sched/waitq.c). The contract:

```c
struct nx_waitq { struct nx_list_head waiters; };

void nx_waitq_init(struct nx_waitq *wq);
int  nx_waitq_wait_with_deadline(struct nx_waitq *wq, uint64_t budget_ns);
void nx_waitq_wake_one(struct nx_waitq *wq);
void nx_waitq_wake_all(struct nx_waitq *wq);
```

`wait_with_deadline` does four things on the calling task:

1. **Dequeue from the runqueue.** The scheduler's `dequeue` op
   pulls the task off whatever bucket it was on.
2. **Link onto `wq->waiters`** via the same `sched_node` —
   blocked tasks reuse the field they were previously occupying
   on the runqueue.
3. **Set state to `BLOCKED`.**
4. **Call `nx_task_yield`.** The scheduler picks someone else.

When `nx_waitq_wake_one` runs (from an ISR or another
kthread), the wait queue does the reverse: unlinks the head
waiter, sets state to `READY`, re-enqueues onto the scheduler,
and the task will run again the next time `pick_next` reaches
it.

The `with_deadline` half — that `uint64_t budget_ns` — lets a
caller say "wait, but no longer than 200 ms". Tasks with
finite deadlines also link into a global `g_deadline_list`,
which `sched_tick` walks once per tick to expire any waiters
whose time is up. An expired wait returns `NX_EDEADLINE`
instead of `NX_OK`, and the caller can distinguish "the event
fired" from "I gave up waiting".

Wait queues unify a lot of disparate behavior. Without them,
every blocking call in the kernel would have its own ad-hoc
"park here, wake there" code, and most of them would have at
least one subtle bug. With them, blocking is a primitive: pick
a wait queue, call wait, the scheduler does the rest. The
UART RX path uses one. `sys_wait` (parent waits for child)
uses one. `sys_ppoll` uses one. Each appears in its respective
chapter; we just need to know the shape now.

> The `BLOCKED` state on the runqueue is a *defensive* state.
> The wait-queue path always dequeues before flipping the
> state, so a `BLOCKED` task shouldn't appear on the
> runqueue at all. The check inside `pick_next` is there in
> case a future caller forgets. It costs one branch per
> queued task and prevents a class of "scheduler picks a
> task that can't actually run" bugs that would be brutal to
> debug.

---

## A worked timeline: two kthreads under sched_rr

To tie everything together, let's walk through what happens
when two kernel threads share one CPU under round-robin, with
a 200 ms quantum. Three tasks total: idle, A, B.

Time 0 ms: the boot CPU is running as idle, in `wfi`. A and
B haven't been spawned yet.

Time 1 ms: `sched_spawn_kthread("A", ...)` runs. The new
task A is queued. The enqueue path notices the running task is
idle and a non-idle task is being added, so it sets
`idle->need_resched = 1`.

Time 1 ms (immediately after): the next IRQ — could be the
timer tick that's coming up at 100 ms, or it could be that
spawn happened inside an existing ISR. On IRQ return,
`sched_check_resched` sees `idle->need_resched == 1`, calls
`pick_next`, gets task A (idle is at the tail now after the
rotate), calls `cpu_switch_to(idle, A)`. A starts running.

Time 5 ms: same story for B. A is currently running, so the
enqueue just appends B to the tail. No wake-idle signal —
we're not in idle.

```
runqueue at this point:  [A] -> [B] -> [idle]
                          ^
                          running
```

Time 100 ms: timer tick. `sched_tick` runs.
`sched_rr_tick` decrements `remaining` from 2 to 1. No
reschedule yet.

Time 200 ms: timer tick. `sched_rr_tick` decrements
`remaining` from 1 to 0, sets `A->need_resched = 1`, resets
`remaining` to 2.

Time 200 ms (immediately after, on IRQ return):
`sched_check_resched` runs. Sees `need_resched`. Sees no
`preempt_count`. Calls `sched_rr_yield`, which moves A from
head to tail:

```
runqueue:  [B] -> [idle] -> [A]
            ^
            about to run
```

`pick_next` returns B. `cpu_switch_to(A, B)`. B starts
running, picking up where it last left off — its first time
running means landing at the bootstrap thunk and calling its
entry function (chapter 7).

Time 400 ms: same routine. B's quantum expires. `yield`
rotates again:

```
runqueue:  [idle] -> [A] -> [B]
            ^
            wait, idle?
```

Yes, idle is now at the head. `pick_next` returns idle. The
switch happens. The CPU runs idle for one quantum.

But wait — idle is just a `wfi` loop, and we've already
discussed that the moment any non-idle task is enqueued, idle
gets `need_resched`. So in practice, idle being picked here
just means we sit in `wfi` until the next timer tick, which
flags reschedule, which rotates the queue back to A. So
effectively:

```
runqueue:  [A] -> [B] -> [idle]
            ^
            running again
```

Time 600 ms onward: this pattern repeats. A runs for 200 ms,
B runs for 200 ms, idle runs for one tick interval (and
mostly sleeps), repeat.

> If you're following along with a debugger, the wake-idle
> signal we mentioned earlier means idle's quantum doesn't
> always last a full tick. Once enough tasks are running, the
> CPU rarely visits idle at all — the runqueue keeps rotating
> between real tasks and idle stays parked at the tail.

That is the entire mechanism, applied. Three tasks, one CPU,
no priorities, no complications, and the system is making
progress on all of them in a way that anyone with a
millisecond-resolution clock can see.

---

## A few extra things to know

- **Why is the policy a component, not just a `#define`?**
  Compile-time `#define`-style swappability lets you ship two
  builds with two schedulers. Component-style swappability lets
  you ship *one* build with two schedulers wired in and *swap
  between them at runtime*. Chapter 14 makes this concrete with
  a worked live swap. The component abstraction also gives us
  separate `init` / `enable` / `disable` / `destroy` lifecycle
  hooks per policy, which is what `enable` uses to plug itself
  into `sched_init`.

- **The dual-scheduler build.** Today's `kernel.json` declares
  `"scheduler": "sched_priority"` and
  `"alternatives": ["sched_rr"]`. Both policies compile into the
  same kernel image; only the bound one's `enable` fires on
  boot. The other sits in `READY` state, waiting for chapter
  14's `nx_config_swap_component` to make it the new active
  policy. This is also why both policies' `enable` hooks call
  `sched_init` — on a live swap, whichever wins must re-publish
  itself before the next tick.

- **`preempt_count` is not a recursion counter.** It is a
  *boolean disguised as an integer*. Any value > 0 means
  "do not preempt". The reason it's an integer is so nested
  critical sections can each `disable` on entry and `enable`
  on exit without coordinating: enter, enter, exit, exit
  works whether there's one critical section or four nested.

- **No SMP locking inside the policy.** Phase 4 is single-CPU,
  so the runqueue manipulations are safe by construction: the
  only code that touches them runs from either the timer ISR
  (which can't interrupt itself) or the IRQ-return path or a
  kthread call to `nx_task_yield` (which is in the kthread's
  own kernel stack). When nonux acquires more cores, every
  policy will need a per-policy lock around `enqueue` /
  `dequeue` / `pick_next` / `yield`. The interface contract
  already allows this — the policy's `void *self` is the
  natural place to hang a lock.

- **`reap_task` exists because of zombies.** When a task exits
  (chapter 17's `exit()` or `nx_process_exit`), it can't free
  its own kernel stack — it's still running on it. The dying
  task dequeues itself, sets state to `ZOMBIE`, switches away
  for the last time, and a different task (the parent's
  `wait`, or the framework's reaper) eventually calls
  `sched_reap_task` which calls the policy's `reap_task`
  which calls `nx_task_destroy` to free the storage.

- **Round-robin's `set_priority` rejects everything.** The
  contract says "policies that don't support priorities
  return `NX_EINVAL` uniformly". This is what lets a caller
  say "set the priority if you can; ignore the error if you
  can't" without checking the active policy first. Round-robin
  follows it; fixed-priority overrides it.

- **The `WFE` cousin.** ARM64 also has `WFE` ("wait for
  event"), which is `wfi`'s relative for inter-CPU
  notifications. nonux uses `wfi` because it's single-CPU
  and the scheduler is interrupt-driven; an SMP version might
  use `WFE` plus `SEV` for cross-CPU wake-ups.

---

## Where to read more

- [`core/sched/sched.h`](../core/sched/sched.h),
  [`core/sched/sched.c`](../core/sched/sched.c) — the core
  driver. Start with `sched_check_resched`; everything else
  is supporting cast.
- [`interfaces/scheduler.h`](../interfaces/scheduler.h) — the
  contract. The per-op comments are the canonical reference
  for what each op promises.
- [`components/sched_rr/sched_rr.c`](../components/sched_rr/sched_rr.c)
  — round-robin, complete. Read top to bottom; it's short.
- [`components/sched_priority/sched_priority.c`](../components/sched_priority/sched_priority.c)
  — fixed-priority. Compare the `pick_next` / `enqueue` /
  `yield` to round-robin's: same interface, different shape.
- [`core/sched/waitq.h`](../core/sched/waitq.h),
  [`core/sched/waitq.c`](../core/sched/waitq.c) — the
  blocking primitive. The "lost-wakeup-safe wait pattern"
  comment in the header is the part to re-read once a year.
- [Chapter 4 §"From flag to switch"](04-timer-and-ticks.md#from-flag-to-switch-where-the-actual-preemption-happens)
  — the timer-tick origin of `need_resched`, finally answered
  end to end by this chapter.
- [Chapter 7 §"`cpu_switch_to`, instruction by instruction"](07-kernel-threads-and-context-switch.md#cpu_switch_to-instruction-by-instruction)
  — the 22-instruction assembly the reschedule shim ends in.
- Linux's `kernel/sched/core.c` `__schedule` — the
  big-kernel analogue of `sched_check_resched`. Same shape,
  many more concerns (SMP, RT, deadlines, eBPF hooks, …).
- The seL4 scheduler is interesting reading if you want to
  see how this gets simpler under formal verification —
  fixed-priority with bounded ops only.
- The ARM Architecture Reference Manual section on `WFI` is
  the one-page primary source for the low-power half of this
  chapter.
