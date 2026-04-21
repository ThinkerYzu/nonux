# Hook framework — `framework/hook.{h,c}`

A lightweight chain-of-responsibility for observation and filtering
at every major framework boundary. Hooks are registered dynamically
and dispatched through per-hook-point chains; each hook returns
`CONTINUE` (keep dispatching) or `ABORT` (stop the chain and tell
the caller to bail out).

Intended uses: tracing, metrics, policy enforcement, fault
injection, runtime invariant checks. Scale is observability /
filtering, not data-path fan-out.

This module owns:

1. **Hook-point enum** — one per framework event the router /
   lifecycle fire.
2. **Per-point chains** — priority-sorted, singly-linked.
3. **Dispatch** — walks the chain, short-circuits on ABORT.
4. **Mark-then-sweep unregister** — safe to unregister any hook
   (including the one currently running) from inside a callback.

---

## Hook points

```c
enum nx_hook_point {
    NX_HOOK_IPC_SEND = 0,           /* pre-route, before edge lookup */
    NX_HOOK_IPC_RECV,               /* pre-handler, inside nx_ipc_dispatch */
    NX_HOOK_COMPONENT_ENABLE,       /* around nx_component_enable */
    NX_HOOK_COMPONENT_DISABLE,      /* around nx_component_disable */
    NX_HOOK_COMPONENT_PAUSE,        /* around nx_component_pause */
    NX_HOOK_COMPONENT_RESUME,       /* around nx_component_resume */
    NX_HOOK_SLOT_SWAPPED,           /* registry: active impl changed */
    NX_HOOK_POINT_COUNT,            /* sentinel — keep last */
};
```

**When each fires (slice 3.8):**

| Point                         | Call site                                        | Before / after             |
|-------------------------------|--------------------------------------------------|----------------------------|
| `NX_HOOK_IPC_SEND`            | `nx_ipc_send` → `do_send`                        | Before edge lookup         |
| `NX_HOOK_IPC_RECV`            | `nx_ipc_dispatch`                                | Before the per-msg handler |
| `NX_HOOK_COMPONENT_ENABLE`    | `nx_component_enable`                            | Before the state transition |
| `NX_HOOK_COMPONENT_DISABLE`   | `nx_component_disable`                           | Before the state transition |
| `NX_HOOK_COMPONENT_PAUSE`     | `nx_component_pause`                             | Before the pause protocol steps |
| `NX_HOOK_COMPONENT_RESUME`    | `nx_component_resume`                            | Before `ops->resume`       |
| `NX_HOOK_SLOT_SWAPPED`        | Registry's swap path (runtime dispatch in 3.9)   | After the swap applies     |

**Lifecycle hook semantics.** All four lifecycle hooks
(`ENABLE / DISABLE / PAUSE / RESUME`) fire **before** the state
transition and before any `ops->` callback. An `ABORT` return
causes the verb to return `NX_EABORT` with no state change, no
event emission, no `ops->` call. `NX_HOOK_COMPONENT_ENABLE` fires
even though slice 3.9's boot walker hasn't wired `ops->enable`
into the lifecycle verb yet — the hook runs regardless.

**IPC hook semantics.**

- `NX_HOOK_IPC_SEND` fires before edge lookup. `ctx.u.ipc.edge ==
  NULL` intentionally — the hook sees the routing intent, not the
  resolved edge, so filters can ABORT before the router even looks
  up the connection. ABORT → `nx_ipc_send` returns `NX_EABORT`.
- `NX_HOOK_IPC_RECV` fires per message during inbox drain, before
  cap-scan and handler invocation. ABORT drops the message and
  continues the drain (the message counts as dispatched) — filters
  can use this to suppress noise without stalling the queue.

---

## Types

### `enum nx_hook_action`

```c
enum nx_hook_action {
    NX_HOOK_CONTINUE = 0,
    NX_HOOK_ABORT,
};
```

### `struct nx_hook_context`

Typed union per hook point. The caller populates exactly one arm —
the one matching `point`. A single `void *data` pointer would force
every hook author to blind-cast; the union gives authors a typed
view with no runtime cost.

```c
struct nx_hook_context {
    enum nx_hook_point point;
    union {
        /* IPC_SEND / IPC_RECV */
        struct {
            struct nx_slot         *src;       /* may be NULL on RECV */
            struct nx_slot         *dst;
            struct nx_ipc_message  *msg;
            struct nx_connection   *edge;      /* NULL on SEND (pre-route) */
        } ipc;

        /* COMPONENT_ENABLE / _DISABLE / _PAUSE / _RESUME */
        struct {
            struct nx_component     *comp;
            enum nx_lifecycle_state  from;
            enum nx_lifecycle_state  to;
        } comp;

        /* SLOT_SWAPPED — old or new impl may be NULL (bind / unbind). */
        struct {
            struct nx_slot      *slot;
            struct nx_component *old_impl;
            struct nx_component *new_impl;
        } swap;
    } u;
};
```

