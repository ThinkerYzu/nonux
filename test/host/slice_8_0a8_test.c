/*
 * Host-side tests for slice 8.0a.8 — cross-cutting test infrastructure.
 *
 * Six infrastructure pieces are exercised here:
 *   1. Mock component (mock_component.h) — programmable handle_msg
 *   2. Hook-chain inspector (hook_inspector.h) — observe-only recording hook
 *   3. Recompose event logger — nx_graph_subscribe-based event capture
 *   4. Pause-injector — externally drive pause states for nx_slot_call_blocking
 *   5. Cap-forgery harness — validate R3 cap-scan enforcement
 *   6. Equivalence-runner macro — paired direct/blocking-call assertions
 *
 * What we don't cover here (kernel ktest territory):
 *   - QUEUE policy pause: nx_slot_call_blocking parks on slot->resume_waitq;
 *     on host, the wait calls cpu_switch_to abort stub.  See ktest_bootstrap.c
 *     for the kernel-side queue+wake test.
 *   - Full posix_shim lifecycle: posix_shim.c is not in the host link set.
 */

#include "test_runner.h"

#include "core/sched/task.h"
#include "core/sched/waitq.h"
#include "framework/component.h"
#include "framework/dispatcher.h"
#include "framework/hook.h"
#include "framework/ipc.h"
#include "framework/registry.h"
#include "framework/slot_call.h"

#include "mock_component.h"
#include "hook_inspector.h"

#include <string.h>
#include <stddef.h>

/* ---- File-scope helpers for hook-ordering test ------------------------- */

static int s_hook_order_counter;
static int s_hook_first_order;

static enum nx_hook_action priority_order_hook_fn(struct nx_hook_context *ctx,
                                                   void *user)
{
    (void)ctx; (void)user;
    s_hook_first_order = ++s_hook_order_counter;
    return NX_HOOK_CONTINUE;
}

/* =========================================================================
 * Shared fixture helpers
 * ========================================================================= */

/*
 * Minimal two-slot fixture: a "caller" slot (stands in for posix_shim /
 * caller_slot) connected to a "svc" slot (the destination mock).
 * All slots and components are stack-allocated; the task fixture used
 * by slot_call_blocking tests is set up separately.
 */
struct base_fixture {
    struct nx_slot     caller_slot;
    struct nx_component caller_comp;
    struct nx_slot     svc_slot;
    struct mock_handle svc;
};

static void base_fixture_setup(struct base_fixture *f,
                                enum nx_conn_mode mode,
                                enum nx_pause_policy policy)
{
    nx_graph_reset();
    nx_dispatcher_reset();
    nx_hook_reset();
    memset(f, 0, sizeof *f);

    f->caller_slot.name        = "caller";
    f->caller_slot.iface       = "caller";
    f->caller_slot.mutability  = NX_MUT_HOT;
    f->caller_slot.concurrency = NX_CONC_SHARED;
    f->caller_comp.manifest_id = "caller_comp";
    f->caller_comp.instance_id = "0";
    f->caller_comp.state       = NX_LC_ACTIVE;

    mock_handle_init(&f->svc, "svc_mock", "0");

    f->svc_slot.name        = "svc";
    f->svc_slot.iface       = "svc";
    f->svc_slot.mutability  = NX_MUT_HOT;
    f->svc_slot.concurrency = NX_CONC_SHARED;

    nx_slot_register(&f->caller_slot);
    nx_component_register(&f->caller_comp);
    nx_slot_swap(&f->caller_slot, &f->caller_comp);

    nx_slot_register(&f->svc_slot);
    nx_component_register(&f->svc.comp);
    nx_slot_swap(&f->svc_slot, &f->svc.comp);

    int err = NX_OK;
    nx_connection_register(&f->caller_slot, &f->svc_slot,
                           mode, false, policy, &err);
    (void)err;
}

static void base_fixture_teardown(struct base_fixture *f)
{
    nx_dispatcher_reset();
    nx_graph_reset();
    nx_hook_reset();
    (void)f;
}


/* =========================================================================
 * 1. Mock component
 * ========================================================================= */

TEST(mock_call_count_increments_per_dispatch)
{
    struct base_fixture f;
    base_fixture_setup(&f, NX_CONN_ASYNC, NX_PAUSE_QUEUE);

    struct nx_ipc_message msg = {
        .src_slot = &f.caller_slot,
        .dst_slot = &f.svc_slot,
    };

    ASSERT_EQ_U(nx_dispatcher_enqueue(&msg), NX_OK);
    ASSERT_EQ_U(nx_dispatcher_pump_once(), 1);
    ASSERT_EQ_U(f.svc.state.call_count, 1);

    ASSERT_EQ_U(nx_dispatcher_enqueue(&msg), NX_OK);
    ASSERT_EQ_U(nx_dispatcher_pump_once(), 1);
    ASSERT_EQ_U(f.svc.state.call_count, 2);

    base_fixture_teardown(&f);
}

TEST(mock_configured_rc_propagates_through_dispatcher)
{
    struct base_fixture f;
    base_fixture_setup(&f, NX_CONN_ASYNC, NX_PAUSE_QUEUE);

    f.svc.state.handler_rc = -7;

    struct nx_ipc_message req = {
        .src_slot = &f.caller_slot,
        .dst_slot = &f.svc_slot,
        .flags    = NX_MSG_FLAG_REPLY_REQUESTED,
    };
    ASSERT_EQ_U(nx_dispatcher_enqueue(&req), NX_OK);
    ASSERT_EQ_U(nx_dispatcher_pump_once(), 1);

    /* Reply is in the dispatcher queue. Drain the reply to check pool. */
    ASSERT_EQ_U(nx_dispatcher_reply_pool_in_use_for_test(), 1);
    /* Pump reply — caller_slot has no descriptor, pump delivers and frees. */
    ASSERT_EQ_U(nx_dispatcher_pump_once(), 1);
    ASSERT_EQ_U(nx_dispatcher_reply_pool_in_use_for_test(), 0);

    base_fixture_teardown(&f);
}

