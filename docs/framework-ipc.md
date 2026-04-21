# IPC router — `framework/ipc.{h,c}`

Inter-component messaging. Components never call each other
directly; they address a destination slot and the router resolves,
enforces policy, and dispatches. The router reads its routing
tables from the [registry](framework-registry.md) and consults the
[hook framework](framework-hooks.md) at send / receive boundaries.

This module owns:

1. **`nx_ipc_message` + `nx_ipc_cap` types** — the on-wire format.
2. **`nx_ipc_send`** — public send entry point. Sync dispatch or
   async enqueue depending on the edge's mode.
3. **Per-slot inbox (`g_queues`)** — FIFO of async messages per
   destination slot.
4. **Per-`(src, dst)` hold queue (`g_holds`)** — messages parked
   while the destination is paused with `NX_PAUSE_QUEUE` policy.
5. **`nx_ipc_dispatch`** — drain helper. Replaced by a pinned
   per-CPU dispatcher thread in slice 3.9.
6. **Cap scanning** — send-side forged-cap rejection, receive-side
   unclaimed-transfer counting.
7. **`nx_slot_ref_retain` / `_release`** — promote a received
   slot-ref cap into a registered connection edge.

---

## Concepts

### Messages

A message travels from `src_slot` to `dst_slot`. The router uses
the registered connection edge between them to pick sync vs async
and to apply the pause-protocol policy. Message storage is
**caller-owned** — the caller allocates the `struct nx_ipc_message`
(typically on the stack for sync sends, in some pool for async).
The router uses the message's `_next` field to link it into a
queue; it does not `free()` the message.

### Capabilities

Messages can carry an array of `struct nx_ipc_cap`. The supported
cap kind today is `NX_CAP_SLOT_REF` — a typed reference to another
slot. Caps travel in a dedicated array, NOT inside `payload[]`, so
the router can scan them generically.

- **Ownership** is either `BORROW` (the receiver may use the ref
  during the handler but not retain it) or `TRANSFER` (the
  receiver must claim via `nx_slot_ref_retain` or the router flags
  the cap as unclaimed-transfer on recv-scan).
- **Forged caps are rejected on send.** The sender must have a
  registered connection edge to every slot it passes a cap to —
  otherwise `nx_ipc_send` returns `NX_EINVAL`.

### Per-slot inbox

For async routing, messages are queued on the destination slot's
inbox. `nx_ipc_dispatch(slot, max)` drains up to `max` messages,
invoking `handle_msg` on the slot's active component. Host build
dispatches synchronously on the caller's thread; kernel build
(slice 3.9) runs a pinned per-CPU dispatcher thread.

### Per-edge hold queue

When the destination slot is paused and the connection's policy is
`NX_PAUSE_QUEUE`, the router stashes the message into a side-table
keyed by `(src_slot, dst_slot)` instead of the inbox. On resume,
`nx_ipc_flush_hold_queue(dst)` replays every held message back
through `nx_ipc_send`, so they observe hooks, cap-scan, and policy
normally on the way out.

Why a per-edge table rather than a field on `struct nx_slot`: a
single paused slot may have distinct pause semantics for distinct
senders (a sync caller and an async caller can share a destination
but differ on `mode`), so the key has to be `(src, dst)`.

### Slot-resolve locality

Slot dereference — reading `slot->active` or calling through
`slot->active->ops->...` — is only permitted on a framework-owned
dispatcher thread. `nx_ipc_send`'s sync shortcut and
`nx_ipc_dispatch` are the framework's designated dispatcher contexts
in v1; ISRs and arbitrary kernel threads may **enqueue** messages
(slice 3.9 adds a lock-free enqueue entry point for that) but never
dispatch. See the [registry doc](framework-registry.md) for the
rationale.

---

## Types

### `enum nx_cap_kind` / `nx_cap_ownership`

```c
enum nx_cap_kind      { NX_CAP_SLOT_REF = 1, /* + NX_CAP_HANDLE, NX_CAP_VMO_CHUNK later */ };
enum nx_cap_ownership { NX_CAP_BORROW = 0, NX_CAP_TRANSFER };
```

### `struct nx_ipc_cap`

```c
struct nx_ipc_cap {
    enum nx_cap_kind      kind;
    enum nx_cap_ownership ownership;
    union {
        struct nx_slot *slot_ref;
    } u;
    uint32_t              cap_id;     /* stable id within the message */
    bool                  claimed;    /* set by nx_slot_ref_retain */
};
```

### `struct nx_ipc_message`

