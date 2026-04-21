# Component lifecycle — `framework/component.{h,c}`

Components are the unit of composition. Each one is a registered
implementation of some interface (scheduler, VFS, block device, …)
that plugs into a slot in the [registry](framework-registry.md).

This module layers the following on top of the raw registry:

1. **Lifecycle state machine** — six verbs + legal transition matrix.
2. **`struct nx_component_ops`** — the typed callback table every
   component implements.
3. **Pause protocol** — cutoff → drain → `pause_hook` → `ops->pause` →
   DONE, with symmetric resume + hold-queue flush.
4. **Destroy guard** — a bound component cannot be destroyed.
5. **Dependency injection** — descriptor types,
   `NX_COMPONENT_REGISTER` macro, linker-section walker hook,
   `nx_resolve_deps`.

Components declare their dependencies in a manifest; the build
pipeline translates each manifest into a `gen/<name>_deps.h` header
that the component `#include`s and passes to `NX_COMPONENT_REGISTER`.
The resulting descriptor lives in the `nx_components` linker section;
the boot walker (slice 3.9) walks that section and brings components
up in dependency order.

---

## Lifecycle state machine

```
  UNINIT ──init──► INIT ──►──┐
     │                       ▼
     └─────────────────────► READY ──enable──► ACTIVE
                               ▲                  │
                               │                  ▼
                 disable ──────┼──────pause──► PAUSED
                               │                  │
                               └──────────────────┘
                                       resume: PAUSED → ACTIVE
                                       disable: PAUSED → READY
                 destroy: READY → DESTROYED (terminal)
```

**Notes:**

- `nx_component_init` drives the whole `UNINIT → READY` transition
  in one call (v1 components are synchronously initialised). The
  separate `INIT` state is reserved for future async bring-up;
  `INIT → READY` is already in the matrix so the framework can
  split `init` later without breaking it.
- `DESTROYED` is terminal — no resurrection. Every verb on a
  `DESTROYED` component returns `NX_ESTATE`.
- `disable` accepts both `ACTIVE` and `PAUSED`.
- `pause` and `resume` are only legal on `ACTIVE` and `PAUSED`
  respectively — see the pause protocol below for what happens
  beyond the matrix edge.

### Matrix query

```c
bool        nx_lifecycle_transition_legal(enum nx_lifecycle_state from,
                                          enum nx_lifecycle_state to);
const char *nx_lifecycle_state_name      (enum nx_lifecycle_state s);
```

- `transition_legal` answers the "is this edge in the matrix?"
  question. The six verbs call it internally; external code can
  use it for consistency checks.
- `state_name` returns `"uninit" / "init" / "ready" / "active" /
  "paused" / "destroyed"` or `"unknown"`. Returned pointer is to
  static storage.

---

## Lifecycle verbs

```c
int nx_component_init   (struct nx_component *c);  /* UNINIT → READY  */
int nx_component_enable (struct nx_component *c);  /* READY  → ACTIVE */
int nx_component_pause  (struct nx_component *c);  /* ACTIVE → PAUSED */
int nx_component_resume (struct nx_component *c);  /* PAUSED → ACTIVE */
int nx_component_disable(struct nx_component *c);  /* ACTIVE|PAUSED → READY */
int nx_component_destroy(struct nx_component *c);  /* READY  → DESTROYED */
```

**Return codes (common to all six):**

| Code          | Meaning                                                |
|---------------|--------------------------------------------------------|
| `NX_OK`       | Transition applied.                                    |
| `NX_EINVAL`   | `c == NULL`.                                           |
| `NX_ENOENT`   | `c` is not in the registry.                            |
| `NX_ESTATE`   | Current state does not permit this verb.               |

**Extra for `enable` / `disable` / `pause` / `resume`:**

| Code          | Meaning                                                |
|---------------|--------------------------------------------------------|
| `NX_EABORT`   | A hook registered on the matching `NX_HOOK_COMPONENT_*` point returned `NX_HOOK_ABORT`. |