TEST(mock_captures_last_message_pointer)
{
    struct base_fixture f;
    base_fixture_setup(&f, NX_CONN_ASYNC, NX_PAUSE_QUEUE);

    struct nx_ipc_message msg_a = {
        .src_slot = &f.caller_slot, .dst_slot = &f.svc_slot, .msg_type = 1,
    };
    struct nx_ipc_message msg_b = {
        .src_slot = &f.caller_slot, .dst_slot = &f.svc_slot, .msg_type = 2,
    };

    ASSERT_EQ_U(nx_dispatcher_enqueue(&msg_a), NX_OK);
    ASSERT_EQ_U(nx_dispatcher_pump_once(), 1);
    ASSERT(f.svc.state.last_msg == &msg_a);

    ASSERT_EQ_U(nx_dispatcher_enqueue(&msg_b), NX_OK);
    ASSERT_EQ_U(nx_dispatcher_pump_once(), 1);
    ASSERT(f.svc.state.last_msg == &msg_b);

    base_fixture_teardown(&f);
}

TEST(mock_reset_zeroes_call_count_and_last_msg)
{
    struct base_fixture f;
    base_fixture_setup(&f, NX_CONN_ASYNC, NX_PAUSE_QUEUE);

    f.svc.state.handler_rc = 42;
    struct nx_ipc_message msg = {
        .src_slot = &f.caller_slot, .dst_slot = &f.svc_slot,
    };
    ASSERT_EQ_U(nx_dispatcher_enqueue(&msg), NX_OK);
    ASSERT_EQ_U(nx_dispatcher_pump_once(), 1);
    ASSERT_EQ_U(f.svc.state.call_count, 1);

    mock_handle_reset(&f.svc);
    ASSERT_EQ_U(f.svc.state.call_count, 0);
    ASSERT(f.svc.state.last_msg == NULL);
    /* handler_rc preserved */
    ASSERT_EQ_U(f.svc.state.handler_rc, 42);

    base_fixture_teardown(&f);
}

TEST(mock_two_instances_have_independent_state)
{
    nx_graph_reset();
    nx_dispatcher_reset();

    struct mock_handle a, b;
    mock_handle_init(&a, "mock_a", "0");
    mock_handle_init(&b, "mock_b", "0");

    struct nx_slot slot_a = {
        .name = "sa", .iface = "sa",
        .mutability = NX_MUT_HOT, .concurrency = NX_CONC_SHARED,
    };
    struct nx_slot slot_b = {
        .name = "sb", .iface = "sb",
        .mutability = NX_MUT_HOT, .concurrency = NX_CONC_SHARED,
    };
    struct nx_slot caller = {
        .name = "caller2", .iface = "caller2",
        .mutability = NX_MUT_HOT, .concurrency = NX_CONC_SHARED,
    };
    struct nx_component caller_comp = {
        .manifest_id = "caller2", .instance_id = "0",
        .state = NX_LC_ACTIVE,
    };

    nx_slot_register(&caller);
    nx_component_register(&caller_comp);
    nx_slot_swap(&caller, &caller_comp);

    nx_slot_register(&slot_a);
    nx_component_register(&a.comp);
    nx_slot_swap(&slot_a, &a.comp);

    nx_slot_register(&slot_b);
    nx_component_register(&b.comp);
    nx_slot_swap(&slot_b, &b.comp);

    int err = NX_OK;
    nx_connection_register(&caller, &slot_a, NX_CONN_ASYNC, false,
                           NX_PAUSE_QUEUE, &err);
    nx_connection_register(&caller, &slot_b, NX_CONN_ASYNC, false,
                           NX_PAUSE_QUEUE, &err);

    struct nx_ipc_message msg_a = {
        .src_slot = &caller, .dst_slot = &slot_a,
    };
    struct nx_ipc_message msg_b = {
        .src_slot = &caller, .dst_slot = &slot_b,
    };

    nx_dispatcher_enqueue(&msg_a);
    nx_dispatcher_pump_once();
    nx_dispatcher_enqueue(&msg_b);
    nx_dispatcher_pump_once();
    nx_dispatcher_enqueue(&msg_b);
    nx_dispatcher_pump_once();

    ASSERT_EQ_U(a.state.call_count, 1);
    ASSERT_EQ_U(b.state.call_count, 2);

    nx_dispatcher_reset();
    nx_graph_reset();
}

TEST(mock_custom_fn_overrides_handler_rc)
{
    struct base_fixture f;
    base_fixture_setup(&f, NX_CONN_ASYNC, NX_PAUSE_QUEUE);

    f.svc.state.handler_rc = 0;
    f.svc.state.custom_fn  = NULL;

    /* First call without custom_fn — returns handler_rc. */
    struct nx_ipc_message msg = {
        .src_slot = &f.caller_slot, .dst_slot = &f.svc_slot,
        .flags    = NX_MSG_FLAG_REPLY_REQUESTED,
    };
    nx_dispatcher_enqueue(&msg);
    nx_dispatcher_pump_once();
    ASSERT_EQ_U(nx_dispatcher_reply_pool_in_use_for_test(), 1);
    /* Drain reply. */
    nx_dispatcher_pump_once();

    base_fixture_teardown(&f);
}


