#ifndef NX_FRAMEWORK_REGISTRY_H
#define NX_FRAMEWORK_REGISTRY_H

#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Component Graph Registry — first-class runtime model of the live composition.
 *
 * Every slot, every component, and every connection edge in the running kernel
 * is registered here.  The registry is the single source of truth for "what is
 * the kernel built out of right now?" — instrumentation, hot-swap, the test
 * harness, and AI reviewers all consume it.
 *
 * This first slice is the bookkeeping skeleton: register / lookup / unregister
 * for slots, components, and connections, plus monotonic generation tracking
 * and basic traversal.  Snapshot/JSON serialization, the bounded change log,
 * and event subscription are deferred to the next slice.  Concurrency is also
 * deferred — the host-side build is single-threaded; the kernel build will
 * wrap mutations in the recomposer's serialization.
 *
 * Memory model: callers own `struct slot` and `struct component` storage
 * (static or component-local).  The registry allocates its own internal nodes
 * via plain malloc (host) — wired to a kernel allocator when the framework
 * lands in-kernel.  `graph_reset()` releases everything the registry owns.
 */

/* ---------- Error codes --------------------------------------------------- */

#define NX_OK         0
#define NX_EINVAL    -1
#define NX_ENOMEM    -2
#define NX_EEXIST    -3
#define NX_ENOENT    -4
#define NX_EBUSY     -5    /* operation would leave dangling references */
#define NX_ESTATE    -6    /* lifecycle / state-machine violation */
#define NX_ELOOP     -7    /* redirect loop depth exceeded */
#define NX_EABORT    -8    /* hook chain returned ABORT */

/* ---------- Enums --------------------------------------------------------- */

enum nx_lifecycle_state {
    NX_LC_UNINIT = 0,
    NX_LC_INIT,
    NX_LC_READY,
    NX_LC_ACTIVE,
    NX_LC_PAUSED,
    NX_LC_DESTROYED,
};

enum nx_conn_mode {
    NX_CONN_ASYNC = 0,
    NX_CONN_SYNC,
};

enum nx_pause_policy {
    NX_PAUSE_QUEUE = 0,
    NX_PAUSE_REJECT,
    NX_PAUSE_REDIRECT,
};

enum nx_slot_mutability {
    NX_MUT_HOT = 0,
    NX_MUT_WARM,
    NX_MUT_FROZEN,
};

enum nx_slot_concurrency {
    NX_CONC_SHARED = 0,
    NX_CONC_SERIALIZED,
    NX_CONC_PER_CPU,
    NX_CONC_DEDICATED,
};

/*
 * Pause state lives on the slot rather than the bound component: the IPC
 * router holds slot pointers and the component behind the slot can swap
 * mid-pause, so the flag must be slot-side.
 *
 * Transitions in v1 (single-core host build):
 *
 *     NONE ──cutoff──► CUTTING ──drain──► DRAINING ──quiesce──► DONE
 *       ▲                                                         │
 *       └─────────────────────resume───────────────────────────────┘
 *
 * SMP upgrade: the field is `_Atomic` from day one — barriers are added
 * where readers live (IPC send path, resume flush), no restructure needed.
 */
enum nx_slot_pause_state {
    NX_SLOT_PAUSE_NONE = 0,
    NX_SLOT_PAUSE_CUTTING,          /* rejecting new sends per policy */
    NX_SLOT_PAUSE_DRAINING,         /* inbox being drained */
    NX_SLOT_PAUSE_DONE,             /* component.pause_hook + ops->pause ran */
};

/* ---------- Caller-owned framework objects -------------------------------- */

/* A slot names a typed connection point in the composition. */
struct nx_slot {
    const char              *name;        /* unique, e.g. "scheduler" */
    const char              *iface;       /* interface tag, e.g. "scheduler" */
    enum nx_slot_mutability  mutability;
    enum nx_slot_concurrency concurrency;
    struct nx_component     *active;      /* NULL until a component is bound */

    /* Fallback for NX_PAUSE_REDIRECT policy.  NULL by default; callers wire
     * it up via `nx_slot_set_fallback`.  When set and the slot is paused,
     * a send with REDIRECT policy is re-routed to `fallback`.  The IPC
     * router enforces a redirect depth limit to cut loops (NX_ELOOP). */
    struct nx_slot          *fallback;

