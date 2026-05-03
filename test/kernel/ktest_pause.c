/*
 * Kernel tests for the slice-8.1 pause/drain/resume protocol.
 *
 * Validates that nx_component_pause correctly waits for the dispatcher
 * kthread to drain all in-flight MPSC messages before transitioning
 * to DONE, and that messages queued to the hold queue during a pause
 * are delivered after resume.
 *
 * Three tests:
 *   1. Basic pause → DONE state transition on the kernel side.
 *   2. Pause while messages are in-flight in the MPSC: drain step must
 *      yield until the dispatcher delivers them.
 *   3. Full pause → hold queue → resume → delivery cycle.
 */

#include "ktest.h"

#include "framework/component.h"
#include "framework/dispatcher.h"
#include "framework/ipc.h"
#include "framework/registry.h"
#include "core/sched/sched.h"
#include "core/sched/task.h"

/* ---------- Shared sink component -------------------------------------- */

static int g_sink_handle_count;
static int g_sink_pause_hook_count;
static int g_sink_pause_count;
static int g_sink_resume_count;

static int sink_handle(void *self, struct nx_ipc_message *msg)
{
    (void)self; (void)msg;
    g_sink_handle_count++;
    return NX_OK;
}

static int sink_pause_hook(void *self) { (void)self; g_sink_pause_hook_count++; return NX_OK; }
static int sink_pause(void *self)      { (void)self; g_sink_pause_count++;      return NX_OK; }
static int sink_resume(void *self)     { (void)self; g_sink_resume_count++;     return NX_OK; }

static const struct nx_component_ops sink_ops = {
    .handle_msg = sink_handle,
    .pause_hook = sink_pause_hook,
    .pause      = sink_pause,
    .resume     = sink_resume,
};

static const struct nx_component_descriptor sink_desc = {
    .name        = "ktest_pause_sink",
    .ops         = &sink_ops,
    .state_size  = 0,
};

/* ---------- Per-test fixture ------------------------------------------- */

struct pause_fixture {
    struct nx_slot       sender;
    struct nx_slot       receiver;
    struct nx_component  sender_comp;
    struct nx_component  receiver_comp;
    struct nx_connection *edge;
};

static void fixture_up(struct pause_fixture *f,
                       const char *sender_name,
                       const char *receiver_name,
                       enum nx_pause_policy policy)
{
    g_sink_handle_count = g_sink_pause_hook_count = 0;
    g_sink_pause_count  = g_sink_resume_count     = 0;
    nx_dispatcher_reset();

    f->sender   = (struct nx_slot){ .name = sender_name,   .iface = "kpause_test",
                                    .mutability  = NX_MUT_HOT,
                                    .concurrency = NX_CONC_SHARED };
    f->receiver = (struct nx_slot){ .name = receiver_name, .iface = "kpause_test",
                                    .mutability  = NX_MUT_HOT,
                                    .concurrency = NX_CONC_SHARED };
    f->sender_comp   = (struct nx_component){ .manifest_id = "kpause_s", .instance_id = "0" };
    f->receiver_comp = (struct nx_component){ .manifest_id = "kpause_r", .instance_id = "0",
                                              .descriptor  = &sink_desc };

    (void)nx_slot_register(&f->sender);
    (void)nx_slot_register(&f->receiver);
    (void)nx_component_register(&f->sender_comp);
    (void)nx_component_register(&f->receiver_comp);
    (void)nx_slot_swap(&f->sender,   &f->sender_comp);
    (void)nx_slot_swap(&f->receiver, &f->receiver_comp);
    (void)nx_component_init(&f->receiver_comp);
    (void)nx_component_enable(&f->receiver_comp);

    int err = NX_OK;
    f->edge = nx_connection_register(&f->sender, &f->receiver,
                                     NX_CONN_ASYNC, false, policy, &err);
}

static void fixture_down(struct pause_fixture *f)
{
    (void)nx_slot_swap(&f->sender,   NULL);
    (void)nx_slot_swap(&f->receiver, NULL);
    (void)nx_component_unregister(&f->sender_comp);
    (void)nx_component_unregister(&f->receiver_comp);
    (void)nx_slot_unregister(&f->sender);
    (void)nx_slot_unregister(&f->receiver);
    nx_dispatcher_reset();
}

/* ---------- Test 1: basic pause → DONE state transition ---------------- */