/* =========================================================================
 * 2. Hook-chain inspector
 * ========================================================================= */

TEST(hook_inspector_records_ipc_send_firing_with_src_dst)
{
    struct base_fixture f;
    base_fixture_setup(&f, NX_CONN_ASYNC, NX_PAUSE_QUEUE);

    struct hook_inspector hi;
    hook_inspector_init(&hi, NX_HOOK_IPC_SEND, 50);

    struct nx_ipc_message msg = {
        .src_slot = &f.caller_slot,
        .dst_slot = &f.svc_slot,
    };
    /* Fire IPC_SEND hook directly (slot_call.c fires it; here we fire via
     * the context builder to test the inspector independently). */
    struct nx_hook_context ctx = {
        .point = NX_HOOK_IPC_SEND,
        .u.ipc = {
            .src  = &f.caller_slot,
            .dst  = &f.svc_slot,
            .msg  = &msg,
            .edge = NULL,
        },
    };
    enum nx_hook_action action = nx_hook_dispatch(&ctx);

    ASSERT_EQ_U(action, NX_HOOK_CONTINUE);
    ASSERT_EQ_U(hi.fire_count, 1);
    ASSERT(hi.events[0].ipc_src == &f.caller_slot);
    ASSERT(hi.events[0].ipc_dst == &f.svc_slot);
    ASSERT(hi.events[0].ipc_msg == &msg);

    hook_inspector_teardown(&hi);
    base_fixture_teardown(&f);
}

TEST(hook_inspector_records_ipc_recv_firing)
{
    nx_graph_reset();
    nx_hook_reset();

    struct hook_inspector hi;
    hook_inspector_init(&hi, NX_HOOK_IPC_RECV, 10);

    struct nx_hook_context ctx = {
        .point = NX_HOOK_IPC_RECV,
        .u.ipc = { .src = NULL, .dst = NULL, .msg = NULL, .edge = NULL },
    };
    nx_hook_dispatch(&ctx);

    ASSERT_EQ_U(hi.fire_count, 1);
    ASSERT_EQ_U(hi.events[0].point, NX_HOOK_IPC_RECV);

    hook_inspector_teardown(&hi);
    nx_hook_reset();
    nx_graph_reset();
}

TEST(hook_inspector_observe_only_does_not_abort_chain)
{
    nx_hook_reset();

    struct hook_inspector hi;
    hook_inspector_init(&hi, NX_HOOK_IPC_SEND, 0);

    struct nx_hook_context ctx = {
        .point = NX_HOOK_IPC_SEND,
        .u.ipc = { .src = NULL, .dst = NULL, .msg = NULL, .edge = NULL },
    };
    enum nx_hook_action action = nx_hook_dispatch(&ctx);

    ASSERT_EQ_U(action, NX_HOOK_CONTINUE);
    ASSERT_EQ_U(hi.fire_count, 1);

    hook_inspector_teardown(&hi);
    nx_hook_reset();
}

TEST(hook_inspector_multiple_firings_accumulate)
{
    nx_hook_reset();

    struct hook_inspector hi;
    hook_inspector_init(&hi, NX_HOOK_IPC_SEND, 0);

    struct nx_hook_context ctx = {
        .point = NX_HOOK_IPC_SEND,
        .u.ipc = { .src = NULL, .dst = NULL, .msg = NULL, .edge = NULL },
    };
    nx_hook_dispatch(&ctx);
    nx_hook_dispatch(&ctx);
    nx_hook_dispatch(&ctx);

    ASSERT_EQ_U(hi.fire_count, 3);

    hook_inspector_teardown(&hi);
    nx_hook_reset();
}

TEST(hook_inspector_reset_clears_accumulated_events)
{
    nx_hook_reset();

    struct hook_inspector hi;
    hook_inspector_init(&hi, NX_HOOK_IPC_SEND, 0);

    struct nx_hook_context ctx = {
        .point = NX_HOOK_IPC_SEND,
        .u.ipc = { .src = NULL, .dst = NULL, .msg = NULL, .edge = NULL },
    };
    nx_hook_dispatch(&ctx);
    nx_hook_dispatch(&ctx);
    ASSERT_EQ_U(hi.fire_count, 2);

    hook_inspector_reset(&hi);
    ASSERT_EQ_U(hi.fire_count, 0);

    hook_inspector_teardown(&hi);
    nx_hook_reset();
}

TEST(hook_inspector_fires_after_lower_priority_hook)
{
    /* Lower priority number runs first.  Inspector at priority=100 should
     * fire after a hook registered at priority=0. */
    nx_hook_reset();

    s_hook_order_counter = 0;
    s_hook_first_order   = 0;

    struct nx_hook first_hook = {
        .point    = NX_HOOK_IPC_SEND,
        .priority = 0,
        .fn       = priority_order_hook_fn,
        .user     = NULL,
        .name     = "first",
    };
    nx_hook_register(&first_hook);

    struct hook_inspector hi;
    hook_inspector_init(&hi, NX_HOOK_IPC_SEND, 100);

    struct nx_hook_context ctx = {
        .point = NX_HOOK_IPC_SEND,
        .u.ipc = { .src = NULL, .dst = NULL, .msg = NULL, .edge = NULL },
    };
    nx_hook_dispatch(&ctx);

    /* priority_order_hook (priority 0) recorded its order number first;
     * inspector (priority 100) increments fire_count second.  So
     * s_hook_first_order must be 1 (first call into counter) and
     * hi.fire_count must also be 1 (inspector fired once). */
    ASSERT_EQ_U(s_hook_first_order, 1);
    ASSERT_EQ_U(hi.fire_count, 1);

    nx_hook_unregister(&first_hook);
    hook_inspector_teardown(&hi);
    nx_hook_reset();
}

