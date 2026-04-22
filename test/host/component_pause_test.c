#include "test_runner.h"
#include "framework/component.h"
#include "framework/hook.h"
#include "framework/ipc.h"
#include "framework/registry.h"

#include <time.h>

/*
 * Host-side tests for the slice-3.8 pause protocol and the IPC
 * router's pause-policy handling.
 *
 * Every test starts with a clean graph / IPC state / hook state so
 * fixtures don't bleed between cases.  A helper builds a one-edge
 * fixture (sender → receiver) with a configurable mode + policy.
 */

/* --- Receiver component fixture ---------------------------------------- */

struct rx_state {
    int                     handle_calls;
    int                     pause_hook_calls;
    int                     pause_calls;
    int                     resume_calls;
    struct nx_ipc_message  *last_msg;
};

static int rx_handle(void *self, struct nx_ipc_message *msg)
{
    struct rx_state *s = self;
    s->handle_calls++;
    s->last_msg = msg;
    return NX_OK;
}

static int rx_pause_hook(void *self)
{
    struct rx_state *s = self;
    s->pause_hook_calls++;
    return NX_OK;
}

static int rx_pause(void *self)
{
    struct rx_state *s = self;
    s->pause_calls++;
    return NX_OK;
}

static int rx_resume(void *self)
{
    struct rx_state *s = self;
    s->resume_calls++;
    return NX_OK;
}

static const struct nx_component_ops rx_ops = {
    .handle_msg = rx_handle,
    .pause_hook = rx_pause_hook,
    .pause      = rx_pause,
    .resume     = rx_resume,
};

static const struct nx_component_descriptor rx_desc = {
    .name        = "rx",
    .state_size  = sizeof(struct rx_state),
    .deps_offset = 0,
    .deps        = NULL,
    .n_deps      = 0,
    .ops         = &rx_ops,
};

/* Dispatcher-only component — no pause_hook. */
static const struct nx_component_ops rx_ops_no_hook = {
    .handle_msg = rx_handle,
    .pause      = rx_pause,
    .resume     = rx_resume,
};

static const struct nx_component_descriptor rx_desc_no_hook = {
    .name        = "rx_no_hook",
    .state_size  = sizeof(struct rx_state),
    .deps_offset = 0,
    .deps        = NULL,
    .n_deps      = 0,
    .ops         = &rx_ops_no_hook,
};

struct pause_fixture {
    struct nx_slot         sender;
    struct nx_slot         receiver;
    struct nx_component    sender_comp;
    struct nx_component    receiver_comp;
    struct rx_state        rx;
    struct nx_connection  *edge;
};

static void fixture_init(struct pause_fixture *f,
                         enum nx_conn_mode mode,
                         enum nx_pause_policy policy,
                         const struct nx_component_descriptor *desc)
{
    nx_graph_reset();
    nx_ipc_reset();
    nx_hook_reset();

    memset(f, 0, sizeof *f);
    f->sender   = (struct nx_slot){ .name = "sender",   .iface = "x" };
    f->receiver = (struct nx_slot){ .name = "receiver", .iface = "x" };
    f->sender_comp   = (struct nx_component){
        .manifest_id = "sender_m", .instance_id = "s0" };
    f->receiver_comp = (struct nx_component){
        .manifest_id = "rx_m", .instance_id = "r0",
        .impl        = &f->rx,
        .descriptor  = desc,
    };

    nx_slot_register(&f->sender);
    nx_slot_register(&f->receiver);
    nx_component_register(&f->sender_comp);
    nx_component_register(&f->receiver_comp);
    nx_slot_swap(&f->sender,   &f->sender_comp);
    nx_slot_swap(&f->receiver, &f->receiver_comp);

    /* Drive the receiver to ACTIVE so pause is legal. */
    nx_component_init(&f->receiver_comp);
    nx_component_enable(&f->receiver_comp);

    int err = NX_OK;
    f->edge = nx_connection_register(&f->sender, &f->receiver,
                                     mode, false, policy, &err);
}

/* --- Pause protocol ---------------------------------------------------- */

