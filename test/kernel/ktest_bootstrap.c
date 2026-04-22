#include "ktest.h"
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
    static char buf[2048];
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