### `struct nx_hook`

```c
typedef enum nx_hook_action (*nx_hook_fn)(struct nx_hook_context *ctx,
                                          void                   *user);

struct nx_hook {
    enum nx_hook_point point;
    int                priority;    /* lower runs first */
    nx_hook_fn         fn;
    void              *user;
    const char        *name;        /* diagnostic; may be NULL */
    /* framework-owned */
    struct nx_hook    *_next;
    bool               _dead;
};
```

**Caller-owns the struct.** Register-time storage must outlive
the registration; typical patterns are static globals in the
component's source, or embedding `struct nx_hook` inside the
component's state struct.

`_next` and `_dead` are framework-owned — don't touch them.

---

## API

### Register / unregister

```c
int  nx_hook_register  (struct nx_hook *h);
void nx_hook_unregister(struct nx_hook *h);
```

**`nx_hook_register(h)`:**

- Inserts `h` into the chain for `h->point`, ordered by
  `priority` ascending. Equal priorities run in registration order
  (stable).
- Complexity: O(chain-length). Observability scale is fine;
  data-path-fanout use-cases should pre-register.

| Return       | Meaning                                                    |
|--------------|------------------------------------------------------------|
| `NX_OK`      | Inserted.                                                  |
| `NX_EINVAL`  | NULL `h`, NULL `h->fn`, or `h->point >= NX_HOOK_POINT_COUNT`. |
| `NX_EEXIST`  | Same `h` pointer already registered on this point.        |

**`nx_hook_unregister(h)`:**

- No-op if `h` is NULL or not currently registered.
- Safe to call from inside `h`'s own callback or another hook's
  callback — see "Unregister-during-dispatch" below.

### Dispatch

```c
enum nx_hook_action nx_hook_dispatch(struct nx_hook_context *ctx);
```

- Walks the chain for `ctx->point` in priority order, invoking each
  live hook.
- If any hook returns `NX_HOOK_ABORT`, dispatch stops and returns
  `NX_HOOK_ABORT`.
- If the chain is empty, or the context's point is out of range, or
  `ctx` is NULL, returns `NX_HOOK_CONTINUE`.
- Reentrant: a hook callback may invoke other framework APIs that
  themselves dispatch hooks. Unregister-during-dispatch is also
  safe (see below).

### Introspection / reset

```c
size_t nx_hook_chain_length(enum nx_hook_point point);
void   nx_hook_reset       (void);
```

- `nx_hook_chain_length(point)` — number of currently-live hooks
  (excludes hooks marked dead but not yet swept). For tests.
- `nx_hook_reset()` — tears down every chain and clears the
  in-dispatch counter. Test-only; real kernels never reset.

---

## Unregister-during-dispatch

The framework maintains a global `g_dispatching` counter that
increments on entry to `nx_hook_dispatch` and decrements on return.
Unregister consults it:

- **`g_dispatching == 0`** — unlink `h` from the chain immediately.
- **`g_dispatching > 0`** — mark `h->_dead = true`, defer the
  unlink. Dispatch skips dead hooks during the walk. When the
  outermost dispatch returns (counter drops back to 0), a sweep
  runs across every chain and unlinks every dead node.

This means:

- A hook can unregister itself mid-callback without corrupting the
  iteration — it simply marks-dead and the sweep cleans up.
- A hook can unregister its peer (another hook on any point)
  mid-callback. If the peer was in the same chain and earlier in
  the walk, it had already run; if later, it's skipped because
  `_dead == true`.
- Re-entrant dispatch (a hook calls a framework API that itself
  dispatches hooks) is safe — the counter bumps on every entry, so
  the sweep only runs when the outermost one returns.
- Newly-registered hooks are **not** seen by an in-flight dispatch.
  Dispatch snapshots `h->_next` before invoking each callback, so
  an insert during the walk lands in the chain but isn't visited
  until the next dispatch.

---

## Interaction with other modules

### IPC router

`framework/ipc.c`:

- `do_send` dispatches `NX_HOOK_IPC_SEND` as the first real step,
  with `ctx.u.ipc = { src, dst, msg, edge = NULL }`. ABORT →
  `nx_ipc_send` returns `NX_EABORT`.
- `nx_ipc_dispatch` dispatches `NX_HOOK_IPC_RECV` per message with
  `ctx.u.ipc = { src = msg->src_slot, dst = slot, msg, edge =
  find_edge(src, dst) }`. ABORT drops the message, still counts
  as dispatched.

### Component lifecycle

`framework/component.c`:

