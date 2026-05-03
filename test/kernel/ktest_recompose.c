/*
 * Kernel tests for slice 8.2 — nx_recompose() orchestrator.
 *
 * Two tests:
 *   1. Leaf slot REPLACE: old comp destroyed, new comp ACTIVE, messages
 *      sent after the swap are dispatched by the new component via the
 *      kernel MPSC dispatcher.
 *   2. Messages queued to the hold queue while the slot is paused reach
 *      the new component after the recompose flushes the hold queue.
 */

#include "ktest.h"

#include "framework/component.h"
#include "framework/dispatcher.h"
#include "framework/ipc.h"
#include "framework/recompose.h"
#include "framework/registry.h"
#include "core/sched/sched.h"
#include "core/sched/task.h"

/* ---- Shared sink component -------------------------------------------- */

static int g_old_handle_count;
static int g_new_handle_count;
static int g_old_pause_hook_count;
static int g_old_pause_count;

static int old_handle(void *self, struct nx_ipc_message *msg)
{
    (void)self; (void)msg;
    g_old_handle_count++;
    return NX_OK;
}
static int new_handle(void *self, struct nx_ipc_message *msg)
{
    (void)self; (void)msg;
    g_new_handle_count++;
    return NX_OK;
}
static int old_pause_hook(void *self) { (void)self; g_old_pause_hook_count++; return NX_OK; }
static int old_pause(void *self)      { (void)self; g_old_pause_count++;      return NX_OK; }

static const struct nx_component_ops old_comp_ops = {
    .handle_msg = old_handle,
    .pause_hook = old_pause_hook,
    .pause      = old_pause,
};
static const struct nx_component_descriptor old_comp_desc = {
    .name  = "krecomp_old",
    .ops   = &old_comp_ops,
};

static const struct nx_component_ops new_comp_ops = {
    .handle_msg = new_handle,
};
static const struct nx_component_descriptor new_comp_desc = {
    .name  = "krecomp_new",
    .ops   = &new_comp_ops,
};

/* ---- Per-test fixture ------------------------------------------------- */

struct recomp_fixture {
    struct nx_slot       src;
    struct nx_slot       tgt;
    struct nx_component  src_comp;
    struct nx_component  old_comp;
    struct nx_component  new_comp;
    struct nx_connection *edge;
};

static void krecomp_fixture_up(struct recomp_fixture *f,
                                const char *src_name, const char *tgt_name,
                                enum nx_pause_policy policy)
{
    g_old_handle_count = g_new_handle_count = 0;
    g_old_pause_hook_count = g_old_pause_count = 0;
    nx_dispatcher_reset();

    f->src = (struct nx_slot){ .name = src_name, .iface = "krecomp_test",
                               .mutability  = NX_MUT_HOT,
                               .concurrency = NX_CONC_SHARED };
    f->tgt = (struct nx_slot){ .name = tgt_name, .iface = "krecomp_test",
                               .mutability  = NX_MUT_HOT,
                               .concurrency = NX_CONC_SHARED };
    f->src_comp = (struct nx_component){ .manifest_id = "krecomp_s",  .instance_id = "0" };
    f->old_comp = (struct nx_component){ .manifest_id = "krecomp_old",.instance_id = "0",
                                         .descriptor  = &old_comp_desc };
    f->new_comp = (struct nx_component){ .manifest_id = "krecomp_new",.instance_id = "0",
                                         .descriptor  = &new_comp_desc };

    (void)nx_slot_register(&f->src);
    (void)nx_slot_register(&f->tgt);
    (void)nx_component_register(&f->src_comp);
    (void)nx_component_register(&f->old_comp);
    (void)nx_component_register(&f->new_comp);
    (void)nx_slot_swap(&f->src, &f->src_comp);
    (void)nx_slot_swap(&f->tgt, &f->old_comp);
    (void)nx_component_init(&f->old_comp);
    (void)nx_component_enable(&f->old_comp);
    (void)nx_component_init(&f->new_comp);   /* new_comp stays READY */

    int err = NX_OK;
    f->edge = nx_connection_register(&f->src, &f->tgt,
                                     NX_CONN_ASYNC, false, policy, &err);
}