TEST(pause_drives_slot_through_cutting_draining_done)
{
    struct pause_fixture f;
    fixture_init(&f, NX_CONN_SYNC, NX_PAUSE_QUEUE, &rx_desc);

    ASSERT_EQ_U(nx_slot_pause_state(&f.receiver), NX_SLOT_PAUSE_NONE);
    ASSERT_EQ_U(nx_component_pause(&f.receiver_comp), NX_OK);

    /* After return: slot's pause state is DONE, hook + pause ops ran. */
    ASSERT_EQ_U(nx_slot_pause_state(&f.receiver), NX_SLOT_PAUSE_DONE);
    ASSERT_EQ_U(f.rx.pause_hook_calls, 1);
    ASSERT_EQ_U(f.rx.pause_calls, 1);
    ASSERT_EQ_U(f.receiver_comp.state, NX_LC_PAUSED);
}

TEST(resume_clears_pause_state_and_calls_ops_resume)
{
    struct pause_fixture f;
    fixture_init(&f, NX_CONN_SYNC, NX_PAUSE_QUEUE, &rx_desc);

    nx_component_pause(&f.receiver_comp);
    ASSERT_EQ_U(nx_slot_pause_state(&f.receiver), NX_SLOT_PAUSE_DONE);

    ASSERT_EQ_U(nx_component_resume(&f.receiver_comp), NX_OK);
    ASSERT_EQ_U(f.rx.resume_calls, 1);
    ASSERT_EQ_U(nx_slot_pause_state(&f.receiver), NX_SLOT_PAUSE_NONE);
    ASSERT_EQ_U(f.receiver_comp.state, NX_LC_ACTIVE);
}

/* --- NX_PAUSE_QUEUE: held messages wait for resume -------------------- */

TEST(paused_queue_policy_holds_sends_and_flushes_on_resume)
{
    struct pause_fixture f;
    fixture_init(&f, NX_CONN_ASYNC, NX_PAUSE_QUEUE, &rx_desc);

    nx_component_pause(&f.receiver_comp);

    struct nx_ipc_message m1 = {
        .src_slot = &f.sender, .dst_slot = &f.receiver, .msg_type = 1 };
    struct nx_ipc_message m2 = {
        .src_slot = &f.sender, .dst_slot = &f.receiver, .msg_type = 2 };

    ASSERT_EQ_U(nx_ipc_send(&m1), NX_OK);
    ASSERT_EQ_U(nx_ipc_send(&m2), NX_OK);
    /* Nothing in the live inbox — they're on the hold queue. */
    ASSERT_EQ_U(nx_ipc_inbox_depth(&f.receiver), 0);
    ASSERT_EQ_U(nx_ipc_hold_queue_depth(&f.sender, &f.receiver), 2);
    ASSERT_EQ_U(f.rx.handle_calls, 0);

    nx_component_resume(&f.receiver_comp);

    /* After resume: hold queue drained back through the router.  Since
     * the edge is ASYNC, messages are now in the inbox in FIFO order. */
    ASSERT_EQ_U(nx_ipc_hold_queue_depth(&f.sender, &f.receiver), 0);
    ASSERT_EQ_U(nx_ipc_inbox_depth(&f.receiver), 2);
    ASSERT_EQ_U(nx_ipc_dispatch(&f.receiver, 10), 2);
    ASSERT_EQ_U(f.rx.handle_calls, 2);
}

/* --- NX_PAUSE_REJECT: paused sends fail ------------------------------- */

TEST(paused_reject_policy_returns_ebusy)
{
    struct pause_fixture f;
    fixture_init(&f, NX_CONN_SYNC, NX_PAUSE_REJECT, &rx_desc);

    nx_component_pause(&f.receiver_comp);

    struct nx_ipc_message m = {
        .src_slot = &f.sender, .dst_slot = &f.receiver };
    ASSERT_EQ_U(nx_ipc_send(&m), NX_EBUSY);
    ASSERT_EQ_U(f.rx.handle_calls, 0);
    ASSERT_EQ_U(nx_ipc_inbox_depth(&f.receiver), 0);
}