- `nx_component_enable` dispatches `NX_HOOK_COMPONENT_ENABLE` with
  `{ comp = c, from = READY, to = ACTIVE }` before the transition.
- Same shape for `disable` (`ACTIVE | PAUSED → READY`), `pause`
  (`ACTIVE → PAUSED`), `resume` (`PAUSED → ACTIVE`).
- ABORT → verb returns `NX_EABORT`.

### Registry

The registry's swap path emits `NX_EV_SLOT_SWAPPED` as a change
event. Runtime dispatch of `NX_HOOK_SLOT_SWAPPED` (i.e. translating
the event into a hook dispatch) lands with slice 3.9 — today the
hook point exists but isn't fired from the framework, only from
manual test dispatches.

---

## Dispatcher-context discipline

Hooks fire on the same thread as the call site they're observing
— currently the framework's dispatcher context. A hook that reads
`slot->active`, calls `nx_slot_resolve`, or otherwise dereferences
a slot is therefore subject to the same slot-resolve-locality rule
that governs `handle_msg`: no stashing resolved `impl *` pointers
past the hook's return.

R8 in the project's AI-verification rubric covers hook handlers
the same way it covers message handlers.

---

## Invariants

1. **Caller owns `struct nx_hook` storage.** Must outlive the
   registration.
2. **`_next` / `_dead` are framework-only.** Callers must not
   touch them.
3. **Duplicate-pointer guard.** The same `struct nx_hook *` can't
   be in the same chain twice; registering again returns
   `NX_EEXIST`.
4. **Dispatch is reentrant.** A hook's callback may invoke another
   framework API that dispatches hooks. The counter + sweep pattern
   keeps this correct.
5. **Newly-registered hooks are invisible to an in-flight dispatch.**
   They're visited starting from the next dispatch.
6. **ABORT is an advisory verb, not an assertion.** A hook that
   ABORTs cancels the current operation; it does not roll back
   earlier hooks' side effects.

---

## Example — count every IPC send

```c
#include "framework/hook.h"

static int send_count;

static enum nx_hook_action count_send(struct nx_hook_context *ctx, void *user)
{
    (void)ctx; (void)user;
    send_count++;
    return NX_HOOK_CONTINUE;
}

static struct nx_hook counter = {
    .point = NX_HOOK_IPC_SEND,
    .priority = 0,
    .fn = count_send,
    .name = "ipc-send-counter",
};

void init_tracing(void) { nx_hook_register(&counter); }
```

## Example — filter-then-forward on IPC_RECV

```c
static enum nx_hook_action block_heartbeats(struct nx_hook_context *ctx,
                                            void *user)
{
    (void)user;
    if (ctx->u.ipc.msg->msg_type == MSG_HEARTBEAT)
        return NX_HOOK_ABORT;    /* drop this message; drain continues */
    return NX_HOOK_CONTINUE;
}

static struct nx_hook hb_filter = {
    .point = NX_HOOK_IPC_RECV,
    .priority = 10,       /* runs after lower-priority tracers */
    .fn = block_heartbeats,
    .name = "drop-heartbeats",
};
```

## Example — abort-on-policy-violation at enable

```c
static enum nx_hook_action enforce_policy(struct nx_hook_context *ctx,
                                          void *user)
{
    const struct policy *p = user;
    if (!policy_allows(p, ctx->u.comp.comp))
        return NX_HOOK_ABORT;      /* nx_component_enable returns NX_EABORT */
    return NX_HOOK_CONTINUE;
}

static struct nx_hook enforcer = {
    .point = NX_HOOK_COMPONENT_ENABLE,
    .priority = -100,            /* run before observers */
    .fn = enforce_policy,
    .user = &boot_policy,
};
```

## Example — self-unregistering one-shot

```c
static enum nx_hook_action first_pause(struct nx_hook_context *ctx, void *user)
{
    (void)ctx;
    struct nx_hook *self = user;
    log_event("first pause observed");
    nx_hook_unregister(self);   /* safe: mark-then-sweep */
    return NX_HOOK_CONTINUE;
}

static struct nx_hook one_shot;    /* ...initialised at register time... */

void arm_first_pause_trace(void)
{
    one_shot = (struct nx_hook){
        .point = NX_HOOK_COMPONENT_PAUSE,
        .fn = first_pause,
        .user = &one_shot,
        .name = "first-pause",
    };
    nx_hook_register(&one_shot);
}
```

---

## See also

- [Registry](framework-registry.md) — the change events that
  `NX_HOOK_SLOT_SWAPPED` parallels.
- [IPC router](framework-ipc.md) — the send / receive call sites
  for `NX_HOOK_IPC_SEND` / `_IPC_RECV`.
- [Component lifecycle](framework-components.md) — the verbs that
  fire `NX_HOOK_COMPONENT_*`.
