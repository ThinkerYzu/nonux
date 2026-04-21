# Registry — `framework/registry.{h,c}`

The Component Graph Registry is the single source of truth for "what
is the kernel built out of right now?" Every slot, every component,
and every connection edge in the running composition is registered
here. Instrumentation, hot-swap, the test harness, and AI reviewers
all consume the same graph.

This module owns:

1. **Caller-owned framework objects** — `struct nx_slot`,
   `struct nx_component`, `struct nx_connection`.
2. **Mutation API** — register, unregister, swap, retune.
3. **Lookup and traversal** — by name, by neighbourhood.
4. **Change events** — 9 typed `NX_EV_*` events delivered to
   subscribers synchronously after each mutation.
5. **Bounded change log** — a 256-entry ring buffer of every event,
   readable by `since_gen`.
6. **Mutation-stable snapshots** — refcounted views frozen at a
   generation; JSON serialisable.

---

## Concepts

### Slot

A **slot** names a typed connection point. Example: the "scheduler"
slot has iface `"scheduler"`; any component that implements the
scheduler interface can be bound as its `active` impl. Slots are
identified by their string `name` (globally unique within the
registry).

### Component

A **component** is a registered implementation instance. Identified
by the pair `(manifest_id, instance_id)` so the same manifest can
have multiple instances (e.g. two `mm_buddy` zones). Carries a
`descriptor` (ops table, dep list — see [Component
lifecycle](framework-components.md)) and an `impl` pointer for the
component's private state.

### Connection

A **connection** is a directed edge `(from_slot → to_slot)`. Carries
mode (`async`/`sync`), stateful flag, and pause-protocol policy
(`NX_PAUSE_QUEUE` / `REJECT` / `REDIRECT`). `from_slot == NULL` means
a boot / external entry edge.

### Generation

Every mutation bumps a monotonic `uint64_t`. Event payloads and
snapshots carry the generation at which they were emitted, so
readers can order events and detect whether their view is up to
date.

### Pause state

Slots carry an `_Atomic(enum nx_slot_pause_state)` flag driven by
the pause protocol (`NONE → CUTTING → DRAINING → DONE`). The IPC
router reads it on every send; see [IPC](framework-ipc.md) and
[Component lifecycle](framework-components.md) for the protocol.

### Fallback

Slots carry an optional `fallback` pointer used by the
`NX_PAUSE_REDIRECT` policy: when the slot is paused and the
connection's policy is REDIRECT, the router re-routes to the
fallback. Set via `nx_slot_set_fallback`.

---

## Types

### Enums

```c
enum nx_lifecycle_state {
    NX_LC_UNINIT = 0,
    NX_LC_INIT,
    NX_LC_READY,
    NX_LC_ACTIVE,
    NX_LC_PAUSED,
    NX_LC_DESTROYED,
};

enum nx_conn_mode       { NX_CONN_ASYNC = 0, NX_CONN_SYNC };
enum nx_pause_policy    { NX_PAUSE_QUEUE = 0, NX_PAUSE_REJECT, NX_PAUSE_REDIRECT };
enum nx_slot_mutability { NX_MUT_HOT = 0, NX_MUT_WARM, NX_MUT_FROZEN };
enum nx_slot_concurrency{ NX_CONC_SHARED = 0, NX_CONC_SERIALIZED,
                          NX_CONC_PER_CPU, NX_CONC_DEDICATED };

enum nx_slot_pause_state {
    NX_SLOT_PAUSE_NONE = 0,
    NX_SLOT_PAUSE_CUTTING,       /* rejecting new sends per policy */
    NX_SLOT_PAUSE_DRAINING,      /* inbox being drained */
    NX_SLOT_PAUSE_DONE,          /* pause_hook + ops->pause ran */
};
```

### `struct nx_slot` (caller-owned)

```c
struct nx_slot {
    const char              *name;         /* unique, e.g. "scheduler" */
    const char              *iface;        /* e.g. "scheduler" */
    enum nx_slot_mutability  mutability;
    enum nx_slot_concurrency concurrency;
    struct nx_component     *active;       /* NULL until bound */
    struct nx_slot          *fallback;     /* NULL until set */
    _Atomic(enum nx_slot_pause_state) pause_state;
};
```

Callers provide `name` and `iface`; `active`, `fallback`, and
`pause_state` are written by the framework (the registry explicitly
re-initialises `fallback` and `pause_state` on `nx_slot_register` —
see note below).

### `struct nx_component` (caller-owned)