/* --- NX_PAUSE_REDIRECT: routed to fallback ---------------------------- */

/* Second receiver — fallback target for the REDIRECT test. */
struct fb_rx_state { int calls; };
static int fb_handle(void *self, struct nx_ipc_message *msg)
{
    (void)msg;
    ((struct fb_rx_state *)self)->calls++;
    return NX_OK;
}
static const struct nx_component_ops fb_ops = { .handle_msg = fb_handle };
static const struct nx_component_descriptor fb_desc = {
    .name = "fb_rx", .state_size = sizeof(struct fb_rx_state),
    .ops  = &fb_ops,
};

TEST(paused_redirect_policy_routes_to_fallback_slot)
{
    struct pause_fixture f;
    fixture_init(&f, NX_CONN_SYNC, NX_PAUSE_REDIRECT, &rx_desc);

    /* Install a fallback receiver and an edge from sender → fallback so
     * the redirected send has a real route. */
    struct nx_slot       fb_slot = { .name = "fb", .iface = "x" };
    struct nx_component  fb_comp = {
        .manifest_id = "fb_m", .instance_id = "f0",
        .descriptor  = &fb_desc,
    };
    struct fb_rx_state fbs = { 0 };
    fb_comp.impl = &fbs;

    nx_slot_register(&fb_slot);
    nx_component_register(&fb_comp);
    nx_slot_swap(&fb_slot, &fb_comp);
    int err = NX_OK;
    nx_connection_register(&f.sender, &fb_slot,
                           NX_CONN_SYNC, false, NX_PAUSE_QUEUE, &err);
    ASSERT_EQ_U(nx_slot_set_fallback(&f.receiver, &fb_slot), NX_OK);

    nx_component_pause(&f.receiver_comp);

    struct nx_ipc_message m = {
        .src_slot = &f.sender, .dst_slot = &f.receiver };
    ASSERT_EQ_U(nx_ipc_send(&m), NX_OK);
    ASSERT_EQ_U(fbs.calls, 1);         /* fallback handler ran */
    ASSERT_EQ_U(f.rx.handle_calls, 0); /* primary handler did not */
    /* msg reflects final hop (documented). */
    ASSERT_EQ_PTR(m.dst_slot, &fb_slot);
}

TEST(paused_redirect_without_fallback_returns_enoent)
{
    struct pause_fixture f;
    fixture_init(&f, NX_CONN_SYNC, NX_PAUSE_REDIRECT, &rx_desc);
    /* No fallback set — fail closed. */
    nx_component_pause(&f.receiver_comp);

    struct nx_ipc_message m = {
        .src_slot = &f.sender, .dst_slot = &f.receiver };
    ASSERT_EQ_U(nx_ipc_send(&m), NX_ENOENT);
}

TEST(paused_redirect_loop_returns_eloop)
{
    nx_graph_reset(); nx_ipc_reset(); nx_hook_reset();

    /* Scenario: sender has edges to both A and B.  A is paused with
     * REDIRECT.fallback=B; B is paused with REDIRECT.fallback=A.  A
     * send from sender → A redirects to B, which redirects back to A,
     * which redirects to B … the depth guard has to fire before the
     * stack unwinds naturally.  The edge lookups all use msg->src_slot
     * (= sender) on every recursion, so sender needs edges to both
     * targets for the redirect chain to reach deep enough to trip the
     * guard. */
    struct nx_slot sender = { .name = "sender", .iface = "x" };
    struct nx_slot a      = { .name = "a",      .iface = "x" };
    struct nx_slot b      = { .name = "b",      .iface = "x" };
    nx_slot_register(&sender);
    nx_slot_register(&a);
    nx_slot_register(&b);

    struct rx_state a_state = { 0 };
    struct rx_state b_state = { 0 };
    struct nx_component a_comp = {
        .manifest_id = "a_m", .instance_id = "0",
        .descriptor  = &rx_desc, .impl = &a_state,
    };
    struct nx_component b_comp = {
        .manifest_id = "b_m", .instance_id = "0",
        .descriptor  = &rx_desc, .impl = &b_state,
    };
    nx_component_register(&a_comp);
    nx_component_register(&b_comp);
    nx_slot_swap(&a, &a_comp);
    nx_slot_swap(&b, &b_comp);
    nx_component_init(&a_comp); nx_component_enable(&a_comp);
    nx_component_init(&b_comp); nx_component_enable(&b_comp);

    int err = NX_OK;
    nx_connection_register(&sender, &a, NX_CONN_SYNC, false,
                           NX_PAUSE_REDIRECT, &err);
    nx_connection_register(&sender, &b, NX_CONN_SYNC, false,
                           NX_PAUSE_REDIRECT, &err);
    ASSERT_EQ_U(nx_slot_set_fallback(&a, &b), NX_OK);
    ASSERT_EQ_U(nx_slot_set_fallback(&b, &a), NX_OK);

    nx_component_pause(&a_comp);
    nx_component_pause(&b_comp);

    struct nx_ipc_message m = { .src_slot = &sender, .dst_slot = &a };
    ASSERT_EQ_U(nx_ipc_send(&m), NX_ELOOP);
}

