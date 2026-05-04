# sched_priority

Fixed-priority scheduler policy — the second policy component under the
split-scheduler architecture (slice 8.4).  Binds to the `scheduler` slot
in `kernel.json` when priority-aware scheduling is wanted.  Swappable at
build time (or live, via slice 8.5) with any other policy that passes the
conformance suite from slice 4.2.

## Interface

- **iface:** `scheduler`
- **Dependencies:** none.
- **Worker threads:** none (`spawns_threads: false`).
- **Pause hook:** not required (`pause_hook: false`).

## Behaviour

`SCHED_PRIORITY_LEVELS` (8) per-priority FIFO buckets, numbered 0 (default /
lowest) through 7 (highest).  Each bucket is an intrusive `nx_list_head`.

- `enqueue` appends to `queues[0]` (default priority).
- `pick_next` scans from bucket 7 down to 0, returning the first non-BLOCKED
  task.  Multiple tasks at the same priority share the CPU in FIFO order.
- `dequeue` removes from whichever bucket holds the task (O(n) scan).
- `yield` rotates the current task within its own bucket (move head → tail),
  then resets the quantum counter.  Falls back to rotating the head of the
  highest non-empty bucket when `nx_task_current()` returns NULL (host tests).
- `set_priority` moves the task to the requested bucket.  Returns `NX_OK` for
  priorities 0–7, `NX_EINVAL` otherwise.  Returns `NX_ENOENT` if the task is
  not on any queue.
- `tick` decrements the remaining quantum; sets `need_resched` and resets the
  counter when it reaches zero.

## State

```c
struct sched_priority_state {
    struct nx_list_head queues[SCHED_PRIORITY_LEVELS];
    unsigned            quantum_ticks;   /* set at init */
    unsigned            remaining;       /* ticks left in current slice */
    /* lifecycle counters for test introspection */
    unsigned            init_called;
    unsigned            enable_called;
    unsigned            disable_called;
    unsigned            destroy_called;
};
```

## Symbols exported

- `sched_priority_scheduler_ops` — `nx_scheduler_ops` table consumed by the
  core driver via `sched_init`.
- `sched_priority_component_ops` — lifecycle table (`init`/`enable`/`disable`/
  `destroy`/`handle_msg`).
- `sched_priority_descriptor` — auto-emitted by
  `NX_COMPONENT_REGISTER_NO_DEPS_IFACE`.
- `sched_priority_purge_user_tasks(void *self, struct nx_task *keep)` —
  test-only: drains stranded user-process tasks from all priority buckets.
- `sched_rr_purge_user_tasks` — build-compatibility shim pointing at the above,
  provided so the 20+ ktest teardown helpers that `extern`-declare the sched_rr
  name keep compiling without modification when sched_priority is the active
  scheduler.
