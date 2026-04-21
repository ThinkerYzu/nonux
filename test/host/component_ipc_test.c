#include "test_runner.h"
#include "framework/component.h"
#include "framework/ipc.h"
#include "framework/registry.h"

/*
 * Host-side tests for framework/ipc.c — slice 3.6.
 *
 * Every test resets the graph and the IPC inbox table up front so
 * state from previous tests doesn't leak in.  A minimal test harness
 * convention:
 *
 *   - Each "component" is a paired (slot, component, state struct).
 *   - The component's descriptor carries ops->handle_msg.
 *   - Tests register slots, components, a connection edge, bind the
 *     component to its slot (nx_slot_swap), link the descriptor onto
 *     the nx_component, and set the impl pointer.
 *
 * That last step — `comp.descriptor = &...; comp.impl = &state;` — is
 * what slice 3.9 will automate from the descriptor walker.
 */

/* --- Shared test fixture: a "receiver" component with a counter ------ */

struct receiver_state {
    int  call_count;
    int  last_msg_type;
    int  last_return;
    struct nx_ipc_message *last_msg;
};

static int receiver_handle(void *self, struct nx_ipc_message *msg)
{
    struct receiver_state *s = self;
    s->call_count++;
    s->last_msg_type = msg->msg_type;
    s->last_msg      = msg;
    return s->last_return;
}

static const struct nx_component_ops receiver_ops = {
    .handle_msg = receiver_handle,
};

static const struct nx_component_descriptor receiver_descriptor = {
    .name        = "receiver",
    .state_size  = sizeof(struct receiver_state),
    .deps_offset = 0,
    .deps        = NULL,
    .n_deps      = 0,
    .ops         = &receiver_ops,
};

/* Build the fixture, leaving the caller to choose the connection mode. */
struct ipc_fixture {
    struct nx_slot         sender;
    struct nx_slot         receiver;
    struct nx_component    sender_comp;
    struct nx_component    receiver_comp;
    struct receiver_state  rx_state;
    struct nx_connection  *edge;
};

static void ipc_fixture_init(struct ipc_fixture *f, enum nx_conn_mode mode)
{
    nx_graph_reset();
    nx_ipc_reset();

    memset(f, 0, sizeof *f);
    f->sender   = (struct nx_slot){ .name = "sender",   .iface = "x" };
    f->receiver = (struct nx_slot){ .name = "receiver", .iface = "x" };
    f->sender_comp   = (struct nx_component){
        .manifest_id = "sender_impl",   .instance_id = "s0" };
    f->receiver_comp = (struct nx_component){
        .manifest_id = "receiver_impl", .instance_id = "r0",
        .impl        = &f->rx_state,
        .descriptor  = &receiver_descriptor,
    };

    nx_slot_register(&f->sender);
    nx_slot_register(&f->receiver);
    nx_component_register(&f->sender_comp);
    nx_component_register(&f->receiver_comp);
    nx_slot_swap(&f->sender,   &f->sender_comp);
    nx_slot_swap(&f->receiver, &f->receiver_comp);

    int err = NX_OK;
    f->edge = nx_connection_register(&f->sender, &f->receiver,
                                     mode, false, NX_PAUSE_QUEUE, &err);
}

/* --- Sync shortcut ---------------------------------------------------- */

TEST(sync_send_invokes_handler_directly)
{
    struct ipc_fixture f;
    ipc_fixture_init(&f, NX_CONN_SYNC);

    struct nx_ipc_message msg = {
        .src_slot = &f.sender,
        .dst_slot = &f.receiver,
        .msg_type = 7,
    };
    ASSERT_EQ_U(nx_ipc_send(&msg), NX_OK);
    ASSERT_EQ_U(f.rx_state.call_count, 1);
    ASSERT_EQ_U(f.rx_state.last_msg_type, 7);
    ASSERT_EQ_U(nx_ipc_inbox_depth(&f.receiver), 0);   /* nothing queued */
}