**Extra for `pause`:** whatever non-zero code `ops->pause_hook` or
`ops->pause` returns when they fail. The component stays in
`ACTIVE` in that case; slot pause state may be mid-protocol (see
the pause section below). Slice 3.9 adds kernel-side rollback.

**Extra for `destroy`:**

| Code          | Meaning                                                |
|---------------|--------------------------------------------------------|
| `NX_EBUSY`    | `nx_component_is_bound(c)` — a slot still has `c` as its `active`. Unbind first. |

Every successful transition goes through `nx_component_state_set`,
so the `NX_EV_COMPONENT_STATE` event fires on every change and the
graph generation bumps.

---

## Pause protocol

`nx_component_pause` does more than flip a state flag. Its job is
to bring every slot the component is bound to into a quiescent
state so the dispatcher can safely tear down or swap. The sequence:

1. **Hook** — dispatch `NX_HOOK_COMPONENT_PAUSE` with `{from:
   ACTIVE, to: PAUSED}`. `ABORT` → return `NX_EABORT`, no state
   change.
2. **Cutoff** — for every slot bound to `c`, set
   `pause_state = NX_SLOT_PAUSE_CUTTING`. The IPC router now
   applies the edge's `NX_PAUSE_QUEUE` / `REJECT` / `REDIRECT`
   policy to new sends.
3. **Drain** — for every bound slot, set
   `pause_state = NX_SLOT_PAUSE_DRAINING` and call
   `nx_ipc_dispatch(slot, SIZE_MAX)` to flush the inbox
   synchronously. Handlers run to completion on the caller's
   thread. Drain reentry (a handler calling `nx_ipc_send` to a
   *different* slot) is fine — only the receiving slot's pause
   flag matters.
4. **`ops->pause_hook(self)`** — if non-NULL. This is where a
   component quiesces its own spawned threads. A non-zero return
   aborts the verb.
5. **`ops->pause(self)`** — if non-NULL. Finishes whatever
   in-flight work is left on the dispatcher's side.
6. **DONE** — for every bound slot, set
   `pause_state = NX_SLOT_PAUSE_DONE`.
7. **State transition** — `nx_component_state_set(c, PAUSED)`.

`nx_component_resume` runs the mirror sequence:

1. Hook dispatch on `NX_HOOK_COMPONENT_RESUME`.
2. `ops->resume(self)` if non-NULL.
3. For every bound slot: set `pause_state = NX_SLOT_PAUSE_NONE`,
   then `nx_ipc_flush_hold_queue(slot)` — held messages are
   replayed back through the router so they observe hooks,
   cap-scan, and policy normally.
4. State transition `PAUSED → ACTIVE`.

**Unbound-component case.** A registered component with zero bound
slots still transitions: the bound-slot walks are no-ops, but
`pause_hook` / `pause` (and `resume`) still fire and the state
machine still advances. This keeps recomposition progressing when
a component is being prepared off to the side.

**Host-side caveat.** On the host build, the 1 ms deadline DESIGN
calls for on `pause_hook` is not enforced — the host has no timer
and the protocol trusts the hook's return. The kernel boot path
(slice 3.9) enforces the deadline via the dispatcher thread's
monotonic clock.

---

## `struct nx_component_ops`

```c
struct nx_component_ops {
    int  (*init)      (void *self);
    int  (*enable)    (void *self);

    int  (*pause_hook)(void *self);   /* slice 3.8 — new */
    int  (*pause)     (void *self);
    int  (*resume)    (void *self);
    int  (*disable)   (void *self);
    void (*destroy)   (void *self);

    int  (*handle_msg)(void *self, struct nx_ipc_message *msg);
};
```

**Every callback receives `self = component->impl`.** This is the
component's private state pointer. All ops are optional — a NULL
slot is a no-op as far as the framework is concerned.

**Which ops are called today (slice 3.8):**