/* --- IPC_SEND / IPC_RECV hooks ---------------------------------------- */

static int g_ipc_send_hook_calls;
static int g_ipc_recv_hook_calls;

static enum nx_hook_action count_ipc_send(struct nx_hook_context *ctx,
                                          void *user)
{
    (void)ctx; (void)user;
    g_ipc_send_hook_calls++;
    return NX_HOOK_CONTINUE;
}

static enum nx_hook_action count_ipc_recv(struct nx_hook_context *ctx,
                                          void *user)
{
    (void)ctx; (void)user;
    g_ipc_recv_hook_calls++;
    return NX_HOOK_CONTINUE;
}

TEST(ipc_send_hook_fires_on_every_send)
{
    struct pause_fixture f;
    fixture_init(&f, NX_CONN_SYNC, NX_PAUSE_QUEUE, &rx_desc);

    g_ipc_send_hook_calls = 0;
    struct nx_hook h = {
        .point = NX_HOOK_IPC_SEND, .fn = count_ipc_send,
    };
    nx_hook_register(&h);

    struct nx_ipc_message m = {
        .src_slot = &f.sender, .dst_slot = &f.receiver };
    nx_ipc_send(&m);
    nx_ipc_send(&m);

    ASSERT_EQ_U(g_ipc_send_hook_calls, 2);
    ASSERT_EQ_U(f.rx.handle_calls, 2);
    nx_hook_unregister(&h);
}

static enum nx_hook_action ipc_send_abort_stub(struct nx_hook_context *ctx,
                                               void *user)
{
    (void)ctx; (void)user;
    return NX_HOOK_ABORT;
}

TEST(ipc_send_hook_abort_prevents_routing)
{
    struct pause_fixture f;
    fixture_init(&f, NX_CONN_SYNC, NX_PAUSE_QUEUE, &rx_desc);

    struct nx_hook h = {
        .point = NX_HOOK_IPC_SEND, .fn = ipc_send_abort_stub,
    };
    nx_hook_register(&h);

    struct nx_ipc_message m = {
        .src_slot = &f.sender, .dst_slot = &f.receiver };
    ASSERT_EQ_U(nx_ipc_send(&m), NX_EABORT);
    ASSERT_EQ_U(f.rx.handle_calls, 0);
    nx_hook_unregister(&h);
}

TEST(ipc_recv_hook_fires_per_dispatched_message)
{
    struct pause_fixture f;
    fixture_init(&f, NX_CONN_ASYNC, NX_PAUSE_QUEUE, &rx_desc);

    g_ipc_recv_hook_calls = 0;
    struct nx_hook h = {
        .point = NX_HOOK_IPC_RECV, .fn = count_ipc_recv,
    };
    nx_hook_register(&h);

    struct nx_ipc_message m1 = {
        .src_slot = &f.sender, .dst_slot = &f.receiver, .msg_type = 1 };
    struct nx_ipc_message m2 = {
        .src_slot = &f.sender, .dst_slot = &f.receiver, .msg_type = 2 };
    nx_ipc_send(&m1);
    nx_ipc_send(&m2);
    nx_ipc_dispatch(&f.receiver, 10);

    ASSERT_EQ_U(g_ipc_recv_hook_calls, 2);
    ASSERT_EQ_U(f.rx.handle_calls, 2);
    nx_hook_unregister(&h);
}

