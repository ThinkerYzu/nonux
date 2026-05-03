/*
 * Host-side tests for slice 8.2 — nx_recompose() orchestrator.
 *
 * Six tests:
 *   1. Leaf slot REPLACE: old comp destroyed, new comp becomes ACTIVE.
 *   2. FROZEN slot → NX_EPERM.
 *   3. new_comp not READY → NX_ESTATE.
 *   4. Hold queue delivers to new component after REPLACE.
 *   5. CONN_ADD registers a new edge as part of the plan.
 *   6. Pause-hook failure → plan rolled back, old comp stays ACTIVE.
 */

#include "test_runner.h"

#include "framework/component.h"
#include "framework/hook.h"
#include "framework/ipc.h"
#include "framework/recompose.h"
#include "framework/registry.h"

#include <string.h>

/* ---- Shared component ops --------------------------------------------- */

struct comp_state {
    int handle_calls;
    int pause_hook_calls;
    int pause_calls;
    int resume_calls;
    int on_dep_swapped_calls;
    int pause_hook_rc;          /* return value for pause_hook (default 0) */
};

static int comp_handle(void *self, struct nx_ipc_message *msg)
{
    (void)msg;
    ((struct comp_state *)self)->handle_calls++;
    return NX_OK;
}
static int comp_pause_hook(void *self)
{
    struct comp_state *s = self;
    s->pause_hook_calls++;
    return s->pause_hook_rc;
}
static int comp_pause(void *self)  { ((struct comp_state *)self)->pause_calls++;  return NX_OK; }
static int comp_resume(void *self) { ((struct comp_state *)self)->resume_calls++; return NX_OK; }
static int comp_on_dep_swapped(void *self, struct nx_slot *dep_slot,
                                struct nx_component *old_comp,
                                struct nx_component *new_comp, uint32_t flags)
{
    (void)dep_slot; (void)old_comp; (void)new_comp; (void)flags;
    ((struct comp_state *)self)->on_dep_swapped_calls++;
    return NX_OK;
}

static const struct nx_component_ops test_comp_ops = {
    .handle_msg     = comp_handle,
    .pause_hook     = comp_pause_hook,
    .pause          = comp_pause,
    .resume         = comp_resume,
    .on_dep_swapped = comp_on_dep_swapped,
};

static const struct nx_component_descriptor test_comp_desc = {
    .name       = "recompose_test_comp",
    .state_size = sizeof(struct comp_state),
    .ops        = &test_comp_ops,
};

/* ---- Common setup / teardown ------------------------------------------ */

static void suite_reset(void)
{
    nx_graph_reset();
    nx_ipc_reset();
    nx_hook_reset();
}

/* Build a minimal two-slot fixture: source → target.
 * Drives target_comp to ACTIVE.  Returns the edge pointer. */
struct two_slot_fixture {
    struct nx_slot      src;
    struct nx_slot      tgt;
    struct nx_component src_comp;
    struct nx_component tgt_comp;
    struct comp_state   src_state;
    struct comp_state   tgt_state;
    struct nx_connection *edge;
};

static void fixture_up(struct two_slot_fixture *f,
                       const char *src_name, const char *tgt_name,
                       enum nx_pause_policy policy)
{
    suite_reset();
    memset(f, 0, sizeof *f);

    f->src = (struct nx_slot){ .name = src_name, .iface = "recomp_test",
                               .mutability  = NX_MUT_HOT,
                               .concurrency = NX_CONC_SHARED };
    f->tgt = (struct nx_slot){ .name = tgt_name, .iface = "recomp_test",
                               .mutability  = NX_MUT_HOT,
                               .concurrency = NX_CONC_SHARED };

    f->src_comp = (struct nx_component){ .manifest_id = "recomp_src",
                                         .instance_id = "0",
                                         .impl        = &f->src_state,
                                         .descriptor  = &test_comp_desc };
    f->tgt_comp = (struct nx_component){ .manifest_id = "recomp_tgt",
                                         .instance_id = "0",
                                         .impl        = &f->tgt_state,
                                         .descriptor  = &test_comp_desc };

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
                                     NX_CONN_ASYNC, false, policy, &err);
}

/* ======================================================================== */
/* Test 1: leaf slot REPLACE — old comp destroyed, new comp ACTIVE          */
/* ======================================================================== */