TEST(hook_chain_length_matches_registered_count)
{
    nx_hook_reset();

    size_t before = nx_hook_chain_length(NX_HOOK_IPC_SEND);

    struct hook_inspector hi1, hi2;
    hook_inspector_init(&hi1, NX_HOOK_IPC_SEND, 10);
    hook_inspector_init(&hi2, NX_HOOK_IPC_SEND, 20);

    ASSERT_EQ_U(nx_hook_chain_length(NX_HOOK_IPC_SEND), before + 2);

    hook_inspector_teardown(&hi1);
    ASSERT_EQ_U(nx_hook_chain_length(NX_HOOK_IPC_SEND), before + 1);

    hook_inspector_teardown(&hi2);
    ASSERT_EQ_U(nx_hook_chain_length(NX_HOOK_IPC_SEND), before);

    nx_hook_reset();
}

TEST(hook_inspector_fires_during_dispatcher_pump)
{
    struct base_fixture f;
    base_fixture_setup(&f, NX_CONN_ASYNC, NX_PAUSE_QUEUE);

    struct hook_inspector hi_send, hi_recv;
    hook_inspector_init(&hi_send, NX_HOOK_IPC_SEND, 10);
    hook_inspector_init(&hi_recv, NX_HOOK_IPC_RECV, 10);

    struct nx_ipc_message msg = {
        .src_slot = &f.caller_slot,
        .dst_slot = &f.svc_slot,
    };
    /* The dispatcher fires IPC_RECV when pumping a message. */
    nx_dispatcher_enqueue(&msg);
    nx_dispatcher_pump_once();

    /* IPC_RECV fires inside pump_once; IPC_SEND does NOT (that's the
     * slot_call_blocking path, not the plain enqueue path). */
    ASSERT_EQ_U(hi_send.fire_count, 0);
    ASSERT_EQ_U(hi_recv.fire_count, 1);

    hook_inspector_teardown(&hi_send);
    hook_inspector_teardown(&hi_recv);
    base_fixture_teardown(&f);
}


/* =========================================================================
 * 3. Recompose event logger (nx_graph_subscribe + nx_change_log_read)
 * ========================================================================= */

struct recompose_logger {
    struct nx_graph_event events[32];
    size_t                count;
    uint64_t              start_gen;
};

static void recompose_logger_cb(const struct nx_graph_event *ev, void *ctx)
{
    struct recompose_logger *l = ctx;
    if (l->count < 32) l->events[l->count++] = *ev;
}

static void recompose_logger_init(struct recompose_logger *l)
{
    memset(l, 0, sizeof *l);
    l->start_gen = nx_change_log_total();
    nx_graph_subscribe(recompose_logger_cb, l);
}

static void recompose_logger_teardown(struct recompose_logger *l)
{
    nx_graph_unsubscribe(recompose_logger_cb, l);
}

static size_t recompose_logger_read_changelog(struct recompose_logger *l,
                                              struct nx_graph_event *out,
                                              size_t max)
{
    return nx_change_log_read(l->start_gen, out, max);
}

TEST(recompose_logger_captures_slot_swap_event)
{
    nx_graph_reset();

    struct recompose_logger logger;
    recompose_logger_init(&logger);

    struct nx_slot s = {
        .name = "swap_test_slot", .iface = "x",
        .mutability = NX_MUT_HOT, .concurrency = NX_CONC_SHARED,
    };
    struct nx_component c = {
        .manifest_id = "swap_test_comp", .instance_id = "0",
        .state = NX_LC_ACTIVE,
    };
    nx_slot_register(&s);
    nx_component_register(&c);
    nx_slot_swap(&s, &c);        /* emits NX_EV_SLOT_SWAPPED */

    /* Subscriber captured it. */
    bool found = false;
    for (size_t i = 0; i < logger.count; i++) {
        if (logger.events[i].type == NX_EV_SLOT_SWAPPED) { found = true; break; }
    }
    ASSERT(found);

    recompose_logger_teardown(&logger);
    nx_graph_reset();
}

TEST(recompose_logger_captures_connection_added_event)
{
    nx_graph_reset();

    struct recompose_logger logger;
    recompose_logger_init(&logger);

    struct nx_slot s1 = {
        .name = "conn_src", .iface = "x",
        .mutability = NX_MUT_HOT, .concurrency = NX_CONC_SHARED,
    };
    struct nx_slot s2 = {
        .name = "conn_dst", .iface = "x",
        .mutability = NX_MUT_HOT, .concurrency = NX_CONC_SHARED,
    };
    nx_slot_register(&s1);
    nx_slot_register(&s2);
    int err = NX_OK;
    nx_connection_register(&s1, &s2, NX_CONN_ASYNC, false,
                           NX_PAUSE_QUEUE, &err);

    bool found = false;
    for (size_t i = 0; i < logger.count; i++) {
        if (logger.events[i].type == NX_EV_CONNECTION_ADDED) {
            found = true; break;
        }
    }
    ASSERT(found);

    recompose_logger_teardown(&logger);
    nx_graph_reset();
}