TEST(sync_send_without_edge_returns_enoent)
{
    nx_graph_reset(); nx_ipc_reset();
    struct nx_slot a = { .name = "a", .iface = "x" };
    struct nx_slot b = { .name = "b", .iface = "x" };
    nx_slot_register(&a); nx_slot_register(&b);
    /* No connection registered — router has nowhere to route to. */

    struct nx_ipc_message msg = { .src_slot = &a, .dst_slot = &b };
    ASSERT_EQ_U(nx_ipc_send(&msg), NX_ENOENT);
}

TEST(sync_send_to_slot_without_active_handler_returns_enoent)
{
    struct ipc_fixture f;
    ipc_fixture_init(&f, NX_CONN_SYNC);
    /* Unbind the receiver — sync shortcut has no one to call. */
    nx_slot_swap(&f.receiver, NULL);

    struct nx_ipc_message msg = {
        .src_slot = &f.sender, .dst_slot = &f.receiver,
    };
    ASSERT_EQ_U(nx_ipc_send(&msg), NX_ENOENT);
}

TEST(send_rejects_null_or_incomplete_message)
{
    ASSERT_EQ_U(nx_ipc_send(NULL), NX_EINVAL);
    struct nx_ipc_message m = { 0 };
    ASSERT_EQ_U(nx_ipc_send(&m), NX_EINVAL);
}

/* --- Async queue ------------------------------------------------------ */

TEST(async_send_enqueues_and_dispatch_drains)
{
    struct ipc_fixture f;
    ipc_fixture_init(&f, NX_CONN_ASYNC);

    struct nx_ipc_message m1 = {
        .src_slot = &f.sender, .dst_slot = &f.receiver, .msg_type = 11 };
    struct nx_ipc_message m2 = {
        .src_slot = &f.sender, .dst_slot = &f.receiver, .msg_type = 22 };

    ASSERT_EQ_U(nx_ipc_send(&m1), NX_OK);
    ASSERT_EQ_U(nx_ipc_send(&m2), NX_OK);

    /* Handler hasn't fired yet — async. */
    ASSERT_EQ_U(f.rx_state.call_count, 0);
    ASSERT_EQ_U(nx_ipc_inbox_depth(&f.receiver), 2);

    /* Drain exactly one first. */
    ASSERT_EQ_U(nx_ipc_dispatch(&f.receiver, 1), 1);
    ASSERT_EQ_U(f.rx_state.call_count, 1);
    ASSERT_EQ_U(f.rx_state.last_msg_type, 11);       /* FIFO order */
    ASSERT_EQ_U(nx_ipc_inbox_depth(&f.receiver), 1);

    /* Drain the rest. */
    ASSERT_EQ_U(nx_ipc_dispatch(&f.receiver, 8), 1);
    ASSERT_EQ_U(f.rx_state.call_count, 2);
    ASSERT_EQ_U(f.rx_state.last_msg_type, 22);
    ASSERT_EQ_U(nx_ipc_inbox_depth(&f.receiver), 0);
}

TEST(dispatch_stops_on_handler_error)
{
    struct ipc_fixture f;
    ipc_fixture_init(&f, NX_CONN_ASYNC);
    f.rx_state.last_return = NX_EINVAL;  /* handler reports failure */

    struct nx_ipc_message m1 = {
        .src_slot = &f.sender, .dst_slot = &f.receiver };
    struct nx_ipc_message m2 = {
        .src_slot = &f.sender, .dst_slot = &f.receiver };
    nx_ipc_send(&m1);
    nx_ipc_send(&m2);

    /* Ask for up to 10; dispatch stops after the first handler fails. */
    ASSERT_EQ_U(nx_ipc_dispatch(&f.receiver, 10), 1);
    ASSERT_EQ_U(f.rx_state.call_count, 1);
    ASSERT_EQ_U(nx_ipc_inbox_depth(&f.receiver), 1);
}