```c
struct nx_component {
    const char                            *manifest_id; /* "sched_rr" */
    const char                            *instance_id; /* unique within manifest_id */
    enum nx_lifecycle_state                state;
    void                                  *impl;
    const struct nx_component_descriptor  *descriptor;
};
```

`descriptor` and `impl` are caller-owned; the registry does not touch
them except via the published APIs (`nx_component_state_set`, etc.).

### `struct nx_connection`

```c
struct nx_connection {
    struct nx_slot         *from_slot;      /* NULL = boot/external edge */
    struct nx_slot         *to_slot;
    enum nx_conn_mode       mode;
    bool                    stateful;
    enum nx_pause_policy    policy;
    uint64_t                installed_gen;
};
```

Connection storage is allocated by `nx_connection_register` and freed
by `nx_connection_unregister` — unlike slots / components, callers
do not provide the struct.

---

## Mutation API

### Slots

```c
int  nx_slot_register      (struct nx_slot *s);
int  nx_slot_swap          (struct nx_slot *s, struct nx_component *new_impl);
int  nx_slot_unregister    (struct nx_slot *s);
int  nx_slot_set_fallback  (struct nx_slot *s, struct nx_slot *fallback);
int  nx_slot_set_pause_state(struct nx_slot *s, enum nx_slot_pause_state st);
enum nx_slot_pause_state nx_slot_pause_state(const struct nx_slot *s);
```

- `nx_slot_register(s)` — records `s` in the registry. `s->name` and
  `s->iface` must be set; the registry re-initialises `s->fallback = NULL`
  and `s->pause_state = NX_SLOT_PAUSE_NONE` (via `atomic_init` — see
  *atomic init gotcha* below). Emits `NX_EV_SLOT_CREATED`. Returns
  `NX_EINVAL` on NULL / missing name, `NX_EEXIST` on duplicate name.
- `nx_slot_swap(s, new_impl)` — retargets `s->active`. `new_impl` may
  be NULL (unbind). `new_impl` must be registered, or `NX_ENOENT`.
  Emits `NX_EV_SLOT_SWAPPED`.
- `nx_slot_unregister(s)` — refuses with `NX_EBUSY` if `s` is bound
  as endpoint of any connection; caller must drop connections first.
  Emits `NX_EV_SLOT_DESTROYED`.
- `nx_slot_set_fallback(s, fb)` — `fb` may be NULL (clears);
  self-loop (`s == fb`) returns `NX_EINVAL`; unregistered `fb`
  returns `NX_ENOENT`. Does NOT emit an event — fallbacks are wiring
  metadata, not part of the live composition graph.
- `nx_slot_set_pause_state(s, st)` / `nx_slot_pause_state(s)` —
  release-store / acquire-load on the atomic. Used by the pause
  protocol; most code observes pause state via the router's behaviour,
  not directly. Returns `NX_SLOT_PAUSE_NONE` for a NULL or
  unregistered slot on the getter.

### Components

```c
int  nx_component_register  (struct nx_component *c);
int  nx_component_unregister(struct nx_component *c);
int  nx_component_state_set (struct nx_component *c, enum nx_lifecycle_state s);
```

- `nx_component_register(c)` — requires `manifest_id` + `instance_id`.
  Duplicate `(manifest_id, instance_id)` pair returns `NX_EEXIST`.
  Emits `NX_EV_COMPONENT_REGISTERED`.
- `nx_component_unregister(c)` — refuses with `NX_EBUSY` if `c` is
  bound as `active` on any slot. Emits `NX_EV_COMPONENT_UNREGISTERED`.
- `nx_component_state_set(c, s)` — raw state setter; the lifecycle
  state machine in `framework/component.c` is the normal entry point
  (see [Component lifecycle](framework-components.md)). Emits
  `NX_EV_COMPONENT_STATE { from, to }`. Does not enforce the legal
  transition matrix — callers who need that go through
  `nx_component_{init,enable,pause,resume,disable,destroy}`.

### Connections

```c
struct nx_connection *nx_connection_register(struct nx_slot *from,
                                             struct nx_slot *to,
                                             enum nx_conn_mode mode,
                                             bool stateful,
                                             enum nx_pause_policy policy,
                                             int *err);
int nx_connection_retune     (struct nx_connection *c,
                              enum nx_conn_mode mode, bool stateful);
int nx_connection_unregister (struct nx_connection *c);
```

- `nx_connection_register` — returns the new `struct nx_connection *`
  on success, or NULL with `*err` set on failure. `from` may be NULL
  (boot/external edge); `to` is required. Emits
  `NX_EV_CONNECTION_ADDED`. `err` pointer may itself be NULL if
  caller doesn't care about the diagnostic.
