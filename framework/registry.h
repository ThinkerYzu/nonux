#ifndef NX_FRAMEWORK_REGISTRY_H
#define NX_FRAMEWORK_REGISTRY_H

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

/* ---------- Caller-owned framework objects -------------------------------- */

/* A slot names a typed connection point in the composition. */
struct nx_slot {
    const char              *name;        /* unique, e.g. "scheduler" */
    const char              *iface;       /* interface tag, e.g. "scheduler" */
    enum nx_slot_mutability  mutability;
    enum nx_slot_concurrency concurrency;
    struct nx_component     *active;      /* NULL until a component is bound */
};

/* A component is a registered implementation instance. */
struct nx_component {
    const char              *manifest_id; /* e.g. "sched_rr" */
    const char              *instance_id; /* unique within manifest_id */
    enum nx_lifecycle_state  state;
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

/* ---------- Lifecycle of the registry itself ------------------------------ */

/* Releases every internal node and zeroes all global state.  Safe to call
 * unconditionally at the start of every test.  Caller-owned slot/component
 * structs are NOT freed — only the registry's own bookkeeping. */
void nx_graph_reset(void);

#endif /* NX_FRAMEWORK_REGISTRY_H */