```c
#define NX_MSG_FLAG_REPLY   (1u << 0)
#define NX_MSG_FLAG_ONEWAY  (1u << 1)

struct nx_ipc_message {
    struct nx_slot        *src_slot;   /* sender; must be registered */
    struct nx_slot        *dst_slot;   /* receiver; active impl dispatches */
    uint32_t               msg_type;
    uint32_t               flags;
    uint32_t               n_caps;
    struct nx_ipc_cap     *caps;       /* may be NULL if n_caps == 0 */
    uint32_t               payload_len;
    const void            *payload;
    struct nx_ipc_message *_next;      /* framework-owned; don't touch */
};
```

- `_next` is used by the router to link messages into inbox / hold
  queue chains. Callers must leave it alone; using the same message
  in two places simultaneously (e.g. re-queuing while it's already
  queued) is undefined.
- `payload` is borrowed — the router passes the pointer unchanged
  to the handler and does not copy. The caller must keep the
  payload alive for the duration of the handler call (sync) or the
  message's queue time (async).

---

## Sending

```c
int nx_ipc_send(struct nx_ipc_message *msg);
```

**Sequence (fast path):**

1. Validate `msg`, `msg->src_slot`, `msg->dst_slot` are non-NULL.
2. Dispatch `NX_HOOK_IPC_SEND` (pre-route, `ctx.edge = NULL`).
   `ABORT` → return `NX_EABORT`.
3. `find_edge(src, dst)` — the registered connection. None → `NX_ENOENT`.
4. `nx_ipc_scan_send_caps` — reject forged caps with `NX_EINVAL`.
5. Read `dst_slot` pause state. `NX_SLOT_PAUSE_NONE` → skip to 7.
6. **Policy branch (slow path):** per the edge's `policy`:
   - `NX_PAUSE_QUEUE` — append to per-`(src, dst)` hold queue, return `NX_OK`.
   - `NX_PAUSE_REJECT` — return `NX_EBUSY`.
   - `NX_PAUSE_REDIRECT` — retarget `msg->dst_slot = dst->fallback`
     and recurse. Depth limit is `NX_IPC_REDIRECT_DEPTH_MAX = 4`;
     exceeding it returns `NX_ELOOP`. `fallback == NULL` returns
     `NX_ENOENT` (fail-closed).
7. If `edge->mode == NX_CONN_SYNC`: invoke `handle_msg(impl, msg)`
   synchronously, run the recv-cap sweep, return the handler's rc.
8. Else (async): enqueue into the destination's inbox, return
   `NX_OK`.

**Return codes:**

| Code         | Meaning                                                        |
|--------------|----------------------------------------------------------------|
| `NX_OK`      | Success (sync: handler returned 0; async: enqueued).           |
| `NX_EINVAL`  | NULL `msg` / missing `src` or `dst` / forged cap.              |
| `NX_ENOENT`  | No edge from `src` to `dst`, or REDIRECT with no fallback, or destination has no active impl / `handle_msg`. |
| `NX_ENOMEM`  | Inbox or hold-queue allocation failed.                         |
| `NX_EBUSY`   | Destination paused with `NX_PAUSE_REJECT` policy.              |
| `NX_ELOOP`   | REDIRECT chain exceeded depth 4.                               |
| `NX_EABORT`  | `NX_HOOK_IPC_SEND` hook returned `NX_HOOK_ABORT`.              |
| _handler rc_ | Sync path: whatever `handle_msg` returned (may be any value).  |

**REDIRECT mutation warning.** On REDIRECT, the router sets
`msg->dst_slot` to the fallback in place and does not restore it
on the way out. This is so that an async enqueue on the recursive
hop correctly uses the fallback slot as the queue key when the
dispatcher runs later. The caller's `msg` therefore reflects the
final destination after `nx_ipc_send` returns — expected behaviour;
document if you care.

---

## Draining (async)

```c
size_t nx_ipc_dispatch(struct nx_slot *slot, size_t max);
size_t nx_ipc_inbox_depth(struct nx_slot *slot);
```

`nx_ipc_dispatch(slot, max)` drains up to `max` messages from
`slot`'s inbox. For each:

1. Dispatch `NX_HOOK_IPC_RECV` (pre-handler, `ctx.edge` looked up
   from `msg->src_slot` to `slot`). `ABORT` drops the message and
   still counts it as dispatched (the queue makes progress).
2. Otherwise invoke `handle_msg(impl, msg)` on the active component.
   Run the recv-cap sweep regardless of handler rc.
3. If handler returned non-zero, stop the drain (don't drain the
   next message either).

Returns the number dispatched (including ABORT-dropped ones).

- `max = 0` returns 0 without touching the inbox.
- If `slot` is NULL or has no inbox, returns 0.
- Empty inbox returns 0 without error.

`nx_ipc_inbox_depth(slot)` — number of messages currently queued on
`slot`'s inbox. For tests / instrumentation.

**Host vs kernel.** Host tests call `nx_ipc_dispatch` explicitly to
drain. The kernel boot path (slice 3.9) hands dispatch ownership to
a pinned per-CPU thread; components never call `nx_ipc_dispatch`
directly in that world. The contract is unchanged.