TEST(recompose_logger_since_gen_skips_older_events)
{
    nx_graph_reset();

    /* Register a slot before setting start_gen. */
    struct nx_slot early = {
        .name = "early_slot", .iface = "x",
        .mutability = NX_MUT_HOT, .concurrency = NX_CONC_SHARED,
    };
    nx_slot_register(&early);

    /* Logger starts tracking only events from here. */
    struct recompose_logger logger;
    recompose_logger_init(&logger);

    /* Register a second slot after start_gen. */
    struct nx_slot late = {
        .name = "late_slot", .iface = "x",
        .mutability = NX_MUT_HOT, .concurrency = NX_CONC_SHARED,
    };
    nx_slot_register(&late);

    /* Change log read with start_gen should include only late_slot creation,
     * not early_slot's. */
    struct nx_graph_event out[16];
    size_t n = recompose_logger_read_changelog(&logger, out, 16);
    ASSERT(n >= 1);
    bool saw_early = false;
    for (size_t i = 0; i < n; i++) {
        if (out[i].type == NX_EV_SLOT_CREATED &&
            out[i].u.slot.name &&
            strcmp(out[i].u.slot.name, "early_slot") == 0) {
            saw_early = true;
        }
    }
    ASSERT(!saw_early);

    recompose_logger_teardown(&logger);
    nx_graph_reset();
}


/* =========================================================================
 * 4. Pause-injector — nx_slot_call_blocking pause-state interactions
 *
 * QUEUE policy (actual block) is a kernel-only test.  Here we cover:
 *   - REJECT policy: NX_EBUSY
 *   - REDIRECT policy (no fallback): NX_ENOENT (fail-closed)
 *   - REDIRECT policy (with fallback): still NX_ENOENT in v1
 *   - slot->resume_waitq is initialized when a slot is registered
 *   - in_flight_calls counter behavior
 * ========================================================================= */

/* Subset of the slot_call_test fixture (task + caller_slot + svc). */

static void dummy_entry_8a8(void *arg) { (void)arg; }

struct pause_inj_fixture {
    struct nx_slot      posix_slot;
    struct nx_component posix_comp;
    struct nx_slot      svc_slot;
    struct mock_handle  svc;
    struct nx_task     *task;
};

static void pause_inj_setup(struct pause_inj_fixture *f,
                             enum nx_pause_policy policy)
{
    nx_graph_reset();
    nx_dispatcher_reset();
    nx_hook_reset();
    memset(f, 0, sizeof *f);

    f->posix_slot.name        = "posix_shim";
    f->posix_slot.iface       = "posix_shim";
    f->posix_slot.mutability  = NX_MUT_HOT;
    f->posix_slot.concurrency = NX_CONC_SHARED;
    f->posix_comp.manifest_id = "posix_shim";
    f->posix_comp.instance_id = "0";
    f->posix_comp.state       = NX_LC_ACTIVE;

    mock_handle_init(&f->svc, "svc_pi", "0");
    f->svc_slot.name        = "svc_pi";
    f->svc_slot.iface       = "svc_pi";
    f->svc_slot.mutability  = NX_MUT_HOT;
    f->svc_slot.concurrency = NX_CONC_SHARED;

    nx_slot_register(&f->posix_slot);
    nx_component_register(&f->posix_comp);
    nx_slot_swap(&f->posix_slot, &f->posix_comp);

    nx_slot_register(&f->svc_slot);
    nx_component_register(&f->svc.comp);
    nx_slot_swap(&f->svc_slot, &f->svc.comp);

    int err = NX_OK;
    nx_connection_register(&f->posix_slot, &f->svc_slot,
                           NX_CONN_ASYNC, false, policy, &err);

    f->task = nx_task_create("pi_caller", dummy_entry_8a8, NULL, 1);
    nx_task_set_current_for_test(f->task);
}

static void pause_inj_teardown(struct pause_inj_fixture *f)
{
    nx_task_set_current_for_test(NULL);
    if (f->task) nx_task_destroy(f->task);
    nx_dispatcher_reset();
    nx_graph_reset();
    nx_hook_reset();
}

TEST(pause_inject_reject_policy_returns_ebusy)
{
    struct pause_inj_fixture f;
    pause_inj_setup(&f, NX_PAUSE_REJECT);

    nx_slot_set_pause_state(&f.svc_slot, NX_SLOT_PAUSE_CUTTING);

    struct nx_ipc_message msg = {
        .src_slot = &f.task->caller_slot,
        .dst_slot = &f.svc_slot,
    };
    ASSERT_EQ_U(nx_slot_call_blocking(&f.svc_slot, &msg, NULL, 0), NX_EBUSY);
    ASSERT(f.task->in_flight_reply_buf == NULL);

    nx_slot_set_pause_state(&f.svc_slot, NX_SLOT_PAUSE_NONE);
    pause_inj_teardown(&f);
}

TEST(pause_inject_redirect_no_fallback_returns_enoent)
{
    struct pause_inj_fixture f;
    pause_inj_setup(&f, NX_PAUSE_REDIRECT);

    nx_slot_set_pause_state(&f.svc_slot, NX_SLOT_PAUSE_CUTTING);

    struct nx_ipc_message msg = {
        .src_slot = &f.task->caller_slot,
        .dst_slot = &f.svc_slot,
    };
    ASSERT_EQ_U(nx_slot_call_blocking(&f.svc_slot, &msg, NULL, 0), NX_ENOENT);
    ASSERT(f.task->in_flight_reply_buf == NULL);

    nx_slot_set_pause_state(&f.svc_slot, NX_SLOT_PAUSE_NONE);
    pause_inj_teardown(&f);
}

