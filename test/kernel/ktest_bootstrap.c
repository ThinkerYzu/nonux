#include "ktest.h"
#include "core/sched/task.h"
#include "framework/bootstrap.h"
#include "framework/component.h"
#include "framework/hook.h"
#include "framework/registry.h"

/*
 * Kernel-side coverage for slice 3.9a.
 *
 * `ktest_main` runs AFTER `boot_main` has called `nx_framework_
 * bootstrap()`, so the composition is already up when these tests
 * execute.  They assert that the expected slots / components /
 * bindings are in place — i.e. the boot walker actually ran and
 * drove every descriptor through init → enable.
 */

/* Tiny state struct defined in components/uart_pl011/uart_pl011.c.
 * Declared here (not in a header — component state is private by
 * design) so the test can sanity-check that init/enable actually
 * fired on the bound instance. */
struct uart_pl011_state {
    unsigned init_called;
    unsigned enable_called;
    unsigned messages_handled;
};

KTEST(bootstrap_registers_expected_slot)
{
    struct nx_slot *s = nx_slot_lookup("char_device.serial");
    KASSERT_NOT_NULL(s);
    KASSERT(strcmp(s->iface, "char_device") == 0);
}

KTEST(bootstrap_binds_uart_to_its_slot)
{
    struct nx_slot *s = nx_slot_lookup("char_device.serial");
    KASSERT_NOT_NULL(s);
    KASSERT_NOT_NULL(s->active);
    KASSERT(strcmp(s->active->manifest_id, "uart_pl011") == 0);
    KASSERT_EQ_U(s->active->state, NX_LC_ACTIVE);
}

KTEST(bootstrap_invoked_component_init_and_enable)
{
    struct nx_slot *s = nx_slot_lookup("char_device.serial");
    KASSERT_NOT_NULL(s);
    KASSERT_NOT_NULL(s->active);
    KASSERT_NOT_NULL(s->active->impl);
    const struct uart_pl011_state *us = s->active->impl;
    KASSERT_EQ_U(us->init_called,   1);
    KASSERT_EQ_U(us->enable_called, 1);
}

KTEST(bootstrap_component_count_matches_descriptor_section)
{
    /* Slice 3.9a wires a single real component (uart_pl011).  As more
     * land this count grows; the assertion stays "at least 1" so new
     * descriptors don't break this test. */
    KASSERT(nx_graph_component_count() >= 1);
}

KTEST(bootstrap_snapshot_json_contains_bound_impl)
{
    /* Bumped 2048 → 4096 in slice 8.0a.4 — adding posix_shim grew the
     * composition to 7 slots + 7 components + 4 connections (the
     * `posix_shim → {vfs, scheduler, memory.page_alloc, char_device.serial}`
     * edges from manifest deps), pushing the rendered JSON past 2 KiB.
     * Slice 8.0a.5 adds per-task `caller_slot` registrations (one slot
     * + 4 cloned edges per task) on top — the dispatcher kthread
     * (and any other kthread spawned through `nx_task_create` after
     * posix_shim is bound) contributes its own task-slot here.  4096
     * still fits with comfortable headroom; future composition growth
     * may need another bump. */
    static char buf[4096];
    struct nx_graph_snapshot *snap = nx_graph_snapshot_take();
    KASSERT_NOT_NULL(snap);

    int rc = nx_graph_snapshot_to_json(snap, buf, sizeof buf);
    KASSERT(rc > 0);

    /* Buffer must be NUL-terminated and contain the slot name and
     * the bound manifest.  strstr isn't in core/lib, so scan manually. */
    const char *needle_slot = "\"name\":\"char_device.serial\"";
    const char *needle_impl = "\"manifest\":\"uart_pl011\"";
    int found_slot = 0, found_impl = 0;
    for (size_t i = 0; buf[i]; i++) {
        if (!found_slot && strncmp(&buf[i], needle_slot, strlen(needle_slot)) == 0)
            found_slot = 1;
        if (!found_impl && strncmp(&buf[i], needle_impl, strlen(needle_impl)) == 0)
            found_impl = 1;
    }
    KASSERT(found_slot);
    KASSERT(found_impl);

    nx_graph_snapshot_put(snap);
}

/* ---- slice 3.9b.2: NX_HOOK_SLOT_SWAPPED runs end-to-end on kernel --- */

struct swap_capture {
    int                  fires;
    struct nx_slot      *slot;
    struct nx_component *old_impl;
    struct nx_component *new_impl;
};

static enum nx_hook_action capture_swap_hook(struct nx_hook_context *ctx,
                                             void                   *user)
{
    struct swap_capture *cap = user;
    cap->fires++;
    cap->slot     = ctx->u.swap.slot;
    cap->old_impl = ctx->u.swap.old_impl;
    cap->new_impl = ctx->u.swap.new_impl;
    return NX_HOOK_CONTINUE;
}