static void krecomp_fixture_down(struct recomp_fixture *f)
{
    (void)nx_slot_swap(&f->src, NULL);
    (void)nx_slot_swap(&f->tgt, NULL);
    (void)nx_component_unregister(&f->src_comp);
    /* old_comp was DESTROYED by nx_recompose; only unregister if still in registry. */
    if (f->old_comp.state != NX_LC_DESTROYED)
        (void)nx_component_unregister(&f->old_comp);
    if (f->new_comp.state == NX_LC_ACTIVE || f->new_comp.state == NX_LC_READY ||
        f->new_comp.state == NX_LC_PAUSED)
        (void)nx_component_unregister(&f->new_comp);
    (void)nx_slot_unregister(&f->src);
    (void)nx_slot_unregister(&f->tgt);
    nx_dispatcher_reset();
}

/* ======================================================================== */
/* Test 1: leaf slot swap — new comp handles messages after the swap        */
/* ======================================================================== */

KTEST(recompose_kernel_leaf_slot_swap)
{
    struct recomp_fixture f;
    krecomp_fixture_up(&f, "kr1_src", "kr1_tgt", NX_PAUSE_QUEUE);

    struct nx_slot_change change = { .slot = &f.tgt, .action = NX_SLOT_REPLACE,
                                     .new_comp = &f.new_comp };
    struct recomp_plan plan = { .changes = &change, .num_changes = 1 };

    KASSERT_EQ_U(nx_recompose(&plan), NX_OK);

    /* New comp is bound and ACTIVE. */
    KASSERT(f.tgt.active == &f.new_comp);
    KASSERT_EQ_U(f.new_comp.state, NX_LC_ACTIVE);
    KASSERT_EQ_U(f.old_comp.state, NX_LC_DESTROYED);
    KASSERT_EQ_U(nx_slot_pause_state(&f.tgt), NX_SLOT_PAUSE_NONE);

    /* Send a message to the swapped slot; dispatcher should deliver to new comp. */
    struct nx_ipc_message msg = {
        .src_slot = &f.src, .dst_slot = &f.tgt, .msg_type = 99,
    };
    KASSERT_EQ_U(nx_ipc_send(&msg), NX_OK);

    /* Yield until the dispatcher delivers it (budget: 64 yields). */
    for (int i = 0; i < 64 && g_new_handle_count < 1; i++)
        nx_task_yield();

    KASSERT_EQ_U(g_new_handle_count,      1);
    KASSERT_EQ_U(g_old_handle_count,      0);
    KASSERT_EQ_U(g_old_pause_hook_count,  1);
    KASSERT_EQ_U(g_old_pause_count,       1);

    krecomp_fixture_down(&f);
}

/* ======================================================================== */
/* Test 2: messages queued to hold queue reach new comp after recompose     */
/* ======================================================================== */

KTEST(recompose_kernel_hold_queue_reaches_new_component)
{
    struct recomp_fixture f;
    krecomp_fixture_up(&f, "kr2_src", "kr2_tgt", NX_PAUSE_QUEUE);

    /* Manually pause old_comp so incoming messages go to the hold queue. */
    KASSERT_EQ_U(nx_component_pause(&f.old_comp), NX_OK);
    KASSERT_EQ_U(nx_slot_pause_state(&f.tgt), NX_SLOT_PAUSE_DONE);

    /* Send two messages — they land on the hold queue (QUEUE policy). */
    struct nx_ipc_message m1 = { .src_slot = &f.src, .dst_slot = &f.tgt, .msg_type = 1 };
    struct nx_ipc_message m2 = { .src_slot = &f.src, .dst_slot = &f.tgt, .msg_type = 2 };
    KASSERT_EQ_U(nx_ipc_send(&m1), NX_OK);
    KASSERT_EQ_U(nx_ipc_send(&m2), NX_OK);
    KASSERT_EQ_U(nx_ipc_hold_queue_depth(&f.src, &f.tgt), 2);

    /* Recompose: replace old with new. slot_clear_pause flushes hold queue. */
    struct nx_slot_change change = { .slot = &f.tgt, .action = NX_SLOT_REPLACE,
                                     .new_comp = &f.new_comp };
    struct recomp_plan plan = { .changes = &change, .num_changes = 1 };
    KASSERT_EQ_U(nx_recompose(&plan), NX_OK);

    /* Hold queue must be empty after the flush. */
    KASSERT_EQ_U(nx_ipc_hold_queue_depth(&f.src, &f.tgt), 0);

    /* Both messages were re-injected via the router into the MPSC.
     * Yield until the dispatcher delivers them. */
    for (int i = 0; i < 64 && g_new_handle_count < 2; i++)
        nx_task_yield();

    KASSERT_EQ_U(g_new_handle_count,   2);
    KASSERT_EQ_U(g_old_handle_count,   0);

    krecomp_fixture_down(&f);
}