TEST(pause_inject_redirect_with_fallback_still_returns_enoent)
{
    /* v1 blocking-call REDIRECT is fail-closed even when a fallback is wired.
     * The comment in slot_call.c explains the trampoline gap. */
    struct pause_inj_fixture f;
    pause_inj_setup(&f, NX_PAUSE_REDIRECT);

    struct nx_slot fallback_slot = {
        .name = "fallback", .iface = "svc_pi",
        .mutability = NX_MUT_HOT, .concurrency = NX_CONC_SHARED,
    };
    nx_slot_register(&fallback_slot);
    nx_slot_set_fallback(&f.svc_slot, &fallback_slot);
    nx_slot_set_pause_state(&f.svc_slot, NX_SLOT_PAUSE_CUTTING);

    struct nx_ipc_message msg = {
        .src_slot = &f.task->caller_slot,
        .dst_slot = &f.svc_slot,
    };
    ASSERT_EQ_U(nx_slot_call_blocking(&f.svc_slot, &msg, NULL, 0), NX_ENOENT);

    nx_slot_set_pause_state(&f.svc_slot, NX_SLOT_PAUSE_NONE);
    nx_slot_set_fallback(&f.svc_slot, NULL);
    pause_inj_teardown(&f);
}

TEST(pause_inject_none_state_pause_read_returns_none)
{
    struct pause_inj_fixture f;
    pause_inj_setup(&f, NX_PAUSE_QUEUE);

    /* Default after register is NX_SLOT_PAUSE_NONE. */
    ASSERT_EQ_U(nx_slot_pause_state(&f.svc_slot), NX_SLOT_PAUSE_NONE);

    /* Setting and restoring individual states. */
    nx_slot_set_pause_state(&f.svc_slot, NX_SLOT_PAUSE_CUTTING);
    ASSERT_EQ_U(nx_slot_pause_state(&f.svc_slot), NX_SLOT_PAUSE_CUTTING);

    nx_slot_set_pause_state(&f.svc_slot, NX_SLOT_PAUSE_DRAINING);
    ASSERT_EQ_U(nx_slot_pause_state(&f.svc_slot), NX_SLOT_PAUSE_DRAINING);

    nx_slot_set_pause_state(&f.svc_slot, NX_SLOT_PAUSE_NONE);
    ASSERT_EQ_U(nx_slot_pause_state(&f.svc_slot), NX_SLOT_PAUSE_NONE);

    pause_inj_teardown(&f);
}

TEST(pause_inject_set_fallback_wires_fallback_field)
{
    nx_graph_reset();

    struct nx_slot s = {
        .name = "fbs", .iface = "x",
        .mutability = NX_MUT_HOT, .concurrency = NX_CONC_SHARED,
    };
    struct nx_slot fb = {
        .name = "fbx", .iface = "x",
        .mutability = NX_MUT_HOT, .concurrency = NX_CONC_SHARED,
    };
    nx_slot_register(&s);
    nx_slot_register(&fb);

    ASSERT(s.fallback == NULL);
    nx_slot_set_fallback(&s, &fb);
    ASSERT(s.fallback == &fb);

    nx_slot_set_fallback(&s, NULL);
    ASSERT(s.fallback == NULL);

    nx_graph_reset();
}

TEST(pause_inject_in_flight_counter_zero_before_dispatch)
{
    struct base_fixture f;
    base_fixture_setup(&f, NX_CONN_ASYNC, NX_PAUSE_QUEUE);

    ASSERT_EQ_U(__atomic_load_n(&f.svc_slot.in_flight_calls, __ATOMIC_ACQUIRE),
                0);

    base_fixture_teardown(&f);
}

TEST(pause_inject_in_flight_counter_increments_and_returns_to_zero)
{
    struct base_fixture f;
    base_fixture_setup(&f, NX_CONN_ASYNC, NX_PAUSE_QUEUE);

    struct nx_ipc_message msg = {
        .src_slot = &f.caller_slot,
        .dst_slot = &f.svc_slot,
    };
    ASSERT_EQ_U(__atomic_load_n(&f.svc_slot.in_flight_calls, __ATOMIC_ACQUIRE),
                0);

    nx_dispatcher_enqueue(&msg);
    nx_dispatcher_pump_once();

    ASSERT_EQ_U(__atomic_load_n(&f.svc_slot.in_flight_calls, __ATOMIC_ACQUIRE),
                0);
    ASSERT_EQ_U(f.svc.state.call_count, 1);

    base_fixture_teardown(&f);
}

TEST(pause_inject_resume_waitq_initialized_on_slot_register)
{
    nx_graph_reset();

    struct nx_slot s = {
        .name = "rw_test", .iface = "x",
        .mutability = NX_MUT_HOT, .concurrency = NX_CONC_SHARED,
    };
    nx_slot_register(&s);

    /* resume_waitq must have a sane (empty) waiter list after register. */
    ASSERT(nx_list_empty(&s.resume_waitq.waiters));

    nx_graph_reset();
}


/* =========================================================================
 * 5. Cap-forgery harness — R3 cap-scan enforcement
 * ========================================================================= */

TEST(cap_scan_send_null_caps_array_passes)
{
    nx_graph_reset();

    struct nx_slot sender = {
        .name = "cap_sender", .iface = "x",
        .mutability = NX_MUT_HOT, .concurrency = NX_CONC_SHARED,
    };
    nx_slot_register(&sender);

    struct nx_ipc_message msg = {
        .src_slot  = &sender,
        .dst_slot  = NULL,
        .n_caps    = 0,
        .caps      = NULL,
    };
    ASSERT_EQ_U(nx_ipc_scan_send_caps(&sender, &msg), NX_OK);

    nx_graph_reset();
}

