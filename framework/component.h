#ifndef NX_FRAMEWORK_COMPONENT_H
#define NX_FRAMEWORK_COMPONENT_H

#include "framework/registry.h"

#include <stdbool.h>
#include <stddef.h>

/*
 * Component Lifecycle State Machine — Phase 3 slice 3.3.
 *
 * Every component in the registry carries an `nx_lifecycle_state`.  Raw state
 * writes go through `nx_component_state_set()` in the registry (which emits
 * the `NX_EV_COMPONENT_STATE` change event and bumps the graph generation).
 * This header adds the state-machine layer on top: six lifecycle verbs that
 * refuse illegal current states with NX_ESTATE before driving the transition
 * through `state_set()`.
 *
 * Legal edges (v1 — no intermediate PAUSING state, added when the pause
 * protocol arrives in slice 3.6/3.8):
 *
 *     UNINIT ──init──► INIT ──►──┐
 *        │                       ▼
 *        └─────────────────────► READY ──enable──► ACTIVE
 *                                  ▲                  │
 *                                  │                  ▼
 *                    disable ──────┼──────pause──► PAUSED
 *                                  │                  │
 *                                  └──────────────────┘
 *                                          resume: PAUSED → ACTIVE
 *                                          disable: PAUSED → READY
 *                    destroy: READY → DESTROYED (terminal)
 *
 * `nx_component_init()` drives the whole UNINIT → READY transition in one
 * call (v1 components are synchronously initialised).  The separate INIT
 * state is reserved for future async bring-up; `INIT → READY` is legal so
 * the framework can split the transition later without breaking the matrix.
 */

/* Return true if a direct `from → to` transition is legal. */
bool nx_lifecycle_transition_legal(enum nx_lifecycle_state from,
                                   enum nx_lifecycle_state to);

/* Short lowercase name for a lifecycle state ("uninit", "ready", ...).
 * Unknown values return "unknown".  Returned pointer is to static storage. */
const char *nx_lifecycle_state_name(enum nx_lifecycle_state s);

/* ----- Lifecycle verbs -------------------------------------------------- */
/*
 * All six return NX_OK on success, NX_EINVAL on a NULL component,
 * NX_ENOENT if the component is not in the registry, or NX_ESTATE if the
 * component is not in a state that permits the transition.
 *
 * Each transition is driven through `nx_component_state_set()`, so the
 * resulting change event and change-log entry carry the correct
 * (from, to) pair and observe the same ordering guarantees as every other
 * registry mutation.
 */

int nx_component_init   (struct nx_component *c);  /* UNINIT → READY  */
int nx_component_enable (struct nx_component *c);  /* READY  → ACTIVE */
int nx_component_pause  (struct nx_component *c);  /* ACTIVE → PAUSED */
int nx_component_resume (struct nx_component *c);  /* PAUSED → ACTIVE */
int nx_component_disable(struct nx_component *c);  /* ACTIVE|PAUSED → READY */
int nx_component_destroy(struct nx_component *c);  /* READY  → DESTROYED */

/* ======================================================================
 *  Component ops — typed callback table (slice 3.6).
 *
 *  Every callback receives `self` = the component's state pointer
 *  (`struct nx_component.impl`).  All ops are optional — a NULL entry
 *  is a no-op as far as the framework is concerned.
 *
 *  In slice 3.6 only `handle_msg` is exercised (by the IPC router).
 *  The lifecycle callbacks are typed here so slice 3.9 can plumb them
 *  through `nx_component_init / enable / ...` without changing the
 *  descriptor's shape.
 */

struct nx_ipc_message;  /* forward — full def in framework/ipc.h */

struct nx_component_ops {
    int  (*init)   (void *self);
    int  (*enable) (void *self);
    int  (*pause)  (void *self);
    int  (*resume) (void *self);
    int  (*disable)(void *self);
    void (*destroy)(void *self);
    int  (*handle_msg)(void *self, struct nx_ipc_message *msg);
};

/* ======================================================================
 *  Dependency injection — Phase 3 slice 3.4.
 *
 *  A component declares its dependencies in `manifest.json`.  The build
 *  pipeline translates each manifest into `gen/<name>_deps.h`, which
 *  contains:
 *
 *    1. A typed `struct <name>_deps` — one `struct nx_slot *` field per
 *       manifest `requires` / `optional` entry.
 *    2. A `<NAME>_DEPS_TABLE(CONTAINER, FIELD)` macro — a list of
 *       `nx_dep_descriptor` initialisers that use `offsetof()` into the
 *       component's own state struct (so the offsets are type-safe and
 *       compile-time).
 *
 *  The component author embeds the generated deps struct inside their
 *  state struct and issues a single `NX_COMPONENT_REGISTER` expansion:
 *
 *      struct sched_rr_state {
 *          struct sched_rr_deps deps;
 *          // ... private state ...
 *      };
 *      NX_COMPONENT_REGISTER(sched_rr,
 *                            struct sched_rr_state,
 *                            deps,
 *                            &sched_rr_ops,
 *                            SCHED_RR_DEPS_TABLE);
 *
 *  At composition time the framework calls `nx_resolve_deps()`, which
 *  looks up each dep by slot name, writes the slot pointer into the
 *  component's state at the descriptor-supplied offset, and registers a
 *  connection edge in the Component Graph Registry.
 *
 *  For slice 3.4, `ops` is an opaque `const void *`; slice 3.6 (IPC
 *  router) introduces `struct nx_component_ops` and types it properly.
 */

