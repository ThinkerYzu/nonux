# sched_rr

Round-robin scheduler policy — the first real policy component under
the split-scheduler architecture introduced in slice 4.1. Binds to the
`scheduler` slot declared in `kernel.json`. Swappable at build time with
any other policy that passes the conformance suite from slice 4.2.

## Interface

- **iface:** `scheduler`
- **Bound by default:** `kernel.json["components"]["scheduler"]`.
- **Dependencies:** none.
- **Worker threads:** none (`spawns_threads: false`).
- **Pause hook:** not required (`pause_hook: false`).

## Behaviour

- Runqueue is an intrusive `struct nx_list_head` over every task's
  embedded `sched_node`. No allocation — borrow semantics per
  `interfaces/scheduler.h`.
- `enqueue` appends to the tail; `pick_next` returns the head without
  dequeuing (idempotent against the same queue state).
- `yield` rotates: the head moves to the tail and the current task's
  remaining quantum is reset. Cooperative-only in slice 4.3; the
  IRQ-return reschedule shim that consumes `need_resched` lands in
  4.4.
- `tick` decrements `remaining`. When it hits zero the current task is
  flagged `need_resched` and `remaining` resets to `quantum_ticks`.
- `set_priority` returns `NX_EINVAL` uniformly — round-robin doesn't
  honour priorities, matching the conformance contract.

## State

```c
struct sched_rr_state {
    struct nx_list_head runqueue;
    struct nx_task     *current;
    unsigned            quantum_ticks;
    unsigned            remaining;
    /* + counters observable by kernel/host tests: init/enable/disable/destroy */
};
```

`quantum_ticks` is set at `init` to `SCHED_RR_DEFAULT_QUANTUM_TICKS`
(10 — one second at the current 10 Hz kernel timer, deliberately
conservative). A future `kernel.json` config pass will expose it as a
per-component knob.

## Symbols exported

- `extern const struct nx_scheduler_ops sched_rr_scheduler_ops;` — the
  interface table the core scheduler driver will consume through
  `sched_init` in slice 4.4.
- `extern const struct nx_component_ops sched_rr_component_ops;` —
  framework-facing lifecycle table (`init` / `enable` / `disable` /
  `destroy`). `handle_msg` intentionally NULL: the scheduler is called
  directly, not IPC-driven.
- `extern const struct nx_component_descriptor sched_rr_descriptor;`
  (auto-emitted by `NX_COMPONENT_REGISTER_NO_DEPS`).

## Why this component exists

Slice 4.3 completes the interface → conformance → component path:

```
interfaces/scheduler.h  ─────────┐
(slice 4.2 defines ops contract)  │
                                  ▼
conformance_scheduler.{h,c}  ─────┐
(slice 4.2 harness)                │
                                   ▼
components/sched_rr/  ─────────── passes every case
(this slice)                       plus lifecycle cycling
                                   │
                                   ▼
                    kernel.json binds "scheduler" → sched_rr
                    nx_framework_bootstrap brings it up in NX_LC_ACTIVE
```

No other scheduler policy is needed for Phase 4; `sched_priority` is
deferred to Phase 8's real recomposition work.

## Future

- **Slice 4.4** wires `sched_rr_scheduler_ops` into the core driver via
  `sched_init`, plumbs the timer tick through `sched_tick` →
  `sched_rr_tick`, and replaces `boot_main`'s `wfi` with
  `sched_start()`. Quantum accounting becomes observable once
  preemption is live.
- **Slice 3.9b** introduces the per-CPU dispatcher kthread — at that
  point `sched_spawn_kthread` is the primitive that spawns the
  dispatcher, and runtime `NX_HOOK_SLOT_SWAPPED` dispatch lets other
  subsystems subscribe to scheduler swaps.
- **Phase 8** adds the real runtime recomposition path (`timer_pause`
  / rewire / `timer_resume`) that will let a kernel swap from `sched_rr`
  to a hypothetical `sched_priority` live. `sched_rr`'s `mutability`
  becomes meaningful then.