/* --- Lifecycle hooks fire around verbs -------------------------------- */

static int g_enable_hook_calls;
static int g_pause_hook_point_calls;
static int g_resume_hook_point_calls;
static int g_disable_hook_calls;

static enum nx_hook_action count_enable(struct nx_hook_context *ctx, void *u)
{ (void)ctx; (void)u; g_enable_hook_calls++; return NX_HOOK_CONTINUE; }

static enum nx_hook_action count_pause(struct nx_hook_context *ctx, void *u)
{ (void)ctx; (void)u; g_pause_hook_point_calls++; return NX_HOOK_CONTINUE; }

static enum nx_hook_action count_resume(struct nx_hook_context *ctx, void *u)
{ (void)ctx; (void)u; g_resume_hook_point_calls++; return NX_HOOK_CONTINUE; }

static enum nx_hook_action count_disable(struct nx_hook_context *ctx, void *u)
{ (void)ctx; (void)u; g_disable_hook_calls++; return NX_HOOK_CONTINUE; }

TEST(lifecycle_hooks_fire_around_every_verb)
{
    struct pause_fixture f;
    fixture_init(&f, NX_CONN_SYNC, NX_PAUSE_QUEUE, &rx_desc);

    g_enable_hook_calls = g_pause_hook_point_calls =
        g_resume_hook_point_calls = g_disable_hook_calls = 0;

    struct nx_hook he = {
        .point = NX_HOOK_COMPONENT_ENABLE, .fn = count_enable };
    struct nx_hook hp = {
        .point = NX_HOOK_COMPONENT_PAUSE, .fn = count_pause };
    struct nx_hook hr = {
        .point = NX_HOOK_COMPONENT_RESUME, .fn = count_resume };
    struct nx_hook hd = {
        .point = NX_HOOK_COMPONENT_DISABLE, .fn = count_disable };
    nx_hook_register(&he);
    nx_hook_register(&hp);
    nx_hook_register(&hr);
    nx_hook_register(&hd);

    /* The fixture already ran enable once before we registered the
     * hook, so start from ACTIVE.  Exercise pause / resume / disable. */
    nx_component_pause(&f.receiver_comp);
    nx_component_resume(&f.receiver_comp);
    nx_component_disable(&f.receiver_comp);
    nx_component_enable(&f.receiver_comp);

    ASSERT_EQ_U(g_pause_hook_point_calls, 1);
    ASSERT_EQ_U(g_resume_hook_point_calls, 1);
    ASSERT_EQ_U(g_disable_hook_calls, 1);
    ASSERT_EQ_U(g_enable_hook_calls, 1);

    nx_hook_unregister(&he);
    nx_hook_unregister(&hp);
    nx_hook_unregister(&hr);
    nx_hook_unregister(&hd);
}

static int g_failing_pause_hook_calls;

static int failing_pause_hook(void *self)
{
    (void)self;
    g_failing_pause_hook_calls++;
    return NX_EINVAL;
}

static const struct nx_component_ops failing_ops = {
    .handle_msg = rx_handle,
    .pause_hook = failing_pause_hook,
    .pause      = rx_pause,
};

static const struct nx_component_descriptor failing_desc = {
    .name        = "failing",
    .state_size  = sizeof(struct rx_state),
    .ops         = &failing_ops,
};

