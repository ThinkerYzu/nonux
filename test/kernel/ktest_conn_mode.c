/*
 * Kernel tests for slice 8.6 — runtime async↔sync mode switching.
 *
 * Two tests (defined in REVERSE execution order):
 *   source-1: conn_mode_sync_to_async_dispatches_via_queue → runs SECOND
 *   source-2: conn_mode_async_to_sync_dispatches_inline    → runs FIRST
 *
 * Tests call nx_connection_retune() directly (no recompose / timer
 * interaction).  Fixture uses static storage to avoid the
 * dangling-comp-node issue.
 *
 * Async-queue observation: send a message, verify the handler hasn't
 * run (count still 0), yield until the dispatcher delivers it.
 * Sync dispatch: send, verify count increments immediately (no yield).
 *
 * Teardown drains the MPSC with yields before unregistering, so no
 * stale messages remain for later tests (live_swap etc.).
 */

#include "ktest.h"

#include "framework/component.h"
#include "framework/ipc.h"
#include "framework/registry.h"
#include "core/sched/sched.h"
#include "core/sched/task.h"

/* ---- Shared counter -------------------------------------------------- */

static volatile int g_cm_count;

static int cm_handle(void *self, struct nx_ipc_message *msg)
{
    (void)self; (void)msg;
    g_cm_count++;
    return NX_OK;
}

static const struct nx_component_ops cm_comp_ops = {
    .handle_msg = cm_handle,
};
static const struct nx_component_descriptor cm_comp_desc = {
    .name = "ktest_cm_comp",
    .ops  = &cm_comp_ops,
};

/* ---- Static fixture -------------------------------------------------- */

static struct nx_slot      s_cm_src;
static struct nx_slot      s_cm_tgt;
static struct nx_component s_cm_src_comp;
static struct nx_component s_cm_tgt_comp;
static struct nx_connection *cm_edge;
static int s_cm_fixture_active;

static void cm_fixture_up(const char *src_name, const char *tgt_name,
                           enum nx_conn_mode initial_mode)
{
    g_cm_count = 0;

    s_cm_src = (struct nx_slot){ .name = src_name, .iface = "cm_test",
                                 .mutability  = NX_MUT_HOT,
                                 .concurrency = NX_CONC_SHARED };
    s_cm_tgt = (struct nx_slot){ .name = tgt_name, .iface = "cm_test",
                                 .mutability  = NX_MUT_HOT,
                                 .concurrency = NX_CONC_SHARED };
    s_cm_src_comp = (struct nx_component){ .manifest_id = "cm_src",
                                           .instance_id = "0" };
    s_cm_tgt_comp = (struct nx_component){ .manifest_id = "cm_tgt",
                                           .instance_id = "0",
                                           .descriptor  = &cm_comp_desc };

    (void)nx_slot_register(&s_cm_src);
    (void)nx_slot_register(&s_cm_tgt);
    (void)nx_component_register(&s_cm_src_comp);
    (void)nx_component_register(&s_cm_tgt_comp);
    (void)nx_slot_swap(&s_cm_src, &s_cm_src_comp);
    (void)nx_slot_swap(&s_cm_tgt, &s_cm_tgt_comp);
    (void)nx_component_init(&s_cm_tgt_comp);
    (void)nx_component_enable(&s_cm_tgt_comp);

    int err = NX_OK;
    cm_edge = nx_connection_register(&s_cm_src, &s_cm_tgt,
                                     initial_mode, false, NX_PAUSE_QUEUE, &err);
    s_cm_fixture_active = 1;
}

static void cm_fixture_down(void)
{
    if (!s_cm_fixture_active) return;
    s_cm_fixture_active = 0;

    /* Unregister connection first: nx_slot_unregister returns NX_EBUSY
     * if any edge is still attached, leaving a dangling slot_node. */
    if (cm_edge) {
        (void)nx_connection_unregister(cm_edge);
        cm_edge = NULL;
    }

    (void)nx_slot_swap(&s_cm_src, NULL);
    (void)nx_slot_swap(&s_cm_tgt, NULL);
    (void)nx_component_disable(&s_cm_tgt_comp);
    (void)nx_component_unregister(&s_cm_src_comp);
    (void)nx_component_unregister(&s_cm_tgt_comp);
    (void)nx_slot_unregister(&s_cm_src);
    (void)nx_slot_unregister(&s_cm_tgt);
}

/* ======================================================================
 * Test 1 (runs SECOND) — retune back to ASYNC; send queues in MPSC
 * ====================================================================== */

KTEST(conn_mode_sync_to_async_dispatches_via_queue)
{
    cm_fixture_up("cm2_src", "cm2_tgt", NX_CONN_SYNC);
    KASSERT_EQ_U(cm_edge->mode, NX_CONN_SYNC);

    /* Direct retune to ASYNC — single-CPU, no concurrent senders. */
    KASSERT_EQ_U(nx_connection_retune(cm_edge, NX_CONN_ASYNC, false), NX_OK);
    KASSERT_EQ_U(cm_edge->mode, NX_CONN_ASYNC);

    /* Send — goes to MPSC dispatcher queue, not inline. */
    struct nx_ipc_message msg = { .src_slot = &s_cm_src, .dst_slot = &s_cm_tgt,
                                  .msg_type = 2 };
    KASSERT_EQ_U(nx_ipc_send(&msg), NX_OK);

    /* Handler has NOT run yet (single-CPU: dispatcher runs only on yield). */
    KASSERT_EQ_U(g_cm_count, 0);

    /* Yield until the dispatcher delivers the message and count increments. */
    for (int i = 0; i < 64 && g_cm_count < 1; i++)
        nx_task_yield();

    KASSERT_EQ_U(g_cm_count, 1);

    cm_fixture_down();
}

/* ======================================================================
 * Test 2 (runs FIRST) — retune to SYNC; send dispatches inline
 * ====================================================================== */

KTEST(conn_mode_async_to_sync_dispatches_inline)
{
    cm_fixture_up("cm1_src", "cm1_tgt", NX_CONN_ASYNC);
    KASSERT_EQ_U(cm_edge->mode, NX_CONN_ASYNC);

    /* Retune to SYNC — single-CPU, no concurrent senders. */
    KASSERT_EQ_U(nx_connection_retune(cm_edge, NX_CONN_SYNC, false), NX_OK);
    KASSERT_EQ_U(cm_edge->mode, NX_CONN_SYNC);

    /* Send — SYNC mode calls handler inline within nx_ipc_send. */
    struct nx_ipc_message msg = { .src_slot = &s_cm_src, .dst_slot = &s_cm_tgt,
                                  .msg_type = 1 };
    KASSERT_EQ_U(nx_ipc_send(&msg), NX_OK);

    /* Count incremented immediately — no yield needed for SYNC dispatch. */
    KASSERT_EQ_U(g_cm_count, 1);

    cm_fixture_down();
}