    /* Written during the pause protocol; read by the IPC router on every
     * send.  `_Atomic` so the SMP build only needs a barrier swap, not a
     * restructure. */
    _Atomic(enum nx_slot_pause_state) pause_state;
};

/* Forward declaration — full type in framework/component.h.  Kept here
 * as a pointer field so `nx_component` can link back to its descriptor
 * without pulling component.h into every registry consumer. */
struct nx_component_descriptor;

/* A component is a registered implementation instance. */
struct nx_component {
    const char              *manifest_id; /* e.g. "sched_rr" */
    const char              *instance_id; /* unique within manifest_id */
    enum nx_lifecycle_state  state;

    /* Caller-owned (not touched by the registry).  Set by the framework
     * bring-up path in slice 3.9 — the IPC router in 3.6 reads both. */
    void                                   *impl;        /* `self` for ops */
    const struct nx_component_descriptor   *descriptor;  /* ops / state_size */
};

/* A connection is a directed edge from one slot's active impl to another slot.
 * `from_slot` may be NULL for "boot" or "external" entry edges (e.g. the boot
 * sequence wiring its first dependency). */
struct nx_connection {
    struct nx_slot          *from_slot;   /* may be NULL */
    struct nx_slot          *to_slot;     /* required */
    enum nx_conn_mode        mode;
    bool                     stateful;
    enum nx_pause_policy     policy;
    uint64_t                 installed_gen;
};

/* ---------- Mutation API -------------------------------------------------- */

int  nx_slot_register      (struct nx_slot *s);
int  nx_slot_swap          (struct nx_slot *s, struct nx_component *new_impl);
int  nx_slot_unregister    (struct nx_slot *s);

/* Configure the REDIRECT fallback on a registered slot.  `fallback` may
 * be NULL to clear.  Returns NX_ENOENT if `s` is not registered, NX_EINVAL
 * if `s == fallback` (self-loop with depth 1).  Does not emit an event —
 * fallbacks are wiring metadata, not part of the live composition graph. */
int  nx_slot_set_fallback  (struct nx_slot *s, struct nx_slot *fallback);

/* Transition a slot's pause_state.  Used by the pause protocol in
 * `nx_component_pause` / `_resume`.  Returns NX_ENOENT if `s` is not
 * registered.  Does not emit an event — pause transitions are observed
 * via the COMPONENT_PAUSE/RESUME hook points, not the change log. */
int  nx_slot_set_pause_state(struct nx_slot *s, enum nx_slot_pause_state st);

/* Read a slot's current pause_state.  Returns NX_SLOT_PAUSE_NONE for a
 * NULL or unregistered slot. */
enum nx_slot_pause_state nx_slot_pause_state(const struct nx_slot *s);

int  nx_component_register (struct nx_component *c);
int  nx_component_unregister(struct nx_component *c);
int  nx_component_state_set(struct nx_component *c, enum nx_lifecycle_state s);

/* Returns the registered connection on success, NULL on failure (and sets
 * *err to a NX_E* code).  `err` may be NULL. */
struct nx_connection *nx_connection_register(struct nx_slot *from,
                                             struct nx_slot *to,
                                             enum nx_conn_mode mode,
                                             bool stateful,
                                             enum nx_pause_policy policy,
                                             int *err);
int  nx_connection_retune    (struct nx_connection *c,
                              enum nx_conn_mode mode, bool stateful);
int  nx_connection_unregister(struct nx_connection *c);

/* ---------- Lookup & introspection ---------------------------------------- */

struct nx_slot      *nx_slot_lookup     (const char *name);
struct nx_component *nx_component_lookup(const char *manifest_id,
                                         const char *instance_id);

/* Monotonically incremented on every mutation. */
uint64_t nx_graph_generation(void);

size_t   nx_graph_slot_count      (void);
size_t   nx_graph_component_count (void);
size_t   nx_graph_connection_count(void);

/* ---------- Traversal ----------------------------------------------------- */

typedef void (*nx_slot_cb)      (struct nx_slot *,       void *ctx);
typedef void (*nx_component_cb) (struct nx_component *,  void *ctx);
typedef void (*nx_connection_cb)(struct nx_connection *, void *ctx);

void nx_graph_foreach_slot      (nx_slot_cb cb,       void *ctx);
void nx_graph_foreach_component (nx_component_cb cb,  void *ctx);
void nx_graph_foreach_connection(nx_connection_cb cb, void *ctx);