| Op            | Called from                                                             |
|---------------|-------------------------------------------------------------------------|
| `init`        | Not yet — slice 3.9's boot walker will invoke it during bring-up.       |
| `enable`      | Not yet — slice 3.9.                                                    |
| `pause_hook`  | `nx_component_pause`, between cutoff/drain and `ops->pause`.            |
| `pause`       | `nx_component_pause`, after `pause_hook`.                               |
| `resume`      | `nx_component_resume`, before clearing slot pause state.                |
| `disable`     | Not yet — slice 3.9.                                                    |
| `destroy`     | Not yet — slice 3.9.                                                    |
| `handle_msg`  | `nx_ipc_dispatch` and the sync `nx_ipc_send` shortcut.                  |

**`pause_hook` semantics.** Called during the pause protocol between
cutoff/drain and `ops->pause`, matching Session 3's component-spawned-threads
rule. A component that spawns its own threads (manifest
`spawns_threads: true`) MUST implement this hook to quiesce those
threads; dispatcher-only components leave it NULL. The manifest
validator (`tools/validate-config.py`) treats `spawns_threads: true`
without `pause_hook: true` as a build-time config error.

**Return-code contract.** `NX_OK` (0) means "success, continue."
Any non-zero return from `pause_hook` or `pause` aborts the verb
and becomes the verb's return value. Hook-layer ABORT is a separate
channel (the `NX_HOOK_COMPONENT_*` point), not a callback return.

---

## Dependency injection

### Descriptors

```c
struct nx_dep_descriptor {
    const char           *name;         /* slot name to look up */
    size_t                offset;       /* from state root, not deps field */
    bool                  required;     /* missing required dep → NX_ENOENT */
    const char           *version_req;  /* e.g. ">=0.1.0" (unused in 3.4) */
    enum nx_conn_mode     mode;
    bool                  stateful;
    enum nx_pause_policy  policy;
};

struct nx_component_descriptor {
    const char                          *name;        /* manifest name */
    size_t                               state_size;  /* sizeof(container) */
    size_t                               deps_offset; /* offsetof(container, deps) */
    const struct nx_dep_descriptor      *deps;
    size_t                               n_deps;
    const struct nx_component_ops       *ops;
};
```

### `NX_COMPONENT_REGISTER`

```c
#define NX_COMPONENT_REGISTER(NAME, CONTAINER, DEPS_FIELD, OPS, DEPS_TABLE) \
    static const struct nx_dep_descriptor NAME##_deps_tbl[] = {             \
        DEPS_TABLE(CONTAINER, DEPS_FIELD)                                   \
    };                                                                      \
    const struct nx_component_descriptor NAME##_descriptor                  \
        __attribute__((section("nx_components"), used)) = { ... };

#define NX_COMPONENT_REGISTER_NO_DEPS(NAME, CONTAINER, DEPS_FIELD, OPS)  \
    /* same shape, deps = NULL, n_deps = 0 */
```

What expansion produces:

1. A file-scope `const struct nx_dep_descriptor NAME##_deps_tbl[]`
   built from the `DEPS_TABLE(CONTAINER, FIELD)` macro (generated by
   `tools/gen-config.py manifest` into `gen/<NAME>_deps.h` — see
   below).
2. A file-scope `const struct nx_component_descriptor
   NAME##_descriptor` placed into the `nx_components` ELF section
   via `__attribute__((section("nx_components"), used))`. GCC / GNU
   ld auto-generate `__start_nx_components` / `__stop_nx_components`
   markers so the boot walker doesn't need a hand-maintained list.

Test code can reference the descriptor by name directly —
`NAME##_descriptor` — without walking the section.

### Component-side layout

```c
/* State struct — deps embedded inside. */
struct sched_rr_state {
    struct sched_rr_deps deps;   /* from gen/sched_rr_deps.h */
    /* ...private state... */
};

/* Manifest-generated DEPS_TABLE — slot-field initialisers. */
#define SCHED_RR_DEPS_TABLE(CONTAINER, FIELD) \
    { .name = "timer", .offset = offsetof(CONTAINER, FIELD.timer), \
      .required = true, .mode = NX_CONN_SYNC, \
      .stateful = false, .policy = NX_PAUSE_QUEUE }

static const struct nx_component_ops sched_rr_ops = {
    .enable     = sched_rr_enable,
    .handle_msg = sched_rr_handle_msg,
    .pause      = sched_rr_pause,
    .resume     = sched_rr_resume,
};

NX_COMPONENT_REGISTER(sched_rr, struct sched_rr_state, deps,
                      &sched_rr_ops, SCHED_RR_DEPS_TABLE);
```