TEST(recompose_leaf_slot_replaces_component)
{
    struct two_slot_fixture f;
    fixture_up(&f, "rc1_src", "rc1_tgt", NX_PAUSE_QUEUE);

    struct comp_state    new_state = { 0 };
    struct nx_component  new_comp  = { .manifest_id = "recomp_new",
                                       .instance_id = "0",
                                       .impl        = &new_state,
                                       .descriptor  = &test_comp_desc };
    nx_component_register(&new_comp);
    nx_component_init(&new_comp);   /* UNINIT → READY */

    struct nx_slot_change change = {
        .slot      = &f.tgt,
        .action    = NX_SLOT_REPLACE,
        .new_comp  = &new_comp,
        .state_lost = false,
    };
    struct recomp_plan plan = {
        .changes      = &change,
        .num_changes  = 1,
        .connections  = NULL,
        .num_connections = 0,
    };

    ASSERT_EQ_U(nx_recompose(&plan), NX_OK);

    /* New comp is now ACTIVE and bound to the slot. */
    ASSERT(f.tgt.active == &new_comp);
    ASSERT_EQ_U(new_comp.state, NX_LC_ACTIVE);

    /* Old comp was disabled → destroyed. */
    ASSERT_EQ_U(f.tgt_comp.state, NX_LC_DESTROYED);

    /* Pause/resume hooks ran on old comp; new comp was enabled (not paused). */
    ASSERT_EQ_U(f.tgt_state.pause_hook_calls, 1);
    ASSERT_EQ_U(f.tgt_state.pause_calls,      1);
    ASSERT_EQ_U(new_state.resume_calls,        0);  /* new comp was enabled, not resumed */

    /* Slot pause state was cleared by slot_clear_pause. */
    ASSERT_EQ_U(nx_slot_pause_state(&f.tgt), NX_SLOT_PAUSE_NONE);
}

/* ======================================================================== */
/* Test 2: FROZEN slot → NX_EPERM                                            */
/* ======================================================================== */

TEST(recompose_frozen_slot_returns_eperm)
{
    suite_reset();

    struct nx_slot frozen_slot = { .name = "rc2_frozen", .iface = "x",
                                   .mutability  = NX_MUT_FROZEN,
                                   .concurrency = NX_CONC_SHARED };
    struct nx_component dummy = { .manifest_id = "dummy", .instance_id = "0" };
    nx_slot_register(&frozen_slot);
    nx_component_register(&dummy);

    struct nx_slot_change change = { .slot = &frozen_slot,
                                     .action = NX_SLOT_REPLACE, .new_comp = &dummy };
    struct recomp_plan plan = { .changes = &change, .num_changes = 1 };

    ASSERT_EQ_U((unsigned)nx_recompose(&plan), (unsigned)NX_EPERM);
}

/* ======================================================================== */
/* Test 3: new_comp not READY → NX_ESTATE                                   */
/* ======================================================================== */

TEST(recompose_new_comp_wrong_state_returns_estate)
{
    struct two_slot_fixture f;
    fixture_up(&f, "rc3_src", "rc3_tgt", NX_PAUSE_QUEUE);

    /* new_comp in UNINIT — not READY. */
    struct nx_component new_comp = { .manifest_id = "recomp_bad", .instance_id = "0" };
    nx_component_register(&new_comp);
    /* deliberately do NOT call nx_component_init — state stays UNINIT */

    struct nx_slot_change change = { .slot = &f.tgt, .action = NX_SLOT_REPLACE,
                                     .new_comp = &new_comp };
    struct recomp_plan plan = { .changes = &change, .num_changes = 1 };

    ASSERT_EQ_U((unsigned)nx_recompose(&plan), (unsigned)NX_ESTATE);

    /* Target unchanged. */
    ASSERT(f.tgt.active == &f.tgt_comp);
    ASSERT_EQ_U(f.tgt_comp.state, NX_LC_ACTIVE);
}

/* ======================================================================== */
/* Test 4: hold queue delivers to new component after REPLACE               */
/* ======================================================================== */

TEST(recompose_hold_queue_delivers_to_new_component)
{
    struct two_slot_fixture f;
    fixture_up(&f, "rc4_src", "rc4_tgt", NX_PAUSE_QUEUE);

    /* Manually pause old comp so subsequent sends go to the hold queue. */
    ASSERT_EQ_U(nx_component_pause(&f.tgt_comp), NX_OK);
    ASSERT_EQ_U(nx_slot_pause_state(&f.tgt), NX_SLOT_PAUSE_DONE);

    /* Send two messages — they land on the hold queue. */
    struct nx_ipc_message m1 = { .src_slot = &f.src, .dst_slot = &f.tgt, .msg_type = 1 };
    struct nx_ipc_message m2 = { .src_slot = &f.src, .dst_slot = &f.tgt, .msg_type = 2 };
    ASSERT_EQ_U(nx_ipc_send(&m1), NX_OK);
    ASSERT_EQ_U(nx_ipc_send(&m2), NX_OK);
    ASSERT_EQ_U(nx_ipc_hold_queue_depth(&f.src, &f.tgt), 2);

    /* Prepare new_comp (READY). */
    struct comp_state   new_state = { 0 };
    struct nx_component new_comp  = { .manifest_id = "recomp_new4",
                                      .instance_id = "0",
                                      .impl        = &new_state,
                                      .descriptor  = &test_comp_desc };
    nx_component_register(&new_comp);
    nx_component_init(&new_comp);

    struct nx_slot_change change = { .slot = &f.tgt, .action = NX_SLOT_REPLACE,
                                     .new_comp = &new_comp };
    struct recomp_plan plan = { .changes = &change, .num_changes = 1 };

    ASSERT_EQ_U(nx_recompose(&plan), NX_OK);

    /* Hold queue must be empty — flushed during slot_clear_pause. */
    ASSERT_EQ_U(nx_ipc_hold_queue_depth(&f.src, &f.tgt), 0);

    /* The flushed messages went through the async router into the inbox.
     * Dispatch to verify new_comp received them (old comp handles none). */
    ASSERT_EQ_U(nx_ipc_dispatch(&f.tgt, 10), 2);
    ASSERT_EQ_U(new_state.handle_calls,  2);
    ASSERT_EQ_U(f.tgt_state.handle_calls, 0);
}

