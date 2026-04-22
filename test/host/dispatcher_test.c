/*
 * Host tests for the MPSC primitive + dispatcher (slice 3.9b.1).
 *
 * The host harness is single-threaded, so we can't genuinely stress
 * the MPSC under concurrent producers.  What we CAN verify:
 *
 *   - Single-threaded FIFO ordering: the dispatcher consumes in the
 *     same order the producer pushes.
 *   - Empty-pop: a freshly-initialised queue returns NULL from pop.
 *   - End-to-end: nx_dispatcher_enqueue → nx_dispatcher_pump_once
 *     actually invokes the target component's handle_msg, runs the
 *     IPC_RECV hook once, and returns 1 per dispatched message.
 *   - Reset: after nx_dispatcher_reset(), the queue is empty again.
 *
 * The concurrent-stress verification lives in the kernel build,
 * where the timer ISR and the dispatcher kthread are genuinely
 * separate producers/consumers.
 */

#include "test_runner.h"

#include "core/lib/mpsc.h"
#include "framework/component.h"
#include "framework/dispatcher.h"
#include "framework/hook.h"
#include "framework/ipc.h"
#include "framework/registry.h"

#include <string.h>

/* --- 1. Raw MPSC primitive ---------------------------------------- */

TEST(mpsc_empty_pop_returns_null)
{
    struct nx_mpsc_queue q;
    nx_mpsc_init(&q);
    ASSERT_NULL(nx_mpsc_pop(&q));
    ASSERT_NULL(nx_mpsc_pop(&q));   /* second pop still empty */
}

TEST(mpsc_single_push_pop_roundtrip)
{
    struct nx_mpsc_queue q;
    nx_mpsc_init(&q);

    struct nx_mpsc_node n;
    nx_mpsc_push(&q, &n);

    struct nx_mpsc_node *out = nx_mpsc_pop(&q);
    ASSERT_EQ_PTR(out, &n);

    /* Pop again — queue should be empty. */
    ASSERT_NULL(nx_mpsc_pop(&q));
}

TEST(mpsc_fifo_ordering_preserved_across_N_pushes)
{
    struct nx_mpsc_queue q;
    nx_mpsc_init(&q);

    enum { N = 16 };
    struct nx_mpsc_node nodes[N];
    for (int i = 0; i < N; i++) nx_mpsc_push(&q, &nodes[i]);

    for (int i = 0; i < N; i++) {
        struct nx_mpsc_node *out = nx_mpsc_pop(&q);
        ASSERT_EQ_PTR(out, &nodes[i]);
    }
    ASSERT_NULL(nx_mpsc_pop(&q));
}

TEST(mpsc_interleaved_push_pop_keeps_ordering)
{
    struct nx_mpsc_queue q;
    nx_mpsc_init(&q);

    struct nx_mpsc_node a, b, c;

    nx_mpsc_push(&q, &a);
    ASSERT_EQ_PTR(nx_mpsc_pop(&q), &a);
    ASSERT_NULL(nx_mpsc_pop(&q));

    nx_mpsc_push(&q, &b);
    nx_mpsc_push(&q, &c);
    ASSERT_EQ_PTR(nx_mpsc_pop(&q), &b);
    ASSERT_EQ_PTR(nx_mpsc_pop(&q), &c);
    ASSERT_NULL(nx_mpsc_pop(&q));
}

/* --- 2. Dispatcher end-to-end ------------------------------------- */

/*
 * Minimal component: a handle_msg that counts invocations and
 * records the last msg pointer.  We wire it into a slot via the
 * registry, enqueue a message through the dispatcher, and pump.
 */
static int         sink_handle_count;
static struct nx_ipc_message *sink_last_msg;

static int sink_handle_msg(void *self, struct nx_ipc_message *msg)
{
    (void)self;
    sink_handle_count++;
    sink_last_msg = msg;
    return NX_OK;
}

static const struct nx_component_ops sink_ops = {
    .handle_msg = sink_handle_msg,
};

/* A tiny component descriptor — not registered via NX_COMPONENT_REGISTER
 * because we don't want to share the linker section with real
 * components; test-local direct composition is fine. */
static const struct nx_component_descriptor sink_descriptor = {
    .name      = "sink",
    .ops       = &sink_ops,
    .n_deps    = 0,
    .deps      = NULL,
    .state_size = 0,
};

struct dispatcher_fixture {
    struct nx_slot      slot;
    struct nx_component comp;
};

