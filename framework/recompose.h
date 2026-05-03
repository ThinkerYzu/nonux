#ifndef NX_FRAMEWORK_RECOMPOSE_H
#define NX_FRAMEWORK_RECOMPOSE_H

#include "framework/registry.h"

#include <stdbool.h>

/*
 * Slice 8.2 — runtime recomposition orchestrator.
 *
 * `nx_recompose()` executes a plan atomically in five phases:
 *
 *   PLAN   — validate + compute topological pause order (dependents first)
 *   PAUSE  — cutoff → drain → pause_hook → ops->pause per component
 *   REWIRE — remove old edges, swap components, add new edges
 *   RESUME — enable new / resume paused; flush hold queues
 *   NOTIFY — fire on_dep_swapped on upstream dependents
 *
 * timer_pause()/timer_resume() bracket the PAUSE…RESUME window in the
 * kernel build to close the tick-driven race window during the swap.
 *
 * Affected slot limit: RECOMP_MAX_SLOTS (32).  Plans touching more slots
 * return NX_ENOMEM.
 */

/* Action for a component change entry. */
enum nx_slot_change_action {
    NX_SLOT_REPLACE = 1,    /* swap old active impl for new_comp */
};

/* Action for a connection change entry. */
enum nx_conn_change_action {
    NX_CONN_ADD    = 1,     /* register a new connection */
    NX_CONN_REMOVE = 2,     /* unregister the matching (from, to) edge */
    NX_CONN_REWIRE = 3,     /* retune mode on an existing edge */
};

/*
 * One component swap.  `slot` must be registered and not FROZEN.
 * `new_comp` must be registered and in NX_LC_READY.  `state_lost` — set
 * NX_SWAP_STATE_LOST in the on_dep_swapped notification flags when the
 * incoming component has no migration path from the outgoing state.
 */
struct nx_slot_change {
    struct nx_slot             *slot;
    enum nx_slot_change_action  action;
    struct nx_component        *new_comp;
    bool                        state_lost;
};

/*
 * One connection change.  For ADD: registers a new edge.  For REMOVE:
 * finds and unregisters the (from_slot, to_slot) edge.  For REWIRE:
 * retuning the existing edge's mode field.
 */
struct nx_conn_change {
    struct nx_slot             *from_slot;  /* NULL for boot-entry edges */
    struct nx_slot             *to_slot;
    enum nx_conn_change_action  action;
    enum nx_conn_mode           mode;
    bool                        stateful;
    enum nx_pause_policy        policy;
};

/*
 * Recomposition plan.  Caller owns all storage; the orchestrator does not
 * retain any plan pointers after nx_recompose() returns.
 * `timeout_ms` is reserved for the SMP upgrade — ignored in v1.
 */
struct recomp_plan {
    struct nx_slot_change *changes;
    int                    num_changes;
    struct nx_conn_change *connections;
    int                    num_connections;
    unsigned int           timeout_ms;
};

/*
 * Execute a recomposition plan.
 *
 * Returns NX_OK on success, or:
 *   NX_EINVAL  — NULL plan, NULL slot/comp in an entry, or cycle in subgraph
 *   NX_EPERM   — plan touches a FROZEN slot
 *   NX_ESTATE  — new_comp not in NX_LC_READY
 *   NX_ENOMEM  — > RECOMP_MAX_SLOTS (32) affected slots
 *   other      — pause_hook/ops->pause error; all already-paused components
 *                are resumed (rolled back) before returning
 */
int nx_recompose(const struct recomp_plan *plan);

#endif /* NX_FRAMEWORK_RECOMPOSE_H */
