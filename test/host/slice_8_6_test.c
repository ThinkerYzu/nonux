/*
 * Host-side tests for slice 8.6 — runtime async↔sync mode switching.
 *
 * Six tests:
 *   1.  CONN_REWIRE changes edge mode and pauses/resumes the to_slot.
 *   2.  After REWIRE to SYNC, nx_ipc_send dispatches the handler inline.
 *   3.  After REWIRE back to ASYNC, nx_ipc_send queues the message.
 *   4.  Hold queue messages are flushed via the new mode after REWIRE.
 *   5.  nx_config_set_conn_mode convenience wrapper works end-to-end.
 *   6.  NX_SYS_CONFIG_REWIRE syscall retunes the edge.
 */

#include "test_runner.h"

#include "framework/component.h"
#include "framework/config.h"
#include "framework/hook.h"
#include "framework/ipc.h"
#include "framework/recompose.h"
#include "framework/registry.h"
#include "framework/syscall.h"

#include <string.h>

/* ---- Trap frame shim ------------------------------------------------ */

struct trap_frame_86 {
    uint64_t x[31];
    uint64_t sp_el0;
    uint64_t pc;
    uint64_t pstate;
};

#define CALL_DISPATCH_86(tf) \
    nx_syscall_dispatch((struct trap_frame *)(tf))

static void reset_frame_86(struct trap_frame_86 *tf)
{
    memset(tf, 0, sizeof *tf);
}

/* ---- Shared component state & ops ---------------------------------- */

struct s86_state {
    int handle_calls;
    int pause_hook_calls;
    int pause_calls;
    int resume_calls;
};

static int s86_handle(void *self, struct nx_ipc_message *msg)
{
    (void)msg;
    ((struct s86_state *)self)->handle_calls++;
    return NX_OK;
}
static int s86_pause_hook(void *self) { ((struct s86_state *)self)->pause_hook_calls++; return NX_OK; }
static int s86_pause(void *self)      { ((struct s86_state *)self)->pause_calls++;      return NX_OK; }
static int s86_resume(void *self)     { ((struct s86_state *)self)->resume_calls++;     return NX_OK; }

static const struct nx_component_ops s86_comp_ops = {
    .handle_msg = s86_handle,
    .pause_hook = s86_pause_hook,
    .pause      = s86_pause,
    .resume     = s86_resume,
};

static const struct nx_component_descriptor s86_comp_desc = {
    .name       = "s86_test_comp",
    .state_size = sizeof(struct s86_state),
    .ops        = &s86_comp_ops,
};

/* ---- Common fixture ------------------------------------------------- */

struct s86_fixture {
    struct nx_slot       src;
    struct nx_slot       tgt;
    struct nx_component  src_comp;
    struct nx_component  tgt_comp;
    struct s86_state     tgt_state;
    struct nx_connection *edge;
};

static void suite_reset(void)
{
    nx_graph_reset();
    nx_ipc_reset();
    nx_hook_reset();
    nx_syscall_reset_for_test();
}

static void fixture_up(struct s86_fixture *f,
                       const char *src_name, const char *tgt_name,
                       enum nx_conn_mode initial_mode)
{
    suite_reset();
    memset(f, 0, sizeof *f);

    f->src = (struct nx_slot){ .name = src_name, .iface = "s86_iface",
                               .mutability  = NX_MUT_HOT,
                               .concurrency = NX_CONC_SHARED };
    f->tgt = (struct nx_slot){ .name = tgt_name, .iface = "s86_iface",
                               .mutability  = NX_MUT_HOT,
                               .concurrency = NX_CONC_SHARED };

    f->src_comp = (struct nx_component){ .manifest_id = "s86_src",
                                         .instance_id = "0" };
    f->tgt_comp = (struct nx_component){ .manifest_id = "s86_tgt",
                                         .instance_id = "0",
                                         .impl        = &f->tgt_state,
                                         .descriptor  = &s86_comp_desc };

    nx_slot_register(&f->src);
    nx_slot_register(&f->tgt);
    nx_component_register(&f->src_comp);
    nx_component_register(&f->tgt_comp);
    nx_slot_swap(&f->src, &f->src_comp);
    nx_slot_swap(&f->tgt, &f->tgt_comp);
    nx_component_init(&f->tgt_comp);
    nx_component_enable(&f->tgt_comp);

    int err = NX_OK;
    f->edge = nx_connection_register(&f->src, &f->tgt,
                                     initial_mode, false, NX_PAUSE_QUEUE, &err);
}

/* ======================================================================
 * Test 1 — CONN_REWIRE changes edge mode; to_slot is paused and resumed
 * ====================================================================== */

