#include "framework/bootstrap.h"
#include "framework/component.h"
#include "framework/process.h"
#include "framework/registry.h"

#if !__STDC_HOSTED__
#include "core/mmu/mmu.h"
#include "core/sched/sched.h"
#include "interfaces/scheduler.h"
#include "framework/dispatcher.h"
#endif

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
 * nx_boot_slots[] / nx_boot_slots_count live in gen/slot_table.c, which
 * is always present in the kernel build (the Makefile cascades
 * gen/slot_table.c off gen/sources.mk).  Slice 3.9a carried a weak
 * fallback here to make trivial test builds link without a generated
 * table, but slice 4.3 hit a nasty `const` + weak-tentative
 * constant-folding bug under `-O2`: the compiler saw the weak
 * `const unsigned nx_boot_slots_count;` as 0 and folded the
 * slot-register loop below into dead code, silently dropping every
 * slot past the first.  Removing the fallbacks makes the extern
 * decls in framework/bootstrap.h authoritative and forces a real
 * runtime load on every read.
 */

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
    /* Slice 7.2: seed the always-present kernel process with the
     * kernel's own TTBR0 root.  `mmu_init()` has already run by the
     * time bootstrap fires (boot.c order), so the kernel L1 root is
     * live and picking it up here is safe.  On host builds
     * `mmu_kernel_address_space()` returns 0 and we stay with the
     * zero-initialised placeholder. */
#if !__STDC_HOSTED__
    g_kernel_process.ttbr0_root = mmu_kernel_address_space();
#endif

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

#if !__STDC_HOSTED__
    /* Slice 4.4: if the composition includes a `scheduler` slot and
     * its bound component exports an iface_ops table, hand it to the
     * core scheduler driver.  This is the single non-slot scheduler-
     * pointer publication the kernel makes — DESIGN.md §Scheduler:
     * Core Driver + Component.  No-op (with no crash) for builds
     * that don't wire up a scheduler. */
    {
        struct nx_slot *sched_slot = nx_slot_lookup("scheduler");
        if (sched_slot && sched_slot->active &&
            sched_slot->active->descriptor &&
            sched_slot->active->descriptor->iface_ops) {
            const struct nx_scheduler_ops *sops =
                sched_slot->active->descriptor->iface_ops;
            sched_init(sops, sched_slot->active->impl);
        }
    }

    /* Slice 3.9b.1: spawn the framework dispatcher kthread.  Must
     * run after sched_init so sched_spawn_kthread has a policy to
     * enqueue onto; before the caller enables IRQs so the first
     * async message the tick generates lands in a ready MPSC. */
    {
        int rc = nx_dispatcher_init();
        if (rc != NX_OK) return rc;
    }
#endif

    return NX_OK;
}