- `nx_connection_retune(c, mode, stateful)` — changes routing mode
  (sync ↔ async) and/or stateful flag without dropping the edge.
  Emits `NX_EV_CONNECTION_RETUNED`.
- `nx_connection_unregister(c)` — removes the edge and frees it.
  Emits `NX_EV_CONNECTION_REMOVED`.

### Introspection

```c
struct nx_slot      *nx_slot_lookup     (const char *name);
struct nx_component *nx_component_lookup(const char *manifest_id,
                                         const char *instance_id);

uint64_t nx_graph_generation       (void);
size_t   nx_graph_slot_count       (void);
size_t   nx_graph_component_count  (void);
size_t   nx_graph_connection_count (void);

bool     nx_component_is_bound          (const struct nx_component *c);
void     nx_component_foreach_bound_slot(const struct nx_component *c,
                                         nx_slot_cb cb, void *ctx);
```

- `nx_component_is_bound(c)` — true if `c` is the `active` impl of at
  least one registered slot. Used by the destroy guard and by the
  pause protocol.
- `nx_component_foreach_bound_slot(c, cb, ctx)` — invokes `cb(slot,
  ctx)` for every slot whose `active == c`. The pause protocol walks
  these to drive per-slot state.

### Traversal

```c
typedef void (*nx_slot_cb)      (struct nx_slot *,       void *ctx);
typedef void (*nx_component_cb) (struct nx_component *,  void *ctx);
typedef void (*nx_connection_cb)(struct nx_connection *, void *ctx);

void nx_graph_foreach_slot      (nx_slot_cb cb,       void *ctx);
void nx_graph_foreach_component (nx_component_cb cb,  void *ctx);
void nx_graph_foreach_connection(nx_connection_cb cb, void *ctx);

/* Per-slot neighbourhoods (O(deg), not O(all-connections)). */
void nx_slot_foreach_dependent  (struct nx_slot *s, nx_connection_cb cb, void *ctx);
void nx_slot_foreach_dependency (struct nx_slot *s, nx_connection_cb cb, void *ctx);
```

- `dependent` = edges where `s` is the `to_slot` (who depends on `s`).
- `dependency` = edges where `s` is the `from_slot` (what `s`
  depends on).

Order within a foreach is not guaranteed (currently linked-list
insertion order, reversed) — do not rely on it.

---

## Events and subscribers

```c
enum nx_graph_event_type {
    NX_EV_SLOT_CREATED = 1,
    NX_EV_SLOT_DESTROYED,
    NX_EV_SLOT_SWAPPED,
    NX_EV_COMPONENT_REGISTERED,
    NX_EV_COMPONENT_UNREGISTERED,
    NX_EV_COMPONENT_STATE,
    NX_EV_CONNECTION_ADDED,
    NX_EV_CONNECTION_REMOVED,
    NX_EV_CONNECTION_RETUNED,
};

struct nx_graph_event {
    enum nx_graph_event_type type;
    uint64_t                 generation;
    uint64_t                 timestamp_ns;
    union {
        struct { const char *name; } slot;
        struct { const char *slot_name;
                 const char *old_manifest, *old_instance;
                 const char *new_manifest, *new_instance; } swap;
        struct { const char *manifest_id, *instance_id; } comp;
        struct { const char             *manifest_id, *instance_id;
                 enum nx_lifecycle_state from, to; } state;
        struct { const char       *from_slot, *to_slot;
                 enum nx_conn_mode mode;
                 bool              stateful; } conn;
    } u;
};

typedef void (*nx_graph_event_cb)(const struct nx_graph_event *ev, void *ctx);

int  nx_graph_subscribe  (nx_graph_event_cb cb, void *ctx);
void nx_graph_unsubscribe(nx_graph_event_cb cb, void *ctx);
```

- Subscribers are stored in a fixed-capacity array (`NX_MAX_SUBSCRIBERS`).
  `nx_graph_subscribe` deduplicates on `(cb, ctx)` — duplicate
  registration returns `NX_EEXIST`. Capacity overflow returns
  `NX_ENOMEM`.
- Events are delivered **synchronously** from inside the mutation
  path, after the generation bump, before the mutation API returns.
  Subscribers therefore observe the new state.
- Event strings (`name`, `manifest_id`, etc.) point at caller-owned
  static storage on the original `nx_slot` / `nx_component` — do
  **not** retain them past the underlying struct's lifetime.