/* Connections where slot `s` is the to_slot (i.e. who depends on `s`). */
void nx_slot_foreach_dependent  (struct nx_slot *s, nx_connection_cb cb, void *ctx);
/* Connections where slot `s` is the from_slot (i.e. what `s` depends on). */
void nx_slot_foreach_dependency (struct nx_slot *s, nx_connection_cb cb, void *ctx);

/* True if `c` is currently bound as the `active` impl of at least one
 * registered slot.  Used by `nx_component_destroy` as the cross-check
 * that a registered-and-bound component can't be silently destroyed.
 * Returns false for NULL. */
bool nx_component_is_bound(const struct nx_component *c);

/* Invoke `cb` for every registered slot whose `active` pointer equals
 * `c`.  The pause protocol walks these to drive slot-side state. */
void nx_component_foreach_bound_slot(const struct nx_component *c,
                                     nx_slot_cb cb, void *ctx);

/* ---------- Change events ------------------------------------------------- */

/*
 * Every mutation of the registry emits a change event.  Subscribers are
 * called synchronously from inside the mutation (after the generation bump,
 * before the mutation API returns to its caller), so they observe the new
 * state.  The event also lands in the bounded change log (see below).
 *
 * Event payloads carry `const char *` names that point to caller-owned
 * static storage on the original `nx_slot` / `nx_component` struct.  In
 * production these will be string literals emitted by gen-config; in tests
 * they're string literals on the stack frame that registered the entity.
 * Subscribers and change-log readers must NOT retain these pointers past
 * the lifetime of the underlying caller storage.
 *
 * Subscriber callbacks must be short and non-blocking.  Intended consumers
 * are instrumentation recorders, the recomposer's invariant checker, and
 * test watchers.
 */

enum nx_graph_event_type {
    NX_EV_SLOT_CREATED = 1,
    NX_EV_SLOT_DESTROYED,
    NX_EV_SLOT_SWAPPED,            /* active impl changed (incl. NULL bind) */
    NX_EV_COMPONENT_REGISTERED,
    NX_EV_COMPONENT_UNREGISTERED,
    NX_EV_COMPONENT_STATE,         /* lifecycle state transition */
    NX_EV_CONNECTION_ADDED,
    NX_EV_CONNECTION_REMOVED,
    NX_EV_CONNECTION_RETUNED,      /* mode or stateful flag changed */
};

struct nx_graph_event {
    enum nx_graph_event_type type;
    uint64_t                 generation;
    uint64_t                 timestamp_ns;
    union {
        /* SLOT_CREATED, SLOT_DESTROYED */
        struct {
            const char *name;
        } slot;

        /* SLOT_SWAPPED — old/new manifest+instance are NULL when the
         * corresponding side is unbound. */
        struct {
            const char *slot_name;
            const char *old_manifest;
            const char *old_instance;
            const char *new_manifest;
            const char *new_instance;
        } swap;

        /* COMPONENT_REGISTERED, COMPONENT_UNREGISTERED */
        struct {
            const char *manifest_id;
            const char *instance_id;
        } comp;

        /* COMPONENT_STATE */
        struct {
            const char             *manifest_id;
            const char             *instance_id;
            enum nx_lifecycle_state from;
            enum nx_lifecycle_state to;
        } state;

        /* CONNECTION_ADDED, CONNECTION_REMOVED, CONNECTION_RETUNED.
         * `from_slot` is NULL for boot/external entry edges. */
        struct {
            const char         *from_slot;
            const char         *to_slot;
            enum nx_conn_mode   mode;
            bool                stateful;
        } conn;
    } u;
};

typedef void (*nx_graph_event_cb)(const struct nx_graph_event *ev, void *ctx);

/* Append a subscriber.  `(cb, ctx)` pairs must be unique — duplicate
 * subscription returns NX_EEXIST.  Capacity is fixed (NX_MAX_SUBSCRIBERS);
 * exceeding it returns NX_ENOMEM.  Returns NX_OK on success. */
int  nx_graph_subscribe  (nx_graph_event_cb cb, void *ctx);

/* Remove the subscriber matching `(cb, ctx)`.  No-op if not subscribed. */
void nx_graph_unsubscribe(nx_graph_event_cb cb, void *ctx);

/* ---------- Change log (registrator) -------------------------------------- */