KTEST(pause_kernel_slot_transitions_to_done)
{
    struct pause_fixture f;
    fixture_up(&f, "kp_sender1", "kp_receiver1", NX_PAUSE_QUEUE);

    KASSERT_EQ_U(nx_slot_pause_state(&f.receiver), NX_SLOT_PAUSE_NONE);
    KASSERT_EQ_U(nx_component_pause(&f.receiver_comp), NX_OK);
    KASSERT_EQ_U(nx_slot_pause_state(&f.receiver), NX_SLOT_PAUSE_DONE);
    KASSERT_EQ_U(g_sink_pause_hook_count, 1);
    KASSERT_EQ_U(g_sink_pause_count,      1);
    KASSERT_EQ_U(f.receiver_comp.state,   NX_LC_PAUSED);

    KASSERT_EQ_U(nx_component_resume(&f.receiver_comp), NX_OK);
    KASSERT_EQ_U(g_sink_resume_count, 1);
    KASSERT_EQ_U(nx_slot_pause_state(&f.receiver), NX_SLOT_PAUSE_NONE);
    KASSERT_EQ_U(f.receiver_comp.state, NX_LC_ACTIVE);

    fixture_down(&f);
}

/* ---------- Test 2: drain step waits for in-flight MPSC messages ------- */

KTEST(pause_kernel_drains_inflight_mpsc_messages)
{
    /* Enqueue an async message without yielding, then immediately call
     * nx_component_pause.  The drain loop in slot_drain_cb must yield
     * until the dispatcher kthread delivers the message — proving that
     * in_flight_calls is tracked at enqueue time (slice 8.1 property). */
    struct pause_fixture f;
    fixture_up(&f, "kp_sender2", "kp_receiver2", NX_PAUSE_QUEUE);

    struct nx_ipc_message msg = {
        .src_slot = &f.sender, .dst_slot = &f.receiver, .msg_type = 77,
    };
    KASSERT_EQ_U(nx_ipc_send(&msg), NX_OK);
    /* Message is in the MPSC; not yet dispatched. */
    KASSERT_EQ_U(g_sink_handle_count, 0);

    /* Pause — the drain step must yield until the dispatcher delivers
     * the in-flight message before proceeding to DONE. */
    KASSERT_EQ_U(nx_component_pause(&f.receiver_comp), NX_OK);
    KASSERT_EQ_U(nx_slot_pause_state(&f.receiver), NX_SLOT_PAUSE_DONE);

    /* The message was delivered during the drain. */
    KASSERT_EQ_U(g_sink_handle_count, 1);

    KASSERT_EQ_U(nx_component_resume(&f.receiver_comp), NX_OK);
    fixture_down(&f);
}

/* ---------- Test 3: pause → hold queue → resume → message delivery ----- */

KTEST(pause_kernel_hold_queue_drains_on_resume)
{
    /* Pause the receiver, send two messages (which land on the hold
     * queue, not the MPSC), resume, then wait for the dispatcher to
     * deliver both.  Verifies no message loss across the pause cycle. */
    struct pause_fixture f;
    fixture_up(&f, "kp_sender3", "kp_receiver3", NX_PAUSE_QUEUE);

    KASSERT_EQ_U(nx_component_pause(&f.receiver_comp), NX_OK);
    KASSERT_EQ_U(nx_slot_pause_state(&f.receiver), NX_SLOT_PAUSE_DONE);

    /* Send while paused — QUEUE policy holds them. */
    struct nx_ipc_message m1 = {
        .src_slot = &f.sender, .dst_slot = &f.receiver, .msg_type = 1 };
    struct nx_ipc_message m2 = {
        .src_slot = &f.sender, .dst_slot = &f.receiver, .msg_type = 2 };
    KASSERT_EQ_U(nx_ipc_send(&m1), NX_OK);
    KASSERT_EQ_U(nx_ipc_send(&m2), NX_OK);
    KASSERT_EQ_U(nx_ipc_hold_queue_depth(&f.sender, &f.receiver), 2);
    KASSERT_EQ_U(g_sink_handle_count, 0);

    /* Resume: hold queue flushes back through the IPC router into the
     * dispatcher MPSC; the kthread delivers them asynchronously. */
    KASSERT_EQ_U(nx_component_resume(&f.receiver_comp), NX_OK);
    KASSERT_EQ_U(g_sink_resume_count, 1);
    KASSERT_EQ_U(nx_ipc_hold_queue_depth(&f.sender, &f.receiver), 0);

    for (int i = 0; i < 64 && g_sink_handle_count < 2; i++)
        nx_task_yield();

    KASSERT_EQ_U(g_sink_handle_count, 2);

    fixture_down(&f);
}