### Callback discipline

Subscriber callbacks must:

- Be **short** — they run inside a lock / dispatcher hot path.
- Be **non-blocking** — no waits, no IPC sends (IPC sends can emit
  events themselves; re-entry is undefined today).
- Treat strings as **borrowed** — copy anything that needs to
  outlive the call.

Intended consumers are instrumentation recorders, the recomposer's
invariant checker, and test watchers.

---

## Change log (registrator)

A bounded append-only ring buffer of every event ever emitted.
Capacity fixed at `NX_CHANGE_LOG_CAPACITY = 256`. Writes are atomic
against concurrent readers via C11 atomics on the head / total
counters. On overflow, the oldest entries are silently overwritten.

```c
uint64_t nx_change_log_total(void);             /* monotonic — may exceed capacity */
size_t   nx_change_log_size (void);             /* currently-readable count */
size_t   nx_change_log_read (uint64_t since_gen,
                             struct nx_graph_event *out, size_t max);
void     nx_change_log_reset(void);             /* test-only */
```

- `nx_change_log_read(since_gen, out, max)` — copies up to `max`
  events with `generation > since_gen`, oldest first. Returns the
  number copied. If `since_gen` is older than the oldest retained
  entry, reads start at the oldest retained entry.
- The change log answers questions the live graph can't: "what was
  the boot composition?", "when did this slot last swap?", "in what
  order did the recomposition transaction apply?".
- `nx_change_log_reset()` is test-only; production code never
  truncates the log.

---

## Snapshots

A snapshot captures the registry's contents at a single generation.
It owns its own copies of the entity arrays, so subsequent mutation
doesn't affect a held snapshot. Refcounted: `take` returns one with
refcount 1; each caller that wants to keep a reference calls
`retain`; every holder calls `put` exactly once.

```c
struct nx_graph_snapshot;                       /* opaque */

struct nx_graph_snapshot *nx_graph_snapshot_take   (void);
void                      nx_graph_snapshot_retain (struct nx_graph_snapshot *s);
void                      nx_graph_snapshot_put    (struct nx_graph_snapshot *s);

uint64_t nx_graph_snapshot_generation     (const struct nx_graph_snapshot *s);
uint64_t nx_graph_snapshot_timestamp_ns   (const struct nx_graph_snapshot *s);
size_t   nx_graph_snapshot_slot_count     (const struct nx_graph_snapshot *s);
size_t   nx_graph_snapshot_component_count(const struct nx_graph_snapshot *s);
size_t   nx_graph_snapshot_connection_count(const struct nx_graph_snapshot *s);

struct nx_snapshot_slot       { const char *name, *iface;
                                enum nx_slot_mutability  mutability;
                                enum nx_slot_concurrency concurrency;
                                const char *active_manifest;   /* NULL = unbound */
                                const char *active_instance; };
struct nx_snapshot_component  { const char *manifest_id, *instance_id;
                                enum nx_lifecycle_state state; };
struct nx_snapshot_connection { const char *from_slot;   /* NULL = boot edge */
                                const char *to_slot;
                                enum nx_conn_mode    mode;
                                bool                 stateful;
                                enum nx_pause_policy policy;
                                uint64_t             installed_gen; };

const struct nx_snapshot_slot       *nx_graph_snapshot_slot      (const struct nx_graph_snapshot *, size_t);
const struct nx_snapshot_component  *nx_graph_snapshot_component (const struct nx_graph_snapshot *, size_t);
const struct nx_snapshot_connection *nx_graph_snapshot_connection(const struct nx_graph_snapshot *, size_t);
```

`nx_graph_snapshot_put(NULL)` is a no-op.

String fields inside `nx_snapshot_*` point at caller-owned storage
on the original structs, exactly like event strings — valid as long
as the underlying structs are.

---

## JSON serialisation

Both the snapshot and the change log can be dumped as JSON for boot
logs, diagnostics, and AI reviewers.

```c
int nx_graph_snapshot_to_json(const struct nx_graph_snapshot *s,
                              char *buf, size_t buflen);
int nx_change_log_to_json    (char *buf, size_t buflen);
```

- Returns the number of bytes written **excluding** the terminating
  NUL, or a negative `NX_E*` on error.
- Truncation: if the buffer is too small, returns `NX_ENOMEM` after
  writing as much as fits (still NUL-terminated at the last valid
  point). Callers can grow the buffer and retry.
- Output is deterministic for the same snapshot — keys are emitted
  in a fixed order.