TEST(pause_hook_returning_error_aborts_transition)
{
    nx_graph_reset(); nx_ipc_reset(); nx_hook_reset();
    g_failing_pause_hook_calls = 0;

    struct rx_state     failing_state = { 0 };
    struct nx_slot      s = { .name = "s", .iface = "x" };
    struct nx_component c = {
        .manifest_id = "f", .instance_id = "0",
        .descriptor  = &failing_desc,
        .impl        = &failing_state,
    };
    nx_slot_register(&s);
    nx_component_register(&c);
    nx_slot_swap(&s, &c);
    nx_component_init(&c);
    nx_component_enable(&c);

    int rc = nx_component_pause(&c);
    ASSERT_EQ_U(rc, NX_EINVAL);
    ASSERT_EQ_U(g_failing_pause_hook_calls, 1);
    ASSERT_EQ_U(c.state, NX_LC_ACTIVE);
    /* Slice 3.9b.2: the bound slot's pause_state is rolled back to NONE
     * so the IPC router treats this component as live again. */
    ASSERT_EQ_U(nx_slot_pause_state(&s), NX_SLOT_PAUSE_NONE);
}

TEST(pause_on_component_without_pause_hook_still_works)
{
    /* Dispatcher-only component: ops->pause_hook is NULL.  Protocol
     * must skip the hook call and still reach DONE. */
    struct pause_fixture f;
    fixture_init(&f, NX_CONN_SYNC, NX_PAUSE_QUEUE, &rx_desc_no_hook);

    ASSERT_EQ_U(nx_component_pause(&f.receiver_comp), NX_OK);
    ASSERT_EQ_U(f.rx.pause_hook_calls, 0);
    ASSERT_EQ_U(f.rx.pause_calls, 1);
    ASSERT_EQ_U(nx_slot_pause_state(&f.receiver), NX_SLOT_PAUSE_DONE);
}

TEST(pause_on_unbound_component_still_transitions)
{
    /* A component registered + initialised + enabled but never bound
     * to a slot.  pause should run with zero bound slots — the verb
     * still succeeds so recomposition can progress. */
    nx_graph_reset(); nx_ipc_reset(); nx_hook_reset();
    struct nx_component c = {
        .manifest_id = "unb", .instance_id = "0",
        .descriptor  = &rx_desc,
    };
    struct rx_state s = { 0 };
    c.impl = &s;
    nx_component_register(&c);
    nx_component_init(&c);
    nx_component_enable(&c);

    ASSERT_EQ_U(nx_component_pause(&c), NX_OK);
    ASSERT_EQ_U(s.pause_hook_calls, 1);
    ASSERT_EQ_U(s.pause_calls, 1);
    ASSERT_EQ_U(c.state, NX_LC_PAUSED);
}

/* --- Slice 3.9b.2: pause-failure rollback replays held messages ------- */