TEST(rewire_changes_mode_pauses_and_resumes_to_slot)
{
    struct s86_fixture f;
    fixture_up(&f, "t1_src", "t1_tgt", NX_CONN_ASYNC);

    ASSERT_EQ_U(f.edge->mode, NX_CONN_ASYNC);

    struct nx_conn_change cc = {
        .from_slot = &f.src,
        .to_slot   = &f.tgt,
        .action    = NX_CONN_REWIRE,
        .mode      = NX_CONN_SYNC,
    };
    struct recomp_plan plan = {
        .connections     = &cc,
        .num_connections = 1,
        .changes         = NULL,
        .num_changes     = 0,
    };

    ASSERT_EQ_U(nx_recompose(&plan), NX_OK);

    /* Edge mode updated. */
    ASSERT_EQ_U(f.edge->mode, NX_CONN_SYNC);

    /* to_slot was paused then resumed (not replaced, so resume via ops->resume). */
    ASSERT_EQ_U(f.tgt_state.pause_hook_calls, 1);
    ASSERT_EQ_U(f.tgt_state.pause_calls,      1);
    ASSERT_EQ_U(f.tgt_state.resume_calls,     1);

    /* Slot pause state cleared. */
    ASSERT_EQ_U(nx_slot_pause_state(&f.tgt), NX_SLOT_PAUSE_NONE);
}

/* ======================================================================
 * Test 2 — after REWIRE to SYNC, nx_ipc_send invokes handler inline
 * ====================================================================== */

TEST(rewire_async_to_sync_dispatches_inline)
{
    struct s86_fixture f;
    fixture_up(&f, "t2_src", "t2_tgt", NX_CONN_ASYNC);

    struct nx_conn_change cc = { .from_slot = &f.src, .to_slot = &f.tgt,
                                 .action = NX_CONN_REWIRE, .mode = NX_CONN_SYNC };
    struct recomp_plan plan = { .connections = &cc, .num_connections = 1 };
    ASSERT_EQ_U(nx_recompose(&plan), NX_OK);
    ASSERT_EQ_U(f.edge->mode, NX_CONN_SYNC);

    /* Send a message — SYNC path calls handle_msg inline. */
    struct nx_ipc_message msg = { .src_slot = &f.src, .dst_slot = &f.tgt,
                                  .msg_type = 7 };
    ASSERT_EQ_U(nx_ipc_send(&msg), NX_OK);

    /* Handler was called immediately — no need to pump the inbox. */
    ASSERT_EQ_U(f.tgt_state.handle_calls, 1);

    /* Async inbox is empty. */
    ASSERT_EQ_U(nx_ipc_dispatch(&f.tgt, 10), 0);
    ASSERT_EQ_U(f.tgt_state.handle_calls, 1);
}

/* ======================================================================
 * Test 3 — after REWIRE back to ASYNC, message goes to queue
 * ====================================================================== */

TEST(rewire_sync_to_async_queues_message)
{
    struct s86_fixture f;
    fixture_up(&f, "t3_src", "t3_tgt", NX_CONN_SYNC);

    /* Rewire back to ASYNC. */
    struct nx_conn_change cc = { .from_slot = &f.src, .to_slot = &f.tgt,
                                 .action = NX_CONN_REWIRE, .mode = NX_CONN_ASYNC };
    struct recomp_plan plan = { .connections = &cc, .num_connections = 1 };
    ASSERT_EQ_U(nx_recompose(&plan), NX_OK);
    ASSERT_EQ_U(f.edge->mode, NX_CONN_ASYNC);

    /* Send — goes to the per-slot inbox, not dispatched yet. */
    struct nx_ipc_message msg = { .src_slot = &f.src, .dst_slot = &f.tgt,
                                  .msg_type = 3 };
    ASSERT_EQ_U(nx_ipc_send(&msg), NX_OK);
    ASSERT_EQ_U(f.tgt_state.handle_calls, 0);

    /* Pump inbox — now handled. */
    ASSERT_EQ_U(nx_ipc_dispatch(&f.tgt, 10), 1);
    ASSERT_EQ_U(f.tgt_state.handle_calls, 1);
}

/* ======================================================================
 * Test 4 — hold queue flushed via the new mode after REWIRE
 *
 * Setup: edge starts ASYNC; tgt is pre-paused so sends go to hold queue.
 * REWIRE to SYNC: PAUSE phase skips already-paused tgt, REWIRE retuned
 * edge, RESUME flushes hold queue through SYNC router → handler called
 * inline immediately (no manual dispatch needed).
 * ====================================================================== */