KTEST(slot_swap_fans_out_to_hook_chain_on_kernel)
{
    /* Register a fresh test-only slot so we don't perturb the live
     * bootstrap composition.  Pick something the dispatcher won't
     * also touch. */
    static struct nx_slot      s = { .name = "ktest.swap_probe", .iface = "x" };
    static struct nx_component a = { .manifest_id = "ktest_swap", .instance_id = "a" };
    static struct nx_component b = { .manifest_id = "ktest_swap", .instance_id = "b" };
    KASSERT_EQ_U(nx_slot_register(&s), NX_OK);
    KASSERT_EQ_U(nx_component_register(&a), NX_OK);
    KASSERT_EQ_U(nx_component_register(&b), NX_OK);

    static struct swap_capture cap;
    cap = (struct swap_capture){ 0 };
    static struct nx_hook h;
    h = (struct nx_hook){
        .point = NX_HOOK_SLOT_SWAPPED,
        .fn    = capture_swap_hook,
        .user  = &cap,
    };
    KASSERT_EQ_U(nx_hook_register(&h), NX_OK);

    KASSERT_EQ_U(nx_slot_swap(&s, &a), NX_OK);
    KASSERT_EQ_U(cap.fires, 1);
    KASSERT(cap.slot == &s);
    KASSERT(cap.old_impl == 0);
    KASSERT(cap.new_impl == &a);

    KASSERT_EQ_U(nx_slot_swap(&s, &b), NX_OK);
    KASSERT_EQ_U(cap.fires, 2);
    KASSERT(cap.old_impl == &a);
    KASSERT(cap.new_impl == &b);

    /* Tear down: hook off, slot back to NULL active, registry lookups
     * for later tests still find `s` but its active is NULL so no
     * dispatch path touches it. */
    nx_hook_unregister(&h);
    (void)nx_slot_swap(&s, 0);
}

/* ---- slice 8.0a.5: per-task caller_slot wiring on the kernel build ---- */

/*
 * In the live kernel composition `posix_shim` is bound, so every task
 * created via `nx_task_create` registers a `caller_slot` and clones
 * posix_shim's 4 outgoing edges.  These tests assert end-to-end on
 * the running kernel — host coverage in `task_test.c` already nails
 * down the create/destroy pairing under a synthesized fixture.
 */

struct task_slot_edge_count {
    struct nx_task *task;
    int             count;
};

static void count_outgoing_cb(struct nx_connection *c, void *ctx)
{
    struct task_slot_edge_count *acc = ctx;
    if (c && c->from_slot == &acc->task->caller_slot)
        acc->count++;
}

static void ktest_dummy_entry(void *arg) { (void)arg; }

KTEST(bootstrap_caller_slot_create_destroy_round_trip_in_kernel)
{
    /* End-to-end on the live kernel composition: posix_shim is bound,
     * so a fresh `nx_task_create` registers the caller_slot, clones
     * posix_shim's 4 outgoing edges, and `nx_task_destroy` unwinds
     * cleanly.  Mirrors task_test.c's host coverage for the path that
     * matters most — `wire_caller_slot` must work in the real kernel
     * registry, not just under a synthesized fixture. */
    size_t slots_before = nx_graph_slot_count();
    size_t conns_before = nx_graph_connection_count();

    struct nx_task *t = nx_task_create("ktest_pcs", ktest_dummy_entry,
                                       NULL, 1);
    KASSERT_NOT_NULL(t);
    KASSERT(t->caller_slot_active);
    KASSERT_NOT_NULL(t->caller_slot.active);
    KASSERT(strcmp(t->caller_slot.active->manifest_id, "posix_shim") == 0);
    KASSERT_EQ_U(nx_graph_slot_count(),       slots_before + 1);
    KASSERT_EQ_U(nx_graph_connection_count(), conns_before + 4);

    /* Slot looks up by its synthesized name. */
    struct nx_slot *looked_up = nx_slot_lookup(t->caller_slot_name);
    KASSERT(looked_up == &t->caller_slot);

    /* Edges all originate from this caller_slot. */
    struct task_slot_edge_count acc = { .task = t, .count = 0 };
    nx_slot_foreach_dependency(&t->caller_slot, count_outgoing_cb, &acc);
    KASSERT_EQ_U(acc.count, 4);

    nx_task_destroy(t);
    KASSERT_EQ_U(nx_graph_slot_count(),       slots_before);
    KASSERT_EQ_U(nx_graph_connection_count(), conns_before);
}
