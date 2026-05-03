#ifndef NX_FRAMEWORK_CONFIG_H
#define NX_FRAMEWORK_CONFIG_H

#include "framework/handle.h"
#include "framework/registry.h"

#include <stdint.h>

/*
 * Slice 8.3 — runtime config manager.
 *
 * Exposes the live component graph to EL0 via three syscalls:
 *   NX_SYS_CONFIG_OPEN  — obtain a config handle
 *   NX_SYS_CONFIG_QUERY — snapshot the current composition
 *   NX_SYS_CONFIG_SWAP  — swap one slot's active component
 *
 * The config handle is a capability token (NX_HANDLE_CONFIG).  In v1
 * there is no privilege separation — any process that opens a config
 * handle gets full composition access.  The handle still serves as the
 * natural revocation point: closing it terminates config access without
 * changing any kernel state.
 *
 * nx_config_swap_component() builds a single-slot recomp_plan and calls
 * nx_recompose().  The caller names the new component by manifest_id;
 * the config manager finds the first registered NX_LC_READY component
 * with that id.
 */

/* ---------- Snapshot --------------------------------------------------- */

#define NX_CONFIG_SNAPSHOT_MAX_SLOTS  32u

struct nx_config_snapshot_entry {
    char     slot_name[32];  /* slot->name, NUL-terminated */
    char     impl_name[32];  /* active component manifest_id, or "" if unbound */
    uint32_t state;          /* enum nx_lifecycle_state of active component */
};

/*
 * Filled by nx_config_query_snapshot().  Userspace allocates this struct
 * and passes a pointer via NX_SYS_CONFIG_QUERY; the kernel copies the
 * populated struct back with copy_to_user.
 */
struct nx_config_snapshot {
    uint64_t generation;                                    /* nx_graph_generation() */
    uint32_t num_slots;                                     /* entries written */
    uint32_t _pad;
    struct nx_config_snapshot_entry slots[NX_CONFIG_SNAPSHOT_MAX_SLOTS];
};

/* ---------- API -------------------------------------------------------- */

/*
 * Allocate a config handle in table `t`.  On success returns NX_OK and
 * sets *out_h.  Returns NX_EINVAL for NULL args, NX_ENOMEM if the table
 * is full.
 */
int nx_config_open(struct nx_handle_table *t, nx_handle_t *out_h);

/*
 * Populate *snap with a snapshot of the live composition.  Always returns
 * NX_OK; num_slots is capped at NX_CONFIG_SNAPSHOT_MAX_SLOTS.
 */
int nx_config_query_snapshot(struct nx_config_snapshot *snap);

/*
 * Swap the component in slot `slot_name` to the first registered
 * NX_LC_READY component whose manifest_id equals `new_impl`.
 *
 * Returns:
 *   NX_OK     — recompose completed; new component is ACTIVE
 *   NX_ENOENT — slot not registered, or no READY component with that id
 *   NX_EPERM  — slot is FROZEN
 *   NX_ESTATE — new component not in NX_LC_READY (shouldn't happen)
 *   other     — propagated from nx_recompose() (pause/drain failure)
 */
int nx_config_swap_component(const char *slot_name, const char *new_impl);

#endif /* NX_FRAMEWORK_CONFIG_H */