TEST(rewire_hold_queue_flushed_via_new_sync_mode)
{
    struct s86_fixture f;
    fixture_up(&f, "t4_src", "t4_tgt", NX_CONN_ASYNC);

    /* Pre-pause tgt so incoming messages go to the hold queue. */
    ASSERT_EQ_U(nx_component_pause(&f.tgt_comp), NX_OK);
    ASSERT_EQ_U(nx_slot_pause_state(&f.tgt), NX_SLOT_PAUSE_DONE);

    struct nx_ipc_message m1 = { .src_slot = &f.src, .dst_slot = &f.tgt, .msg_type = 1 };
    struct nx_ipc_message m2 = { .src_slot = &f.src, .dst_slot = &f.tgt, .msg_type = 2 };
    ASSERT_EQ_U(nx_ipc_send(&m1), NX_OK);
    ASSERT_EQ_U(nx_ipc_send(&m2), NX_OK);
    ASSERT_EQ_U(nx_ipc_hold_queue_depth(&f.src, &f.tgt), 2);
    ASSERT_EQ_U(f.tgt_state.handle_calls, 0);

    /* REWIRE to SYNC: recompose skips pause (tgt already PAUSED), retuned,
     * then resumes tgt and flushes hold queue via SYNC (inline dispatch). */
    struct nx_conn_change cc = { .from_slot = &f.src, .to_slot = &f.tgt,
                                 .action = NX_CONN_REWIRE, .mode = NX_CONN_SYNC };
    struct recomp_plan plan = { .connections = &cc, .num_connections = 1 };
    ASSERT_EQ_U(nx_recompose(&plan), NX_OK);

    /* Hold queue drained and both messages handled inline. */
    ASSERT_EQ_U(nx_ipc_hold_queue_depth(&f.src, &f.tgt), 0);
    ASSERT_EQ_U(f.tgt_state.handle_calls, 2);
    ASSERT_EQ_U(f.edge->mode, NX_CONN_SYNC);
}

/* ======================================================================
 * Test 5 — nx_config_set_conn_mode convenience wrapper
 * ====================================================================== */

TEST(config_set_conn_mode_rewires_edge)
{
    struct s86_fixture f;
    fixture_up(&f, "t5_src", "t5_tgt", NX_CONN_ASYNC);

    ASSERT_EQ_U(f.edge->mode, NX_CONN_ASYNC);

    int rc = nx_config_set_conn_mode("t5_src", "t5_tgt", NX_CONN_SYNC);
    ASSERT_EQ_U(rc, NX_OK);
    ASSERT_EQ_U(f.edge->mode, NX_CONN_SYNC);

    /* Send to confirm sync dispatch. */
    struct nx_ipc_message msg = { .src_slot = &f.src, .dst_slot = &f.tgt,
                                  .msg_type = 5 };
    ASSERT_EQ_U(nx_ipc_send(&msg), NX_OK);
    ASSERT_EQ_U(f.tgt_state.handle_calls, 1);

    /* NX_ENOENT for unknown slot names. */
    ASSERT_EQ_U((unsigned)nx_config_set_conn_mode("t5_src", "no_such", NX_CONN_ASYNC),
                (unsigned)NX_ENOENT);
}

/* ======================================================================
 * Test 6 — NX_SYS_CONFIG_REWIRE syscall retunes edge
 * ====================================================================== */

TEST(config_sys_rewire_syscall)
{
    struct s86_fixture f;
    fixture_up(&f, "t6_src", "t6_tgt", NX_CONN_ASYNC);

    /* Open a config handle. */
    struct trap_frame_86 tf;
    reset_frame_86(&tf);
    tf.x[8] = NX_SYS_CONFIG_OPEN;
    CALL_DISPATCH_86(&tf);
    nx_handle_t h = (nx_handle_t)(int64_t)tf.x[0];
    ASSERT((int64_t)h > 0);

    const char *from_str = "t6_src";
    const char *to_str   = "t6_tgt";

    reset_frame_86(&tf);
    tf.x[8] = NX_SYS_CONFIG_REWIRE;
    tf.x[0] = (uint64_t)h;
    tf.x[1] = (uint64_t)(uintptr_t)from_str;
    tf.x[2] = (uint64_t)(uintptr_t)to_str;
    tf.x[3] = (uint64_t)NX_CONN_SYNC;
    CALL_DISPATCH_86(&tf);
    ASSERT_EQ_U((uint64_t)(int64_t)tf.x[0], (uint64_t)NX_OK);

    /* Edge is now SYNC. */
    ASSERT_EQ_U(f.edge->mode, NX_CONN_SYNC);

    /* Send — handled inline. */
    struct nx_ipc_message msg = { .src_slot = &f.src, .dst_slot = &f.tgt,
                                  .msg_type = 6 };
    ASSERT_EQ_U(nx_ipc_send(&msg), NX_OK);
    ASSERT_EQ_U(f.tgt_state.handle_calls, 1);

    /* Invalid mode value returns NX_EINVAL. */
    reset_frame_86(&tf);
    tf.x[8] = NX_SYS_CONFIG_REWIRE;
    tf.x[0] = (uint64_t)h;
    tf.x[1] = (uint64_t)(uintptr_t)from_str;
    tf.x[2] = (uint64_t)(uintptr_t)to_str;
    tf.x[3] = 99; /* invalid mode */
    CALL_DISPATCH_86(&tf);
    ASSERT_EQ_U((unsigned)(int64_t)tf.x[0], (unsigned)NX_EINVAL);
}