TEST(pause_hook_failure_rolls_back_and_replays_held_messages)
{
    /* Setup: sender → receiver, ASYNC + QUEUE policy.  Pause the
     * receiver with a failing pause_hook.  During the partial pause
     * (after cutoff but before the failing hook), a send from the
     * outer test code lands on the hold queue.  On rollback, the slot
     * returns to NONE and the held message replays to the live
     * inbox so the handler can drain it. */
    nx_graph_reset(); nx_ipc_reset(); nx_hook_reset();
    g_failing_pause_hook_calls = 0;

    struct nx_slot sender = { .name = "sender", .iface = "x" };
    struct nx_slot rcv    = { .name = "rcv",    .iface = "x" };
    struct nx_component sender_comp = {
        .manifest_id = "s_m", .instance_id = "0" };
    struct rx_state rxs = { 0 };
    struct nx_component rcv_comp = {
        .manifest_id = "f", .instance_id = "0",
        .descriptor  = &failing_desc,
        .impl        = &rxs,
    };
    nx_slot_register(&sender);
    nx_slot_register(&rcv);
    nx_component_register(&sender_comp);
    nx_component_register(&rcv_comp);
    nx_slot_swap(&sender, &sender_comp);
    nx_slot_swap(&rcv,    &rcv_comp);
    nx_component_init(&rcv_comp);
    nx_component_enable(&rcv_comp);
    int err = NX_OK;
    nx_connection_register(&sender, &rcv, NX_CONN_ASYNC, false,
                           NX_PAUSE_QUEUE, &err);

    /* Stage a "held" message by putting the slot into CUTTING manually
     * and pushing one message — simulates the race window where a
     * sender raced with the failing pause. */
    nx_slot_set_pause_state(&rcv, NX_SLOT_PAUSE_CUTTING);
    struct nx_ipc_message m = {
        .src_slot = &sender, .dst_slot = &rcv, .msg_type = 7 };
    ASSERT_EQ_U(nx_ipc_send(&m), NX_OK);
    ASSERT_EQ_U(nx_ipc_hold_queue_depth(&sender, &rcv), 1);
    nx_slot_set_pause_state(&rcv, NX_SLOT_PAUSE_NONE);

    /* Now the real pause — which will fail — races with that held msg. */
    int rc = nx_component_pause(&rcv_comp);
    ASSERT_EQ_U(rc, NX_EINVAL);
    ASSERT_EQ_U(g_failing_pause_hook_calls, 1);
    ASSERT_EQ_U(rcv_comp.state, NX_LC_ACTIVE);
    ASSERT_EQ_U(nx_slot_pause_state(&rcv), NX_SLOT_PAUSE_NONE);
    /* The held message replayed through nx_ipc_send on rollback; ASYNC
     * edge so it now sits in the live inbox. */
    ASSERT_EQ_U(nx_ipc_hold_queue_depth(&sender, &rcv), 0);
    ASSERT_EQ_U(nx_ipc_inbox_depth(&rcv), 1);
    ASSERT_EQ_U(nx_ipc_dispatch(&rcv, 10), 1);
    ASSERT_EQ_U(rxs.handle_calls, 1);
}

/* ops-side pause that fails AFTER pause_hook succeeded — rollback must
 * still fire.  Uses a descriptor whose pause_hook returns OK but whose
 * pause() returns an error. */
static int pass_hook_stub(void *self)     { (void)self; return NX_OK; }
static int failing_pause_stub(void *self) { (void)self; return NX_EBUSY; }
static const struct nx_component_ops pause_fail_ops = {
    .handle_msg = rx_handle,
    .pause_hook = pass_hook_stub,
    .pause      = failing_pause_stub,
    .resume     = rx_resume,
};
static const struct nx_component_descriptor pause_fail_desc = {
    .name = "pause_fail", .state_size = sizeof(struct rx_state),
    .ops  = &pause_fail_ops,
};

TEST(pause_op_failure_after_hook_success_rolls_back)
{
    nx_graph_reset(); nx_ipc_reset(); nx_hook_reset();
    struct nx_slot      s = { .name = "s", .iface = "x" };
    struct rx_state     st = { 0 };
    struct nx_component c = {
        .manifest_id = "pf", .instance_id = "0",
        .descriptor  = &pause_fail_desc,
        .impl        = &st,
    };
    nx_slot_register(&s);
    nx_component_register(&c);
    nx_slot_swap(&s, &c);
    nx_component_init(&c);
    nx_component_enable(&c);

    ASSERT_EQ_U(nx_component_pause(&c), NX_EBUSY);
    ASSERT_EQ_U(c.state, NX_LC_ACTIVE);
    ASSERT_EQ_U(nx_slot_pause_state(&s), NX_SLOT_PAUSE_NONE);
}

/* Pause_hook that deliberately burns more than 1 ms of wall-clock time
 * so the slice-3.9b.2 deadline check trips.  Uses nanosleep + a
 * CLOCK_MONOTONIC loop so we don't race against a 1 ms sleep that the
 * kernel might schedule short. */
