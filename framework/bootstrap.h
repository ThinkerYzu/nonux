#ifndef NX_FRAMEWORK_BOOTSTRAP_H
#define NX_FRAMEWORK_BOOTSTRAP_H

#include "framework/component.h"
#include "framework/registry.h"

/*
 * Framework bootstrap — slice 3.9a.
 *
 * `nx_framework_bootstrap()` is the single entry point called from
 * the kernel's boot sequence (after PMM, IRQ, timer are up).  It:
 *
 *   1. Registers every slot declared in the auto-generated slot
 *      table (`nx_boot_slots[]`, emitted by tools/gen-config.py).
 *   2. Walks the nx_components linker section and registers every
 *      component descriptor it finds.
 *   3. Binds each component to its slot by matching
 *      descriptor->name against each slot's `impl` field in the
 *      boot table.  Unmatched components stay unbound (not an
 *      error — they may be manifest-optional or bound later).
 *   4. Runs Kahn's-algorithm topological sort over the descriptors'
 *      dependency lists, then per component in order:
 *      nx_resolve_deps → nx_component_init → nx_component_enable.
 *      A missing required dep panics boot with a pointer at the
 *      offending manifest.
 *
 * The function does NOT fire hooks during bootstrap registration
 * (hooks are registered AFTER composition is up in v1; slice 3.9b
 * adds static hook wiring from kernel.json).
 */

/* Per-slot binding emitted by gen-config into gen/slot_table.c. */
struct nx_boot_slot {
    struct nx_slot  *slot;         /* static storage, already-initialized
                                     * name/iface/mutability/concurrency */
    const char      *impl_name;    /* matches descriptor->name */
};

/* Provided by gen/slot_table.c.  If the gen/ tree hasn't been
 * generated (e.g. a trivial test build) these resolve to 0 / NULL
 * via a weak fallback defined in bootstrap.c. */
extern struct nx_boot_slot  nx_boot_slots[];
extern const unsigned       nx_boot_slots_count;

/*
 * Run the whole bring-up sequence.  Returns NX_OK if every component
 * reached ACTIVE, or a negative NX_E* on the first failure (component
 * init / enable returning non-zero, missing required dep, topo cycle).
 *
 * On failure the composition is left partially up — callers (the boot
 * path in particular) should treat any non-OK return as a fatal
 * condition and panic.  Slice 3.9b adds a bounded retry / rollback
 * story once kthreads exist.
 */
int nx_framework_bootstrap(void);

#endif /* NX_FRAMEWORK_BOOTSTRAP_H */
