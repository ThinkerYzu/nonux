/*
 * Kernel tests for the framework dispatcher (slice 3.9b.1).
 *
 * By the time ktest_main runs, boot has already called:
 *   nx_framework_bootstrap() → sched_init() → nx_dispatcher_init()
 *
 * So the dispatcher kthread is spawned, enqueued on sched_rr's
 * runqueue, and ready to pop from the MPSC.  These tests verify:
 *
 *   1. The dispatcher pumps a message end-to-end from
 *      nx_dispatcher_enqueue into a component handler.
 *   2. nx_ipc_enqueue_from_irq routes the same way (ISR-safe
 *      entry — dispatched on the kthread, not in ISR context).
 *   3. Yield-driven interleave actually produces a handler call
 *      without the test manually pumping (proving the kthread is
 *      alive and cooperating).
 */

#include "ktest.h"

#include "framework/component.h"
#include "framework/dispatcher.h"
#include "framework/ipc.h"
#include "framework/registry.h"
#include "core/sched/sched.h"
#include "core/sched/task.h"

static int          sink_count;
static struct nx_ipc_message *sink_last;

static int sink_handle_msg(void *self, struct nx_ipc_message *msg)
{
    (void)self;
    sink_count++;
    sink_last = msg;
    return NX_OK;
}

static const struct nx_component_ops sink_ops = {
    .handle_msg = sink_handle_msg,
};

static const struct nx_component_descriptor sink_descriptor = {
    .name       = "ktest_sink",
    .ops        = &sink_ops,
    .n_deps     = 0,
    .deps       = NULL,
    .state_size = 0,
};

/* Fixture: a slot with a test-only sink bound, registered fresh per
 * test.  We don't clean up via destroy_guard rollbacks — these are
 * test-scope objects and the kernel exits via semihosting at
 * ktest_main's end. */
struct sink_fixture {
    struct nx_slot      slot;
    struct nx_component comp;
};

static void fixture_up(struct sink_fixture *f, const char *slot_name)
{
    sink_count = 0;
    sink_last  = NULL;
    nx_dispatcher_reset();

    f->slot = (struct nx_slot){
        .name        = slot_name,
        .iface       = "sink",
        .mutability  = NX_MUT_HOT,
        .concurrency = NX_CONC_SHARED,
    };
    f->comp = (struct nx_component){
        .manifest_id = "ktest_sink",
        .instance_id = "0",
        .descriptor  = &sink_descriptor,
        .impl        = NULL,
    };
    (void)nx_slot_register(&f->slot);
    (void)nx_component_register(&f->comp);
    (void)nx_slot_swap(&f->slot, &f->comp);
}

static void fixture_down(struct sink_fixture *f)
{
    /* Remove the binding + entries so a subsequent test can register
     * the same named slot again.  Swap to NULL first so unregister
     * doesn't hit the destroy guard. */
    (void)nx_slot_swap(&f->slot, NULL);
    (void)nx_component_unregister(&f->comp);
    (void)nx_slot_unregister(&f->slot);
    nx_dispatcher_reset();
}

KTEST(dispatcher_kthread_is_spawned_by_bootstrap)
{
    /* Direct evidence the dispatcher is alive: enqueue a message then
     * yield until it lands.  If nx_dispatcher_init failed to spawn
     * the kthread, no pumping happens and the counter stays zero. */
    struct sink_fixture f;
    fixture_up(&f, "disp_sink1");

    struct nx_ipc_message msg = {
        .src_slot = &f.slot, .dst_slot = &f.slot, .msg_type = 1,
    };
    KASSERT_EQ_U(nx_dispatcher_enqueue(&msg), NX_OK);

    for (int i = 0; i < 32 && sink_count == 0; i++)
        nx_task_yield();

    KASSERT_EQ_U(sink_count, 1);
    KASSERT_EQ_U((uint64_t)(uintptr_t)sink_last,
                 (uint64_t)(uintptr_t)&msg);

    fixture_down(&f);
}

KTEST(dispatcher_enqueue_from_irq_reaches_handler)
{
    /* nx_ipc_enqueue_from_irq is the ISR-safe entry.  We call it
     * from a plain kernel context (no real ISR), which is the same
     * code path — the only difference at runtime is the caller's
     * context.  The dispatcher kthread drains asynchronously. */
    struct sink_fixture f;
    fixture_up(&f, "disp_sink2");

    struct nx_ipc_message msg = {
        .src_slot = &f.slot, .dst_slot = &f.slot, .msg_type = 9,
    };
    KASSERT_EQ_U(nx_ipc_enqueue_from_irq(&msg), NX_OK);

    for (int i = 0; i < 32 && sink_count == 0; i++)
        nx_task_yield();

    KASSERT_EQ_U(sink_count, 1);
    fixture_down(&f);
}

KTEST(dispatcher_drains_multiple_messages_in_order)
{
    struct sink_fixture f;
    fixture_up(&f, "disp_sink3");

    enum { N = 4 };
    struct nx_ipc_message msgs[N];
    for (int i = 0; i < N; i++) {
        msgs[i] = (struct nx_ipc_message){
            .src_slot = &f.slot, .dst_slot = &f.slot,
            .msg_type = (uint32_t)i,
        };
        KASSERT_EQ_U(nx_dispatcher_enqueue(&msgs[i]), NX_OK);
    }

    /* Yield enough that the dispatcher has a chance to drain all N. */
    for (int i = 0; i < 64 && sink_count < N; i++)
        nx_task_yield();

    KASSERT_EQ_U(sink_count, N);
    /* Last-seen message is the last-enqueued (FIFO from sink_last). */
    KASSERT_EQ_U((uint64_t)(uintptr_t)sink_last,
                 (uint64_t)(uintptr_t)&msgs[N - 1]);

    fixture_down(&f);
}
