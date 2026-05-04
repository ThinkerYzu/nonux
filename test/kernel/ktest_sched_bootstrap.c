/*
 * Kernel-side coverage for slice 4.3 / slice 8.4.
 *
 * After `boot_main` runs `nx_framework_bootstrap()`, the `scheduler`
 * slot declared in kernel.json must be bound to the sched_priority
 * component in NX_LC_ACTIVE state, with init + enable having fired
 * exactly once.  Mirrors the slice-3.9a uart_pl011 bootstrap tests.
 *
 * Slice 4.4 adds the `sched_init` handoff (bootstrap stashes
 * sched_priority's scheduler_ops in the core driver's g_sched pointer);
 * that test lives with slice 4.4.
 *
 * Updated at slice 8.4: scheduler slot now bound to sched_priority
 * (was sched_rr).  State-mirror layout updated accordingly.
 */

#include "ktest.h"
#include "framework/bootstrap.h"
#include "framework/component.h"
#include "framework/registry.h"
#include "framework/hook.h"

/* Mirror of sched_priority.c's private state struct — same technique
 * as ktest_bootstrap.c's uart_pl011_state mirror.  queues[8] each
 * hold a single nx_list_head (two void* = 16 bytes); counters follow.
 * If the component reorders fields the counter reads below produce
 * unexpected values and the test fails loudly. */
struct sched_priority_state_mirror {
    struct { struct { void *next, *prev; } n; } queues[8]; /* 128 bytes */
    unsigned  quantum_ticks;
    unsigned  remaining;
    unsigned  init_called;
    unsigned  enable_called;
    unsigned  disable_called;
    unsigned  destroy_called;
};

KTEST(bootstrap_registers_scheduler_slot)
{
    struct nx_slot *s = nx_slot_lookup("scheduler");
    KASSERT_NOT_NULL(s);
    KASSERT(strcmp(s->iface, "scheduler") == 0);
}

KTEST(bootstrap_binds_sched_priority_to_scheduler_slot)
{
    struct nx_slot *s = nx_slot_lookup("scheduler");
    KASSERT_NOT_NULL(s);
    KASSERT_NOT_NULL(s->active);
    KASSERT(strcmp(s->active->manifest_id, "sched_priority") == 0);
    KASSERT_EQ_U(s->active->state, NX_LC_ACTIVE);
}

KTEST(bootstrap_invoked_sched_priority_init_and_enable)
{
    struct nx_slot *s = nx_slot_lookup("scheduler");
    KASSERT_NOT_NULL(s);
    KASSERT_NOT_NULL(s->active);
    KASSERT_NOT_NULL(s->active->impl);
    const struct sched_priority_state_mirror *m = s->active->impl;
    KASSERT_EQ_U(m->init_called,   1);
    KASSERT_EQ_U(m->enable_called, 1);
    /* Default quantum must have landed; exact value is a component
     * implementation detail so we just check it's > 0. */
    KASSERT(m->quantum_ticks > 0);
}

KTEST(bootstrap_context_switch_hook_point_ready_but_unused)
{
    /* Slice 4.3 ships the enum; slice 4.4 fires the hook from the
     * reschedule shim.  For now, no hooks are registered and the
     * chain length is zero — prove the enum wiring survives the
     * kernel boot path (not just the host build). */
    KASSERT(NX_HOOK_CONTEXT_SWITCH < NX_HOOK_POINT_COUNT);
    KASSERT_EQ_U(nx_hook_chain_length(NX_HOOK_CONTEXT_SWITCH), 0);
}