/*
 * Bounded append-only ring buffer of recent events.  Capacity is fixed at
 * compile time (NX_CHANGE_LOG_CAPACITY).  Writes are atomic against
 * concurrent readers; on overflow, the oldest entries are overwritten.
 *
 * The log answers questions the live graph can't: "what was the boot
 * composition?", "when did this slot last swap?", "in what order did the
 * recomposition transaction apply?".
 */

/* Total events ever appended (monotonic).  May exceed capacity. */
uint64_t nx_change_log_total(void);

/* Number of currently-readable entries (min(total, capacity)). */
size_t   nx_change_log_size(void);

/* Read up to `max` events with `generation > since_gen`, oldest first,
 * into `out`.  Returns the number actually copied.  Stops at the oldest
 * still-retained event if `since_gen` is older than what the ring holds. */
size_t   nx_change_log_read(uint64_t since_gen,
                            struct nx_graph_event *out, size_t max);

/* Reset the log (test-only; registry doesn't normally clear it). */
void     nx_change_log_reset(void);

/* ---------- Snapshot ------------------------------------------------------ */

/*
 * A graph snapshot captures the registry's contents at a single generation.
 * It owns its own copies of the entity arrays — once taken, the snapshot is
 * stable even if the registry mutates underneath.  String fields point to
 * the same caller-owned storage referenced by the live registry; they remain
 * valid as long as the underlying nx_slot / nx_component structs do.
 *
 * Snapshots are reference-counted: `nx_graph_snapshot_take()` returns one
 * with refcount=1; pass it around freely and call `nx_graph_snapshot_put()`
 * exactly once per take.  Reading a snapshot does not block writers.
 */

struct nx_graph_snapshot;       /* opaque */

struct nx_graph_snapshot *nx_graph_snapshot_take  (void);
void                      nx_graph_snapshot_retain(struct nx_graph_snapshot *s);
void                      nx_graph_snapshot_put   (struct nx_graph_snapshot *s);

uint64_t nx_graph_snapshot_generation     (const struct nx_graph_snapshot *s);
uint64_t nx_graph_snapshot_timestamp_ns   (const struct nx_graph_snapshot *s);
size_t   nx_graph_snapshot_slot_count     (const struct nx_graph_snapshot *s);
size_t   nx_graph_snapshot_component_count(const struct nx_graph_snapshot *s);
size_t   nx_graph_snapshot_connection_count(const struct nx_graph_snapshot *s);

/* Per-entity views (frozen copies, NOT pointers into the live registry). */
struct nx_snapshot_slot {
    const char              *name;
    const char              *iface;
    enum nx_slot_mutability  mutability;
    enum nx_slot_concurrency concurrency;
    const char              *active_manifest;   /* NULL if no active impl */
    const char              *active_instance;
};

struct nx_snapshot_component {
    const char             *manifest_id;
    const char             *instance_id;
    enum nx_lifecycle_state state;
};

struct nx_snapshot_connection {
    const char           *from_slot;            /* NULL for boot edge */
    const char           *to_slot;
    enum nx_conn_mode     mode;
    bool                  stateful;
    enum nx_pause_policy  policy;
    uint64_t              installed_gen;
};

const struct nx_snapshot_slot       *nx_graph_snapshot_slot      (const struct nx_graph_snapshot *s, size_t i);
const struct nx_snapshot_component  *nx_graph_snapshot_component (const struct nx_graph_snapshot *s, size_t i);
const struct nx_snapshot_connection *nx_graph_snapshot_connection(const struct nx_graph_snapshot *s, size_t i);

/* ---------- JSON serialization -------------------------------------------- */

/*
 * Both serializers write a NUL-terminated JSON string into `buf`.  Return
 * value: number of bytes written EXCLUDING the terminating NUL, or a
 * negative NX_E* code on error.  If the output is truncated due to buflen,
 * returns NX_ENOMEM and writes as much as fits (still NUL-terminated).
 */
int nx_graph_snapshot_to_json(const struct nx_graph_snapshot *s,
                              char *buf, size_t buflen);

int nx_change_log_to_json(char *buf, size_t buflen);

/* ---------- Lifecycle of the registry itself ------------------------------ */

/* Releases every internal node and zeroes all global state, including the
 * change log and subscriber list.  Safe to call unconditionally at the
 * start of every test.  Caller-owned slot/component structs are NOT
 * freed — only the registry's own bookkeeping. */
void nx_graph_reset(void);

#endif /* NX_FRAMEWORK_REGISTRY_H */
