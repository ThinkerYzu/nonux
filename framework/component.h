#ifndef NX_FRAMEWORK_COMPONENT_H
#define NX_FRAMEWORK_COMPONENT_H

#include "framework/registry.h"

#include <stdbool.h>

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

#endif /* NX_FRAMEWORK_COMPONENT_H */