---

## Pause policy inspection

```c
size_t nx_ipc_hold_queue_depth(struct nx_slot *src, struct nx_slot *dst);
void   nx_ipc_flush_hold_queue(struct nx_slot *dst);
```

- `nx_ipc_hold_queue_depth(src, dst)` — number of messages currently
  held for the `(src, dst)` edge. `src` may be NULL for boot /
  external edges. Zero for non-existent entries.
- `nx_ipc_flush_hold_queue(dst)` — called by the pause-protocol
  resume path after `dst->pause_state` returns to `NONE`. Walks
  `g_holds`, detaches every entry with `dst == dst`, replays each
  message via `nx_ipc_send` so they route normally (hooks +
  pause-policy + cap-scan all run). Safe to call on a slot with no
  held messages (no-op). Replay errors are silently consumed —
  they're observable via `NX_HOOK_IPC_SEND` for callers that need
  feedback.

---

## Reset (test-only)

```c
void nx_ipc_reset(void);
```

Drops every per-slot inbox and every `(src, dst)` hold entry; frees
their internal bookkeeping. Caller-owned message storage is
untouched (the router doesn't own it). Always safe to call at the
top of a test.

Production kernel code never calls this.

---

## Cap scanning

```c
int    nx_ipc_scan_send_caps(struct nx_slot *sender, struct nx_ipc_message *msg);
size_t nx_ipc_scan_recv_caps(struct nx_slot *recv,   struct nx_ipc_message *msg);
```

### Send-side (`scan_send_caps`)

Runs automatically from `nx_ipc_send`. Validates that the sender
legitimately holds every slot-ref cap it passes: there must be a
registered connection edge `sender → cap->u.slot_ref` in the graph
for each `NX_CAP_SLOT_REF` cap. Any missing edge is a forgery and
the function returns `NX_EINVAL` on the first one. Non-SLOT_REF
caps are ignored in this slice.

Pure function — can be called repeatedly; no side effects.

| Return      | Meaning                                         |
|-------------|-------------------------------------------------|
| `NX_OK`     | Every cap is a legit reference the sender holds. |
| `NX_EINVAL` | NULL args; null `slot_ref`; no edge to a passed slot. |

### Receive-side (`scan_recv_caps`)

Runs automatically from `invoke_handler` after the handler returns,
regardless of handler rc. Counts unclaimed `NX_CAP_TRANSFER` caps —
caps the sender promised to transfer ownership of but that the
receiver never claimed via `nx_slot_ref_retain`. `BORROW` caps that
end up unclaimed are expected and silently dropped. Claimed caps
(`cap->claimed == true`) are not counted.

Returns the number of unclaimed TRANSFER caps. Zero means clean
sweep. Non-zero indicates a protocol bug — the sender gave up
ownership but no one took it.

`recv` is currently unused (reserved for future per-component
logging); callers may pass NULL.

---

## `slot_ref_retain` / `_release`

Promote a received slot-ref cap into a registered connection edge
owned by the receiving slot. This is how a component that received
a slot reference as part of a message turns that transient cap into
a durable dependency.

```c
int  nx_slot_ref_retain (struct nx_slot *self,
                         struct nx_ipc_cap *cap,
                         const char *purpose,
                         struct nx_connection **out_conn);
void nx_slot_ref_release(struct nx_slot *self, struct nx_slot *target);
```

**`nx_slot_ref_retain(self, cap, purpose, out_conn)`:**

- Must be called from inside the handler (before it returns).
- Registers a new connection edge `self → cap->u.slot_ref` with
  `NX_CONN_ASYNC`, `stateful = false`, `NX_PAUSE_QUEUE`. (Retain
  doesn't carry mode metadata — the receiver's manifest declares
  how it uses the dep.)
- Marks `cap->claimed = true`. The recv-cap sweep will ignore the
  cap after this.
- `purpose` is a diagnostic tag for logs / change-log annotations;
  may be NULL.
- `out_conn` receives the new connection pointer (may be NULL if
  the caller doesn't need it).

| Return      | Meaning                                                    |
|-------------|------------------------------------------------------------|
| `NX_OK`     | Claimed; new edge registered.                              |
| `NX_EINVAL` | Bad args — NULL `self` / `cap`, wrong kind, already claimed, NULL slot ref. |
| `NX_ENOENT` | `self` or target is not registered.                        |
| `NX_ENOMEM` | Connection registration failed.                            |

**`nx_slot_ref_release(self, target)`:**

Walks `self`'s outgoing connections, finds the edge to `target`,
and `nx_connection_unregister`s it. No-op if the edge is already
gone. Pairs with `retain` for `NX_CAP_TRANSFER` flows; components
typically release in `disable` or `destroy`.

---

## Interaction with the pause protocol

The pause protocol in `framework/component.c` owns the slot
`pause_state` transitions; the router reads them. Summary of who
writes / reads what:

| State               | Written by                                              | Router behaviour                                                       |
|---------------------|---------------------------------------------------------|------------------------------------------------------------------------|
| `NX_SLOT_PAUSE_NONE`      | Initial value; resume path                          | Fast path — no policy branch, direct route.                            |
| `NX_SLOT_PAUSE_CUTTING`   | Pause protocol, step 2                              | Apply edge's `policy` to every new send.                              |
| `NX_SLOT_PAUSE_DRAINING`  | Pause protocol, step 3 (just before inbox drain)    | Still apply `policy` — new sends during drain go to hold / reject / redirect. |
| `NX_SLOT_PAUSE_DONE`      | Pause protocol, step 6                              | Same as CUTTING/DRAINING — policy applies to every send.               |

Once `resume` flips the state back to `NONE`, hold-queue replay
routes messages normally.

---

## Example — synchronous request / reply

```c
struct nx_ipc_message req = {
    .src_slot = &posix_shim_slot,
    .dst_slot = &scheduler_slot,
    .msg_type = SCHED_MSG_YIELD,
};
int rc = nx_ipc_send(&req);
if (rc != NX_OK) log_send_failure(rc);
```

Handler signature (`sched_rr_ops.handle_msg`):

```c
static int sched_rr_handle(void *self, struct nx_ipc_message *msg) {
    struct sched_rr_state *s = self;
    switch (msg->msg_type) {
    case SCHED_MSG_YIELD: return sched_rr_yield(s);
    default:              return NX_ENOENT;
    }
}
```

## Example — async send + explicit drain (host test shape)

```c
struct nx_ipc_message m = {
    .src_slot = &producer, .dst_slot = &consumer, .msg_type = 42,
};
nx_ipc_send(&m);
/* ...more sends... */
size_t drained = nx_ipc_dispatch(&consumer, 100);
```

## Example — capability transfer

```c
/* Sender side */
struct nx_ipc_cap caps[1] = {
    { .kind = NX_CAP_SLOT_REF, .ownership = NX_CAP_TRANSFER,
      .u.slot_ref = &storage_slot },
};
struct nx_ipc_message m = {
    .src_slot = &registrar_slot, .dst_slot = &consumer_slot,
    .n_caps = 1, .caps = caps,
};
nx_ipc_send(&m);

/* Receiver side — inside handler */
static int consumer_handle(void *self, struct nx_ipc_message *msg) {
    struct consumer_state *s = self;
    for (uint32_t i = 0; i < msg->n_caps; i++) {
        if (msg->caps[i].kind != NX_CAP_SLOT_REF) continue;
        struct nx_connection *c = NULL;
        int rc = nx_slot_ref_retain(&consumer_slot, &msg->caps[i],
                                    "storage", &c);
        if (rc == NX_OK) s->storage = msg->caps[i].u.slot_ref;
    }
    return NX_OK;
}
```

Later, when the consumer is torn down:

```c
nx_slot_ref_release(&consumer_slot, s->storage);
```

---

## Invariants

1. **Caller owns message storage.** The router stores pointers into
   queues but never frees messages. Double-use (enqueuing the same
   `nx_ipc_message` twice simultaneously) is undefined.
2. **No forged caps.** Every SLOT_REF cap on a send must correspond
   to a registered connection the sender holds. Sending with a
   forged cap returns `NX_EINVAL`, the handler never runs.
3. **Recv-cap sweep runs regardless of handler rc.** Even a failing
   handler is responsible for BORROW caps being dropped and
   TRANSFER caps being accounted for.
4. **`msg->_next` is framework-only.** Callers must not touch it.
5. **REDIRECT retargets in place.** After a REDIRECT-resolved send,
   `msg->dst_slot` points at the final destination, not the
   original target. Intentional — see the "REDIRECT mutation
   warning" above.
6. **IPC hooks run on the dispatcher.** `NX_HOOK_IPC_SEND` /
   `_IPC_RECV` handlers must obey the slot-resolve locality rule
   if they read `slot->active`.
7. **`NX_PAUSE_REJECT` returns `NX_EBUSY`, never silently drops.**
   Callers that need fire-and-forget on a paused destination should
   declare the edge as `NX_PAUSE_QUEUE` instead.

---

## See also

- [Registry](framework-registry.md) — slot / connection definitions,
  pause-state / fallback fields.
- [Component lifecycle](framework-components.md) — pause protocol
  that drives `pause_state` through `CUTTING → DRAINING → DONE`,
  and the `handle_msg` callback.
- [Hook framework](framework-hooks.md) — IPC_SEND / IPC_RECV hook
  points and ABORT semantics.