TEST(dispatch_on_empty_slot_is_zero)
{
    nx_graph_reset(); nx_ipc_reset();
    struct nx_slot s = { .name = "s", .iface = "x" };
    nx_slot_register(&s);
    ASSERT_EQ_U(nx_ipc_dispatch(&s, 10), 0);
}

/* --- Cap scanning ----------------------------------------------------- */

TEST(scan_send_caps_accepts_registered_slot_ref)
{
    nx_graph_reset(); nx_ipc_reset();
    struct nx_slot a = { .name = "a", .iface = "x" };
    struct nx_slot b = { .name = "b", .iface = "x" };
    nx_slot_register(&a); nx_slot_register(&b);
    int err = NX_OK;
    nx_connection_register(&a, &b, NX_CONN_ASYNC, false,
                           NX_PAUSE_QUEUE, &err);

    struct nx_ipc_cap cap = {
        .kind = NX_CAP_SLOT_REF, .ownership = NX_CAP_BORROW,
        .u.slot_ref = &b,
    };
    struct nx_ipc_message m = {
        .src_slot = &a, .dst_slot = &b,
        .n_caps = 1, .caps = &cap,
    };
    ASSERT_EQ_U(nx_ipc_scan_send_caps(&a, &m), NX_OK);
}

TEST(scan_send_caps_rejects_forged_slot_ref)
{
    nx_graph_reset(); nx_ipc_reset();
    struct nx_slot a = { .name = "a", .iface = "x" };
    struct nx_slot b = { .name = "b", .iface = "x" };
    struct nx_slot c = { .name = "c", .iface = "x" };   /* a has no edge to c */
    nx_slot_register(&a); nx_slot_register(&b); nx_slot_register(&c);
    int err = NX_OK;
    nx_connection_register(&a, &b, NX_CONN_ASYNC, false,
                           NX_PAUSE_QUEUE, &err);

    struct nx_ipc_cap cap = {
        .kind = NX_CAP_SLOT_REF, .ownership = NX_CAP_BORROW,
        .u.slot_ref = &c,          /* forged — no registered edge a→c */
    };
    struct nx_ipc_message m = {
        .src_slot = &a, .dst_slot = &b,
        .n_caps = 1, .caps = &cap,
    };
    ASSERT_EQ_U(nx_ipc_scan_send_caps(&a, &m), NX_EINVAL);
}

TEST(send_rejects_forged_caps)
{
    /* Forged cap on the router's real path. */
    struct ipc_fixture f;
    ipc_fixture_init(&f, NX_CONN_SYNC);

    struct nx_slot stranger = { .name = "stranger", .iface = "x" };
    nx_slot_register(&stranger);

    struct nx_ipc_cap cap = {
        .kind = NX_CAP_SLOT_REF, .ownership = NX_CAP_BORROW,
        .u.slot_ref = &stranger,   /* sender has no edge to stranger */
    };
    struct nx_ipc_message m = {
        .src_slot = &f.sender, .dst_slot = &f.receiver,
        .n_caps = 1, .caps = &cap,
    };
    ASSERT_EQ_U(nx_ipc_send(&m), NX_EINVAL);
    ASSERT_EQ_U(f.rx_state.call_count, 0);         /* handler never ran */
}

TEST(scan_recv_caps_drops_borrowed_silently)
{
    struct nx_ipc_cap cap = {
        .kind = NX_CAP_SLOT_REF, .ownership = NX_CAP_BORROW,
    };
    struct nx_ipc_message m = { .n_caps = 1, .caps = &cap };
    ASSERT_EQ_U(nx_ipc_scan_recv_caps(NULL, &m), 0);
    ASSERT(!cap.claimed);   /* no claim implied by the sweep */
}

TEST(scan_recv_caps_flags_unclaimed_transfer)
{
    struct nx_ipc_cap caps[2] = {
        { .kind = NX_CAP_SLOT_REF, .ownership = NX_CAP_TRANSFER },
        { .kind = NX_CAP_SLOT_REF, .ownership = NX_CAP_TRANSFER,
          .claimed = true },   /* claimed — must not count */
    };
    struct nx_ipc_message m = { .n_caps = 2, .caps = caps };
    ASSERT_EQ_U(nx_ipc_scan_recv_caps(NULL, &m), 1);
}