/* ======================================================================== */
/* Test 5: CONN_ADD registers a new edge within the same plan               */
/* ======================================================================== */

TEST(recompose_conn_add_registers_edge)
{
    suite_reset();

    /* Two independent slots — no connection yet. */
    struct nx_slot      sa = { .name = "rc5_a", .iface = "x",
                               .mutability = NX_MUT_HOT, .concurrency = NX_CONC_SHARED };
    struct nx_slot      sb = { .name = "rc5_b", .iface = "x",
                               .mutability = NX_MUT_HOT, .concurrency = NX_CONC_SHARED };
    struct comp_state   sa_state = { 0 }, sb_state = { 0 };
    struct nx_component ca = { .manifest_id = "rc5a", .instance_id = "0",
                               .impl = &sa_state, .descriptor = &test_comp_desc };
    struct nx_component cb = { .manifest_id = "rc5b", .instance_id = "0",
                               .impl = &sb_state, .descriptor = &test_comp_desc };

    nx_slot_register(&sa);
    nx_slot_register(&sb);
    nx_component_register(&ca);
    nx_component_register(&cb);
    nx_slot_swap(&sa, &ca);
    nx_slot_swap(&sb, &cb);
    nx_component_init(&cb);
    nx_component_enable(&cb);

    size_t conn_before = nx_graph_connection_count();

    /* Plan: swap cb with a new comp AND add an sa→sb connection. */
    struct comp_state   new_state = { 0 };
    struct nx_component new_cb = { .manifest_id = "rc5new", .instance_id = "0",
                                   .impl = &new_state, .descriptor = &test_comp_desc };
    nx_component_register(&new_cb);
    nx_component_init(&new_cb);

    struct nx_slot_change change = { .slot = &sb, .action = NX_SLOT_REPLACE,
                                     .new_comp = &new_cb };
    struct nx_conn_change conn_add = { .from_slot = &sa, .to_slot = &sb,
                                       .action = NX_CONN_ADD,
                                       .mode = NX_CONN_ASYNC, .policy = NX_PAUSE_QUEUE };
    struct recomp_plan plan = {
        .changes         = &change,     .num_changes      = 1,
        .connections     = &conn_add,   .num_connections  = 1,
    };

    ASSERT_EQ_U(nx_recompose(&plan), NX_OK);
    ASSERT_EQ_U(nx_graph_connection_count(), conn_before + 1);
    ASSERT(sb.active == &new_cb);
}

/* ======================================================================== */
/* Test 6: pause-hook failure → plan rolled back, old comp stays ACTIVE     */
/* ======================================================================== */

TEST(recompose_rollback_on_pause_hook_fail)
{
    struct two_slot_fixture f;
    fixture_up(&f, "rc6_src", "rc6_tgt", NX_PAUSE_QUEUE);

    /* Make old comp's pause_hook return an error. */
    f.tgt_state.pause_hook_rc = NX_EDEADLINE;

    struct comp_state   new_state = { 0 };
    struct nx_component new_comp  = { .manifest_id = "recomp_new6",
                                      .instance_id = "0",
                                      .impl        = &new_state,
                                      .descriptor  = &test_comp_desc };
    nx_component_register(&new_comp);
    nx_component_init(&new_comp);

    struct nx_slot_change change = { .slot = &f.tgt, .action = NX_SLOT_REPLACE,
                                     .new_comp = &new_comp };
    struct recomp_plan plan = { .changes = &change, .num_changes = 1 };

    int rc = nx_recompose(&plan);
    ASSERT(rc != NX_OK);

    /* Rollback: old comp is still active, slot not swapped. */
    ASSERT(f.tgt.active == &f.tgt_comp);
    ASSERT_EQ_U(f.tgt_comp.state, NX_LC_ACTIVE);
    ASSERT_EQ_U(nx_slot_pause_state(&f.tgt), NX_SLOT_PAUSE_NONE);

    /* New comp was never enabled. */
    ASSERT_EQ_U(new_comp.state, NX_LC_READY);
}