static int slow_pause_hook(void *self)
{
    (void)self;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
        struct timespec req = { .tv_sec = 0, .tv_nsec = 500000 }; /* 0.5 ms */
        nanosleep(&req, NULL);
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while ((uint64_t)(now.tv_sec - start.tv_sec) * 1000000000ULL +
             (now.tv_nsec - start.tv_nsec) < 2000000ULL);  /* 2 ms */
    return NX_OK;
}
static const struct nx_component_ops slow_hook_ops = {
    .handle_msg = rx_handle,
    .pause_hook = slow_pause_hook,
    .pause      = rx_pause,
    .resume     = rx_resume,
};
static const struct nx_component_descriptor slow_hook_desc = {
    .name = "slow_hook", .state_size = sizeof(struct rx_state),
    .ops  = &slow_hook_ops,
};

TEST(pause_hook_exceeding_deadline_returns_edeadline_and_rolls_back)
{
    nx_graph_reset(); nx_ipc_reset(); nx_hook_reset();
    struct nx_slot      s = { .name = "s", .iface = "x" };
    struct rx_state     st = { 0 };
    struct nx_component c = {
        .manifest_id = "sh", .instance_id = "0",
        .descriptor  = &slow_hook_desc,
        .impl        = &st,
    };
    nx_slot_register(&s);
    nx_component_register(&c);
    nx_slot_swap(&s, &c);
    nx_component_init(&c);
    nx_component_enable(&c);

    int rc = nx_component_pause(&c);
    ASSERT_EQ_U(rc, NX_EDEADLINE);
    ASSERT_EQ_U(c.state, NX_LC_ACTIVE);
    ASSERT_EQ_U(nx_slot_pause_state(&s), NX_SLOT_PAUSE_NONE);
    /* ops->pause never ran — the hook-side check tripped first. */
    ASSERT_EQ_U(st.pause_calls, 0);
}

/* --- Slice 3.9b.2: NX_HOOK_SLOT_SWAPPED dispatch ---------------------- */

struct swap_capture {
    int                   fires;
    struct nx_slot       *last_slot;
    struct nx_component  *last_old;
    struct nx_component  *last_new;
};

static enum nx_hook_action capture_swap(struct nx_hook_context *ctx, void *user)
{
    struct swap_capture *cap = user;
    cap->fires++;
    cap->last_slot = ctx->u.swap.slot;
    cap->last_old  = ctx->u.swap.old_impl;
    cap->last_new  = ctx->u.swap.new_impl;
    return NX_HOOK_CONTINUE;
}

TEST(slot_swap_fans_out_to_slot_swapped_hook_chain)
{
    nx_graph_reset(); nx_ipc_reset(); nx_hook_reset();

    struct nx_slot      s = { .name = "s", .iface = "x" };
    struct nx_component a = { .manifest_id = "m", .instance_id = "a" };
    struct nx_component b = { .manifest_id = "m", .instance_id = "b" };
    nx_slot_register(&s);
    nx_component_register(&a);
    nx_component_register(&b);

    struct swap_capture cap = { 0 };
    struct nx_hook h = {
        .point = NX_HOOK_SLOT_SWAPPED,
        .fn    = capture_swap,
        .user  = &cap,
    };
    ASSERT_EQ_U(nx_hook_register(&h), NX_OK);

    /* Bind → the hook fires with old=NULL, new=&a. */
    ASSERT_EQ_U(nx_slot_swap(&s, &a), NX_OK);
    ASSERT_EQ_U(cap.fires, 1);
    ASSERT_EQ_PTR(cap.last_slot, &s);
    ASSERT_EQ_PTR(cap.last_old,  NULL);
    ASSERT_EQ_PTR(cap.last_new,  &a);

    /* Swap to &b → fires with old=&a, new=&b. */
    ASSERT_EQ_U(nx_slot_swap(&s, &b), NX_OK);
    ASSERT_EQ_U(cap.fires, 2);
    ASSERT_EQ_PTR(cap.last_old, &a);
    ASSERT_EQ_PTR(cap.last_new, &b);

    /* Unbind → fires with old=&b, new=NULL. */
    ASSERT_EQ_U(nx_slot_swap(&s, NULL), NX_OK);
    ASSERT_EQ_U(cap.fires, 3);
    ASSERT_EQ_PTR(cap.last_old, &b);
    ASSERT_EQ_PTR(cap.last_new, NULL);

    nx_hook_unregister(&h);
}