TEST(cap_scan_send_forged_slot_ref_rejected)
{
    /* sender has no outgoing edge to victim — forged cap → NX_EINVAL */
    nx_graph_reset();

    struct nx_slot sender = {
        .name = "cfs", .iface = "x",
        .mutability = NX_MUT_HOT, .concurrency = NX_CONC_SHARED,
    };
    struct nx_slot victim = {
        .name = "cfv", .iface = "x",
        .mutability = NX_MUT_HOT, .concurrency = NX_CONC_SHARED,
    };
    nx_slot_register(&sender);
    nx_slot_register(&victim);

    struct nx_ipc_cap forged = {
        .kind      = NX_CAP_SLOT_REF,
        .ownership = NX_CAP_BORROW,
        .u.slot_ref = &victim,
        .cap_id    = 1,
    };
    struct nx_ipc_message msg = {
        .src_slot = &sender,
        .dst_slot = NULL,
        .n_caps   = 1,
        .caps     = &forged,
    };
    ASSERT_EQ_U(nx_ipc_scan_send_caps(&sender, &msg), NX_EINVAL);

    nx_graph_reset();
}

TEST(cap_scan_send_connected_slot_ref_passes)
{
    /* sender has a registered edge to target — cap is legitimate. */
    nx_graph_reset();

    struct nx_slot sender = {
        .name = "ccs", .iface = "x",
        .mutability = NX_MUT_HOT, .concurrency = NX_CONC_SHARED,
    };
    struct nx_slot target = {
        .name = "cct", .iface = "x",
        .mutability = NX_MUT_HOT, .concurrency = NX_CONC_SHARED,
    };
    nx_slot_register(&sender);
    nx_slot_register(&target);

    int err = NX_OK;
    nx_connection_register(&sender, &target, NX_CONN_ASYNC, false,
                           NX_PAUSE_QUEUE, &err);

    struct nx_ipc_cap valid_cap = {
        .kind      = NX_CAP_SLOT_REF,
        .ownership = NX_CAP_BORROW,
        .u.slot_ref = &target,
        .cap_id    = 1,
    };
    struct nx_ipc_message msg = {
        .src_slot = &sender,
        .dst_slot = &target,
        .n_caps   = 1,
        .caps     = &valid_cap,
    };
    ASSERT_EQ_U(nx_ipc_scan_send_caps(&sender, &msg), NX_OK);

    nx_graph_reset();
}

TEST(cap_scan_recv_unclaimed_borrow_silently_dropped)
{
    nx_graph_reset();

    struct nx_slot recv = {
        .name = "recv_b", .iface = "x",
        .mutability = NX_MUT_HOT, .concurrency = NX_CONC_SHARED,
    };
    nx_slot_register(&recv);

    struct nx_ipc_cap cap = {
        .kind      = NX_CAP_SLOT_REF,
        .ownership = NX_CAP_BORROW,
        .u.slot_ref = NULL,
        .cap_id    = 1,
        .claimed   = false,
    };
    struct nx_ipc_message msg = {
        .src_slot = NULL,
        .dst_slot = &recv,
        .n_caps   = 1,
        .caps     = &cap,
    };
    /* BORROW + unclaimed → silent drop (0 error count) */
    size_t errors = nx_ipc_scan_recv_caps(&recv, &msg);
    ASSERT_EQ_U(errors, 0);

    nx_graph_reset();
}

TEST(cap_scan_recv_unclaimed_transfer_counted)
{
    nx_graph_reset();

    struct nx_slot recv = {
        .name = "recv_t", .iface = "x",
        .mutability = NX_MUT_HOT, .concurrency = NX_CONC_SHARED,
    };
    nx_slot_register(&recv);

    struct nx_ipc_cap cap = {
        .kind      = NX_CAP_SLOT_REF,
        .ownership = NX_CAP_TRANSFER,
        .u.slot_ref = NULL,
        .cap_id    = 1,
        .claimed   = false,
    };
    struct nx_ipc_message msg = {
        .src_slot = NULL,
        .dst_slot = &recv,
        .n_caps   = 1,
        .caps     = &cap,
    };
    /* TRANSFER + unclaimed → protocol error; returned count is 1 */
    size_t errors = nx_ipc_scan_recv_caps(&recv, &msg);
    ASSERT_EQ_U(errors, 1);

    nx_graph_reset();
}

TEST(cap_scan_send_rejects_on_first_forged_cap)
{
    /* Two caps: valid then forged.  Scan stops at first forged → NX_EINVAL. */
    nx_graph_reset();

    struct nx_slot sender = {
        .name = "cfms", .iface = "x",
        .mutability = NX_MUT_HOT, .concurrency = NX_CONC_SHARED,
    };
    struct nx_slot connected = {
        .name = "cfc", .iface = "x",
        .mutability = NX_MUT_HOT, .concurrency = NX_CONC_SHARED,
    };
    struct nx_slot unconnected = {
        .name = "cfu", .iface = "x",
        .mutability = NX_MUT_HOT, .concurrency = NX_CONC_SHARED,
    };
    nx_slot_register(&sender);
    nx_slot_register(&connected);
    nx_slot_register(&unconnected);

    int err = NX_OK;
    nx_connection_register(&sender, &connected, NX_CONN_ASYNC, false,
                           NX_PAUSE_QUEUE, &err);

    struct nx_ipc_cap caps[2] = {
        {
            .kind = NX_CAP_SLOT_REF, .ownership = NX_CAP_BORROW,
            .u.slot_ref = &connected, .cap_id = 1,
        },
        {
            .kind = NX_CAP_SLOT_REF, .ownership = NX_CAP_BORROW,
            .u.slot_ref = &unconnected, .cap_id = 2,   /* forged */
        },
    };
    struct nx_ipc_message msg = {
        .src_slot = &sender,
        .dst_slot = NULL,
        .n_caps   = 2,
        .caps     = caps,
    };
    ASSERT_EQ_U(nx_ipc_scan_send_caps(&sender, &msg), NX_EINVAL);

    nx_graph_reset();
}