struct nx_dep_descriptor {
    const char           *name;         /* slot name to look up */
    size_t                offset;       /* from container root (not deps field) */
    bool                  required;     /* missing required dep → NX_ENOENT */
    const char           *version_req;  /* NULL or e.g. ">=0.1.0" (unused in 3.4) */
    enum nx_conn_mode     mode;
    bool                  stateful;
    enum nx_pause_policy  policy;
};

struct nx_component_descriptor {
    const char                       *name;         /* manifest name */
    size_t                            state_size;   /* sizeof(container) */
    size_t                            deps_offset;  /* offsetof(container, deps) */
    const struct nx_dep_descriptor   *deps;
    size_t                            n_deps;
    const struct nx_component_ops    *ops;
};

/* Resolve every dep in `d`, writing slot pointers into `state` at the
 * descriptor-supplied offsets, and register a connection edge from
 * `self_slot` to each resolved target.
 *
 * Returns NX_OK on success, NX_EINVAL on NULL args, or NX_ENOENT if a
 * required dep has no registered slot of that name.  On failure the
 * function leaves any pointers written so far alone — callers must
 * reset component state before retrying.
 *
 * `self_slot` may be NULL for boot-time edges (see registry.h) — the
 * emitted connection then carries `from_slot = NULL`, flagging it as
 * an external/boot entry edge.
 */
int nx_resolve_deps(const struct nx_component_descriptor *d,
                    struct nx_slot *self_slot,
                    void *state);

/*
 * NX_COMPONENT_REGISTER expands to a static `nx_component_descriptor`
 * placed in the `nx_components` linker section.  GCC / GNU ld
 * auto-generate `__start_nx_components` and `__stop_nx_components`
 * markers (since the section name is a valid C identifier), so the
 * framework can walk the section at boot without hand-maintaining a
 * table.  Test code can reference the descriptor symbol directly
 * (`<name>_descriptor`) without walking the section.
 *
 * NAME         — manifest name, also used to prefix generated symbols
 * CONTAINER    — component's full state struct type (e.g. struct sched_rr_state)
 * DEPS_FIELD   — field name of the embedded deps struct inside CONTAINER
 * OPS          — const ops pointer (opaque in 3.4)
 * DEPS_TABLE   — generated macro (e.g. SCHED_RR_DEPS_TABLE) expanding to
 *                a comma-separated list of nx_dep_descriptor initialisers
 */
#define NX_COMPONENT_REGISTER(NAME, CONTAINER, DEPS_FIELD, OPS, DEPS_TABLE) \
    static const struct nx_dep_descriptor NAME##_deps_tbl[] = {             \
        DEPS_TABLE(CONTAINER, DEPS_FIELD)                                   \
    };                                                                      \
    const struct nx_component_descriptor NAME##_descriptor                  \
        __attribute__((section("nx_components"), used)) = {                 \
        .name        = #NAME,                                               \
        .state_size  = sizeof(CONTAINER),                                   \
        .deps_offset = offsetof(CONTAINER, DEPS_FIELD),                     \
        .deps        = NAME##_deps_tbl,                                     \
        .n_deps      = sizeof(NAME##_deps_tbl)                              \
                       / sizeof(NAME##_deps_tbl[0]),                        \
        .ops         = (OPS),                                               \
    }

/* Same as NX_COMPONENT_REGISTER but for a component with no dependencies.
 * The empty deps array triggers GCC's `-Wpedantic zero-length array`
 * diagnostic, which is a false positive here — we pick a tiny unused
 * sentinel to keep the macro uniform. */
#define NX_COMPONENT_REGISTER_NO_DEPS(NAME, CONTAINER, DEPS_FIELD, OPS)     \
    const struct nx_component_descriptor NAME##_descriptor                  \
        __attribute__((section("nx_components"), used)) = {                 \
        .name        = #NAME,                                               \
        .state_size  = sizeof(CONTAINER),                                   \
        .deps_offset = offsetof(CONTAINER, DEPS_FIELD),                     \
        .deps        = NULL,                                                \
        .n_deps      = 0,                                                   \
        .ops         = (OPS),                                               \
    }

#endif /* NX_FRAMEWORK_COMPONENT_H */
