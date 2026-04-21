#include "framework/bootstrap.h"
#include "framework/component.h"
#include "framework/registry.h"

#if __STDC_HOSTED__
#include <stdlib.h>
#include <string.h>
#else
#include "core/lib/kheap.h"
#include "core/lib/lib.h"
#endif

#include <stddef.h>

/*
 * Framework bootstrap — slice 3.9a.  See framework/bootstrap.h for the
 * contract.
 *
 * Linker-generated symbols mark the nx_components section (see
 * core/boot/linker.ld).  Each entry is a `struct nx_component_descriptor`
 * emitted by NX_COMPONENT_REGISTER (framework/component.h).
 */
extern const struct nx_component_descriptor __start_nx_components[];
extern const struct nx_component_descriptor __stop_nx_components[];

/*
 * Weak fallbacks for gen/slot_table.c.  A trivial build (e.g. a host
 * test that doesn't generate a slot table) sees an empty table rather
 * than an unresolved reference.
 */
__attribute__((weak)) struct nx_boot_slot nx_boot_slots[1];
__attribute__((weak)) const unsigned      nx_boot_slots_count;

/* ---- Scratch state -------------------------------------------------- */

/*
 * The bootstrap allocates two arrays through the kheap: one parallel
 * to the descriptor section with our working `struct nx_component *`,
 * and one topological-ordering index array.  Both have bounded
 * lifetimes — we release them on success OR failure.
 */

/* Find the slot whose impl_name matches descriptor->name.  Returns
 * NULL if no slot names this component — the component is still
 * registered, just unbound, and init/enable still run. */
static struct nx_slot *find_slot_for(const struct nx_component_descriptor *d)
{
    for (unsigned i = 0; i < nx_boot_slots_count; i++) {
        const char *impl = nx_boot_slots[i].impl_name;
        if (impl && d->name && strcmp(impl, d->name) == 0)
            return nx_boot_slots[i].slot;
    }
    return NULL;
}

/* True if every REQUIRED dep of `d` refers to a slot that already has
 * its active impl marked as "ready" via `visited[k]==true`.  Optional
 * deps are ignored — they can resolve in any order. */
static bool deps_ready(const struct nx_component_descriptor *d,
                       struct nx_component * const *comps,
                       const bool *visited,
                       size_t n)
{
    for (size_t i = 0; i < d->n_deps; i++) {
        const struct nx_dep_descriptor *dep = &d->deps[i];
        if (!dep->required) continue;

        struct nx_slot *target = nx_slot_lookup(dep->name);
        if (!target || !target->active) {
            /* Required dep refers to a slot with no impl at all —
             * manifest / kernel.json mismatch.  Caller reports. */
            return false;
        }
        /* Find the position of `target->active` in our `comps` list
         * and check it's visited. */
        for (size_t j = 0; j < n; j++) {
            if (comps[j] == target->active) {
                if (!visited[j]) return false;
                break;
            }
        }
    }
    return true;
}

int nx_framework_bootstrap(void)
{
    /* Step 1: register every slot in the boot table. */
    for (unsigned i = 0; i < nx_boot_slots_count; i++) {
        int rc = nx_slot_register(nx_boot_slots[i].slot);
        if (rc != NX_OK && rc != NX_EEXIST) return rc;
    }

    /* Step 2: walk the descriptor section; register one component
     * per descriptor and (optionally) bind it to its slot. */
    size_t n = (size_t)(__stop_nx_components - __start_nx_components);
    if (n == 0) return NX_OK;       /* empty composition is legal */

    struct nx_component **comps = calloc(n, sizeof *comps);
    if (!comps) return NX_ENOMEM;

    for (size_t i = 0; i < n; i++) {
        const struct nx_component_descriptor *d = &__start_nx_components[i];

        struct nx_component *c = calloc(1, sizeof *c);
        if (!c) return NX_ENOMEM;
        c->manifest_id = d->name;
        c->instance_id = "0";
        c->descriptor  = d;
        if (d->state_size > 0) {
            c->impl = calloc(1, d->state_size);
            if (!c->impl) return NX_ENOMEM;
        }

        int rc = nx_component_register(c);
        if (rc != NX_OK) return rc;
        comps[i] = c;

        struct nx_slot *slot = find_slot_for(d);
        if (slot) {
            rc = nx_slot_swap(slot, c);
            if (rc != NX_OK) return rc;
        }
    }

    /* Step 3: topo sort.  Bounded O(n²) since n is small and
     * deps per component are typically 0–3.  A cycle or a missing
     * required dep stalls the loop — we report NX_ELOOP or NX_ENOENT
     * respectively. */
    bool *visited = calloc(n, sizeof *visited);
    if (!visited) return NX_ENOMEM;

    size_t done = 0;
    while (done < n) {
        bool progressed = false;
        for (size_t i = 0; i < n; i++) {
            if (visited[i]) continue;
            const struct nx_component_descriptor *d = &__start_nx_components[i];

            if (!deps_ready(d, comps, visited, n)) continue;

            struct nx_slot *self_slot = find_slot_for(d);
            int rc = nx_resolve_deps(d, self_slot, comps[i]->impl);
            if (rc != NX_OK) return rc;

            rc = nx_component_init(comps[i]);
            if (rc != NX_OK) return rc;

            rc = nx_component_enable(comps[i]);
            if (rc != NX_OK) return rc;

            visited[i] = true;
            done++;
            progressed = true;
        }
        if (!progressed) {
            /* Either a cycle in the required-dep graph or a dep that
             * references a slot with no bound component.  Either way
             * we can't make forward progress. */
            return NX_ELOOP;
        }
    }

    return NX_OK;
}