/* --- slot_ref_retain / release ---------------------------------------- */

TEST(slot_ref_retain_registers_edge_and_claims_cap)
{
    nx_graph_reset(); nx_ipc_reset();

    struct nx_slot holder = { .name = "holder", .iface = "x" };
    struct nx_slot target = { .name = "target", .iface = "x" };
    nx_slot_register(&holder);
    nx_slot_register(&target);

    ASSERT_EQ_U(nx_graph_connection_count(), 0);

    struct nx_ipc_cap cap = {
        .kind = NX_CAP_SLOT_REF, .ownership = NX_CAP_TRANSFER,
        .u.slot_ref = &target,
    };
    struct nx_connection *c = NULL;
    ASSERT_EQ_U(nx_slot_ref_retain(&holder, &cap, "hold-target", &c),
                NX_OK);
    ASSERT_NOT_NULL(c);
    ASSERT(cap.claimed);
    ASSERT_EQ_U(nx_graph_connection_count(), 1);

    /* Post-retain recv-scan must NOT count a claimed transfer cap. */
    struct nx_ipc_message m = { .n_caps = 1, .caps = &cap };
    ASSERT_EQ_U(nx_ipc_scan_recv_caps(&holder, &m), 0);
}

TEST(slot_ref_release_unregisters_edge)
{
    nx_graph_reset(); nx_ipc_reset();

    struct nx_slot holder = { .name = "holder", .iface = "x" };
    struct nx_slot target = { .name = "target", .iface = "x" };
    nx_slot_register(&holder);
    nx_slot_register(&target);

    struct nx_ipc_cap cap = {
        .kind = NX_CAP_SLOT_REF, .ownership = NX_CAP_TRANSFER,
        .u.slot_ref = &target,
    };
    nx_slot_ref_retain(&holder, &cap, NULL, NULL);
    ASSERT_EQ_U(nx_graph_connection_count(), 1);

    nx_slot_ref_release(&holder, &target);
    ASSERT_EQ_U(nx_graph_connection_count(), 0);

    /* Release of an edge that doesn't exist is a no-op (not a crash). */
    nx_slot_ref_release(&holder, &target);
}

TEST(slot_ref_retain_rejects_bad_caps)
{
    nx_graph_reset(); nx_ipc_reset();
    struct nx_slot s = { .name = "s", .iface = "x" };
    nx_slot_register(&s);

    ASSERT_EQ_U(nx_slot_ref_retain(NULL, NULL, NULL, NULL), NX_EINVAL);

    struct nx_ipc_cap wrong_kind = { .kind = 999 };
    ASSERT_EQ_U(nx_slot_ref_retain(&s, &wrong_kind, NULL, NULL), NX_EINVAL);

    struct nx_ipc_cap null_target = {
        .kind = NX_CAP_SLOT_REF, .u.slot_ref = NULL };
    ASSERT_EQ_U(nx_slot_ref_retain(&s, &null_target, NULL, NULL), NX_EINVAL);

    struct nx_ipc_cap already_claimed = {
        .kind = NX_CAP_SLOT_REF, .claimed = true, .u.slot_ref = &s };
    ASSERT_EQ_U(nx_slot_ref_retain(&s, &already_claimed, NULL, NULL),
                NX_EINVAL);
}

/* --- Reset semantics -------------------------------------------------- */

TEST(ipc_reset_clears_all_inboxes)
{
    struct ipc_fixture f;
    ipc_fixture_init(&f, NX_CONN_ASYNC);

    struct nx_ipc_message m = {
        .src_slot = &f.sender, .dst_slot = &f.receiver };
    nx_ipc_send(&m);
    ASSERT_EQ_U(nx_ipc_inbox_depth(&f.receiver), 1);

    nx_ipc_reset();
    ASSERT_EQ_U(nx_ipc_inbox_depth(&f.receiver), 0);
}