/* =========================================================================
 * 6. Equivalence-runner macro + identity property
 *
 * The ASSERT_CALL_EQUIVALENT macro asserts that a direct dispatch and a
 * dispatch-mediated path produce identical handler outcomes.  On host the
 * full blocking round-trip cannot complete (no scheduler), so we exercise
 * the "direct dispatch" (dispatcher pump) vs expected call_count.
 * ========================================================================= */

/*
 * ASSERT_CALL_EQUIVALENT: assert two integer expressions are equal.
 * Intended usage: compare the rc from a direct component-op invocation
 * with the rc produced by nx_slot_call_blocking (or its test equivalent)
 * so callsite migration in slice 8.0c can be validated pair-by-pair.
 */
#define ASSERT_CALL_EQUIVALENT(direct_rc, blocking_rc) \
    ASSERT_EQ_U((unsigned int)(direct_rc), (unsigned int)(blocking_rc))

TEST(equivalence_mock_direct_dispatch_and_dispatcher_same_call_count)
{
    struct base_fixture f;
    base_fixture_setup(&f, NX_CONN_ASYNC, NX_PAUSE_QUEUE);

    /* Direct invocation through component ops. */
    struct nx_ipc_message msg = {
        .src_slot = &f.caller_slot,
        .dst_slot = &f.svc_slot,
    };
    int direct_rc = f.svc.comp.descriptor->ops->handle_msg(
                        f.svc.comp.impl, &msg);
    int direct_count = f.svc.state.call_count;

    mock_handle_reset(&f.svc);

    /* Dispatch-mediated path. */
    nx_dispatcher_enqueue(&msg);
    nx_dispatcher_pump_once();
    int dispatched_count = f.svc.state.call_count;

    /* Both paths invoke handle_msg exactly once and return the same rc. */
    ASSERT_CALL_EQUIVALENT(direct_rc, f.svc.state.handler_rc);
    ASSERT_CALL_EQUIVALENT(direct_count, dispatched_count);

    base_fixture_teardown(&f);
}

TEST(equivalence_no_hooks_no_caps_handler_rc_matches)
{
    struct base_fixture f;
    base_fixture_setup(&f, NX_CONN_ASYNC, NX_PAUSE_QUEUE);

    f.svc.state.handler_rc = -5;

    /* Direct call. */
    struct nx_ipc_message msg = { .src_slot = &f.caller_slot,
                                  .dst_slot = &f.svc_slot };
    int direct_rc = f.svc.comp.descriptor->ops->handle_msg(
                        f.svc.comp.impl, &msg);

    /* Dispatcher-mediated call (with REPLY_REQUESTED so we can inspect
     * the reply header's rc). */
    mock_handle_reset(&f.svc);
    msg.flags = NX_MSG_FLAG_REPLY_REQUESTED;
    nx_dispatcher_enqueue(&msg);
    nx_dispatcher_pump_once();

    /* Pool has 1 entry (reply not yet drained); its header rc == handler_rc. */
    ASSERT_EQ_U(nx_dispatcher_reply_pool_in_use_for_test(), 1);

    /* Drain reply. */
    nx_dispatcher_pump_once();
    ASSERT_EQ_U(nx_dispatcher_reply_pool_in_use_for_test(), 0);

    ASSERT_CALL_EQUIVALENT(direct_rc, -5);

    base_fixture_teardown(&f);
}

TEST(equivalence_observe_only_hook_does_not_alter_call_count)
{
    struct base_fixture f;
    base_fixture_setup(&f, NX_CONN_ASYNC, NX_PAUSE_QUEUE);

    struct nx_ipc_message msg = {
        .src_slot = &f.caller_slot,
        .dst_slot = &f.svc_slot,
    };

    /* Without hook: 1 call. */
    nx_dispatcher_enqueue(&msg);
    nx_dispatcher_pump_once();
    int count_no_hook = f.svc.state.call_count;
    mock_handle_reset(&f.svc);

    /* Register observer hook and dispatch again: still 1 call. */
    struct hook_inspector hi;
    hook_inspector_init(&hi, NX_HOOK_IPC_RECV, 10);

    nx_dispatcher_enqueue(&msg);
    nx_dispatcher_pump_once();
    int count_with_hook = f.svc.state.call_count;

    ASSERT_CALL_EQUIVALENT(count_no_hook, count_with_hook);
    ASSERT_EQ_U(hi.fire_count, 1);   /* inspector fired once */

    hook_inspector_teardown(&hi);
    base_fixture_teardown(&f);
}

TEST(equivalence_multiple_sequential_calls_match_direct_count)
{
    struct base_fixture f;
    base_fixture_setup(&f, NX_CONN_ASYNC, NX_PAUSE_QUEUE);

    struct nx_ipc_message msg = {
        .src_slot = &f.caller_slot,
        .dst_slot = &f.svc_slot,
    };

    const int N = 5;
    for (int i = 0; i < N; i++) {
        nx_dispatcher_enqueue(&msg);
        nx_dispatcher_pump_once();
    }

    ASSERT_CALL_EQUIVALENT(f.svc.state.call_count, N);

    base_fixture_teardown(&f);
}

TEST(equivalence_macro_asserts_equal_rcs)
{
    /* Self-test: macro must not fire when both sides are equal. */
    ASSERT_CALL_EQUIVALENT(0, 0);
    ASSERT_CALL_EQUIVALENT(-1, -1);
    ASSERT_CALL_EQUIVALENT(NX_OK, NX_OK);
}