### `nx_resolve_deps`

```c
int nx_resolve_deps(const struct nx_component_descriptor *d,
                    struct nx_slot *self_slot,
                    void *state);
```

For every entry in `d->deps`:

1. `nx_slot_lookup(dep->name)` — find the target slot.
2. Missing + required → return `NX_ENOENT` (caller should reset
   component state before retrying).
3. Missing + optional → leave the field NULL, skip.
4. Found → write the `struct nx_slot *` at
   `(char *)state + dep->offset`, then
   `nx_connection_register(self_slot, target, dep->mode,
   dep->stateful, dep->policy, &err)`.

`self_slot` may be NULL to signal a boot/external entry edge — the
emitted connection carries `from_slot = NULL`. Returns `NX_OK`
/`NX_EINVAL` / `NX_ENOENT` / `NX_ENOMEM` per registry registration
errors.

**Idempotence warning.** If `nx_resolve_deps` partially succeeds
and then fails, already-written dep pointers and already-registered
connections are **not rolled back**. Callers retrying must reset
component state first.

---

## Invariants

1. **Every successful lifecycle transition fires a state event.** If
   it didn't, the registry / change log views would drift.
2. **Destroy is terminal and gated.** Both the lifecycle matrix and
   the bound-slot guard block destroy while the component is
   visible anywhere.
3. **`pause_hook` ordering is normative.** Cutoff → drain →
   `pause_hook` → `ops->pause`. Components must assume this order
   when implementing `pause_hook` (no inbox messages arrive after
   drain; in-flight handlers are done).
4. **Ops are optional individually but not collectively.** A
   component with no `handle_msg` cannot receive messages (sync
   `nx_ipc_send` and async `nx_ipc_dispatch` both return
   `NX_ENOENT` when `handle_msg` is NULL).

---

## Example — minimal component

```c
#include "framework/component.h"
#include "framework/ipc.h"

struct counter_state {
    int count;
};

static int counter_handle(void *self, struct nx_ipc_message *msg) {
    struct counter_state *s = self;
    (void)msg;
    s->count++;
    return NX_OK;
}

static const struct nx_component_ops counter_ops = {
    .handle_msg = counter_handle,
};

NX_COMPONENT_REGISTER_NO_DEPS(counter,
                              struct counter_state, /* deps embedded */,
                              &counter_ops);
```

*(In a real component the `NO_DEPS` variant uses an unused `deps`
field inside the state struct just to keep the macro shape uniform.)*

---

## Example — pause-aware component

```c
static int my_pause_hook(void *self) {
    struct my_state *s = self;
    s->quit_flag = true;
    wait_for_worker_thread(&s->worker);   /* must finish within 1 ms */
    return NX_OK;
}

static int my_pause(void *self) {
    struct my_state *s = self;
    flush_pending_writes(s);              /* finish in-flight handler work */
    return NX_OK;
}

static int my_resume(void *self) {
    struct my_state *s = self;
    s->quit_flag = false;
    respawn_worker_thread(&s->worker);
    return NX_OK;
}

static const struct nx_component_ops my_ops = {
    .handle_msg = my_handle,
    .pause_hook = my_pause_hook,
    .pause      = my_pause,
    .resume     = my_resume,
};
```

The manifest for this component MUST declare both `spawns_threads:
true` and `pause_hook: true`, or `tools/validate-config.py` fails
the build.

---

## See also

- [Registry](framework-registry.md) — slots, events, change log,
  snapshots.
- [IPC router](framework-ipc.md) — what happens when `handle_msg`
  is invoked and how pause-policy routing interacts with the
  protocol above.
- [Hook framework](framework-hooks.md) — `NX_HOOK_COMPONENT_*` and
  `NX_HOOK_IPC_*` hook points the verbs and router dispatch.