static void fixture_up(struct dispatcher_fixture *f)
{
    nx_graph_reset();
    nx_ipc_reset();
    nx_dispatcher_reset();
    sink_handle_count = 0;
    sink_last_msg = NULL;

    memset(f, 0, sizeof *f);
    f->slot.name       = "sink";
    f->slot.iface      = "sink";
    f->slot.mutability = NX_MUT_HOT;
    f->slot.concurrency = NX_CONC_SHARED;
    f->comp.manifest_id = "sink";
    f->comp.instance_id = "0";
    f->comp.descriptor = &sink_descriptor;
    f->comp.impl = NULL;

    ASSERT_EQ_U(nx_slot_register(&f->slot),      NX_OK);
    ASSERT_EQ_U(nx_component_register(&f->comp), NX_OK);
    ASSERT_EQ_U(nx_slot_swap(&f->slot, &f->comp), NX_OK);
}

TEST(dispatcher_pump_once_on_empty_returns_zero)
{
    nx_dispatcher_reset();
    ASSERT_EQ_U(nx_dispatcher_pump_once(), 0);
}

TEST(dispatcher_pump_runs_handler_after_enqueue)
{
    struct dispatcher_fixture f;
    fixture_up(&f);

    struct nx_ipc_message msg;
    memset(&msg, 0, sizeof msg);
    msg.src_slot = &f.slot;   /* self-send is OK for the test */
    msg.dst_slot = &f.slot;
    msg.msg_type = 7;

    ASSERT_EQ_U(nx_dispatcher_enqueue(&msg), NX_OK);
    ASSERT_EQ_U(nx_dispatcher_pump_once(), 1);
    ASSERT_EQ_U(sink_handle_count, 1);
    ASSERT_EQ_PTR(sink_last_msg, &msg);

    /* No more messages. */
    ASSERT_EQ_U(nx_dispatcher_pump_once(), 0);
}

TEST(dispatcher_handles_N_enqueues_fifo_order)
{
    struct dispatcher_fixture f;
    fixture_up(&f);

    enum { N = 8 };
    struct nx_ipc_message msgs[N];
    for (int i = 0; i < N; i++) {
        memset(&msgs[i], 0, sizeof msgs[i]);
        msgs[i].src_slot = &f.slot;
        msgs[i].dst_slot = &f.slot;
        msgs[i].msg_type = (uint32_t)i;
        ASSERT_EQ_U(nx_dispatcher_enqueue(&msgs[i]), NX_OK);
    }
    for (int i = 0; i < N; i++) {
        ASSERT_EQ_U(nx_dispatcher_pump_once(), 1);
        ASSERT_EQ_PTR(sink_last_msg, &msgs[i]);
    }
    ASSERT_EQ_U(sink_handle_count, N);
    ASSERT_EQ_U(nx_dispatcher_pump_once(), 0);
}

TEST(dispatcher_enqueue_rejects_null_args)
{
    nx_dispatcher_reset();
    ASSERT_EQ_U(nx_dispatcher_enqueue(NULL), NX_EINVAL);

    struct nx_ipc_message m;
    memset(&m, 0, sizeof m);
    m.dst_slot = NULL;   /* NULL destination */
    ASSERT_EQ_U(nx_dispatcher_enqueue(&m), NX_EINVAL);
}

TEST(dispatcher_reset_drops_pending_messages)
{
    struct dispatcher_fixture f;
    fixture_up(&f);

    struct nx_ipc_message a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    a.src_slot = a.dst_slot = &f.slot;
    b.src_slot = b.dst_slot = &f.slot;

    nx_dispatcher_enqueue(&a);
    nx_dispatcher_enqueue(&b);
    nx_dispatcher_reset();
    ASSERT_EQ_U(nx_dispatcher_pump_once(), 0);
    ASSERT_EQ_U(sink_handle_count, 0);
}

/* File-scope hook + state for the hook-ordering test. */
static int recv_hook_count;
static int recv_hook_saw_handle_count;

static enum nx_hook_action recv_hook_fn(struct nx_hook_context *c, void *u)
{
    (void)c; (void)u;
    recv_hook_count++;
    recv_hook_saw_handle_count = sink_handle_count;
    return NX_HOOK_CONTINUE;
}

TEST(dispatcher_fires_ipc_recv_hook_before_handler)
{
    struct dispatcher_fixture f;
    fixture_up(&f);

    recv_hook_count = 0;
    recv_hook_saw_handle_count = -1;

    struct nx_hook h = {
        .point = NX_HOOK_IPC_RECV,
        .priority = 0,
        .fn = recv_hook_fn,
        .user = NULL,
        .name = "disp_recv",
    };
    ASSERT_EQ_U(nx_hook_register(&h), NX_OK);

    struct nx_ipc_message m;
    memset(&m, 0, sizeof m);
    m.src_slot = m.dst_slot = &f.slot;
    m.msg_type = 42;

    nx_dispatcher_enqueue(&m);
    ASSERT_EQ_U(nx_dispatcher_pump_once(), 1);
    ASSERT_EQ_U(recv_hook_count, 1);
    ASSERT_EQ_U(recv_hook_saw_handle_count, 0);   /* hook ran before handler */
    ASSERT_EQ_U(sink_handle_count, 1);

    nx_hook_unregister(&h);
}