- Escapes `"`, `\`, `\n`, `\r`, `\t`, control characters via
  `\u00xx`. Escapes only what JSON requires; identifiers are assumed
  to be ASCII-safe (manifest validation enforces this upstream).

**Shape:**

```json
{
  "generation": 42,
  "timestamp_ns": 12345678,
  "slots":       [ {"name": "scheduler", "iface": "scheduler",
                    "active": {"manifest": "sched_rr", "instance": "0"}} ],
  "components":  [ {"manifest": "sched_rr", "instance": "0", "state": "active"} ],
  "connections": [ {"from": null, "to": "scheduler",
                    "mode": "sync", "stateful": false,
                    "policy": "queue", "installed_gen": 3} ]
}
```

---

## Lifecycle of the registry itself

```c
void nx_graph_reset(void);
```

Releases every internal bookkeeping node and zeroes all global state,
including the change log, subscriber list, and generation counter.
**Caller-owned slot and component structs are NOT freed** — only the
registry's own bookkeeping. Test-only on the kernel side; host tests
call it at the top of every case.

---

## Invariants

1. **Slot identity by name.** Two slots with the same `name` are a
   duplicate-registration error, even if they differ in iface or
   mutability.
2. **Component identity by pair.** Two components with the same
   `(manifest_id, instance_id)` are duplicates. Same
   `manifest_id` with different `instance_id` is fine.
3. **Bound components cannot be destroyed / unregistered.** Both
   `nx_component_unregister` and the destroy verb in
   `framework/component.c` refuse with `NX_EBUSY` while
   `nx_component_is_bound(c)` is true.
4. **Connection endpoints pin their slots.** Trying to unregister a
   slot that is still an endpoint of any live connection returns
   `NX_EBUSY`.
5. **Event strings are borrowed.** Subscribers must not retain them
   past the underlying nx_slot / nx_component lifetime.
6. **The change log is monotonic.** `nx_change_log_total()` only
   grows, even across overflow.
7. **Pause state and fallback are slot-side.** The IPC router holds
   slot pointers; the component can swap mid-pause, so these must
   survive component swap.

---

## Atomic-init gotcha

`struct nx_slot::pause_state` is `_Atomic(enum nx_slot_pause_state)`.
The C11 standard does not fully define what happens when a struct
containing an `_Atomic` member is zero-initialised via compound
literal or `memset`. Every host compiler we test on happens to do
the right thing, but the standard-conformant answer is to use
`atomic_init`.

`nx_slot_register` calls `atomic_init(&s->pause_state,
NX_SLOT_PAUSE_NONE)` and sets `s->fallback = NULL` on every
registration, so the registry's published view of these fields is
always well-defined regardless of how the caller zero-initialised.

If you reuse a `struct nx_slot` across register/unregister cycles,
each re-registration re-initialises these fields. Don't rely on the
caller's own initialisation sticking past a `nx_graph_reset()` + new
`nx_slot_register`.

---

## Example — registering a slot and listening for mutations

```c
#include "framework/registry.h"

struct nx_slot scheduler_slot = {
    .name = "scheduler", .iface = "scheduler",
    .mutability = NX_MUT_HOT, .concurrency = NX_CONC_SHARED,
};

static void trace(const struct nx_graph_event *ev, void *ctx) {
    (void)ctx;
    switch (ev->type) {
    case NX_EV_SLOT_CREATED:
        printf("[graph] slot %s created at gen %" PRIu64 "\n",
               ev->u.slot.name, ev->generation);
        break;
    case NX_EV_SLOT_SWAPPED:
        printf("[graph] %s: %s/%s -> %s/%s\n",
               ev->u.swap.slot_name,
               ev->u.swap.old_manifest ?: "-",
               ev->u.swap.old_instance ?: "-",
               ev->u.swap.new_manifest ?: "-",
               ev->u.swap.new_instance ?: "-");
        break;
    default:
        break;
    }
}

void boot_init(void) {
    nx_graph_subscribe(trace, NULL);
    nx_slot_register(&scheduler_slot);
    /* ...bind a component later via nx_slot_swap... */
}
```

---

## Example — dumping the live composition

```c
char buf[4096];
struct nx_graph_snapshot *s = nx_graph_snapshot_take();
int n = nx_graph_snapshot_to_json(s, buf, sizeof buf);
if (n >= 0) fputs(buf, stdout);
nx_graph_snapshot_put(s);
```

The `nx_ipc_*` router, the IPC-router's pause policies, and the
hook framework all build on these primitives — see the sibling docs
for how the composition actually drives message flow.
