#include "test_runner.h"
#include "framework/component.h"
#include "framework/registry.h"

/*
 * Host-side tests for framework/component.c — the lifecycle state machine
 * layered on top of the registry's raw `nx_component_state_set`.  Each test
 * starts with `nx_graph_reset()` so state from the previous test is gone.
 */

#define COMP(N, MFEST)                                                  \
    struct nx_component N = { .manifest_id = MFEST, .instance_id = #N }

/* --- Transition matrix predicate --------------------------------------- */

TEST(matrix_allows_every_expected_legal_edge)
{
    ASSERT(nx_lifecycle_transition_legal(NX_LC_UNINIT, NX_LC_INIT));
    ASSERT(nx_lifecycle_transition_legal(NX_LC_UNINIT, NX_LC_READY));
    ASSERT(nx_lifecycle_transition_legal(NX_LC_INIT,   NX_LC_READY));
    ASSERT(nx_lifecycle_transition_legal(NX_LC_READY,  NX_LC_ACTIVE));
    ASSERT(nx_lifecycle_transition_legal(NX_LC_READY,  NX_LC_DESTROYED));
    ASSERT(nx_lifecycle_transition_legal(NX_LC_ACTIVE, NX_LC_PAUSED));
    ASSERT(nx_lifecycle_transition_legal(NX_LC_ACTIVE, NX_LC_READY));
    ASSERT(nx_lifecycle_transition_legal(NX_LC_PAUSED, NX_LC_ACTIVE));
    ASSERT(nx_lifecycle_transition_legal(NX_LC_PAUSED, NX_LC_READY));
}

TEST(matrix_rejects_known_illegal_edges)
{
    /* Skipping the state machine: UNINIT → ACTIVE is the canonical bug. */
    ASSERT(!nx_lifecycle_transition_legal(NX_LC_UNINIT, NX_LC_ACTIVE));
    ASSERT(!nx_lifecycle_transition_legal(NX_LC_UNINIT, NX_LC_PAUSED));
    ASSERT(!nx_lifecycle_transition_legal(NX_LC_UNINIT, NX_LC_DESTROYED));

    /* No shortcut from ACTIVE straight to DESTROYED — must disable first. */
    ASSERT(!nx_lifecycle_transition_legal(NX_LC_ACTIVE, NX_LC_DESTROYED));
    ASSERT(!nx_lifecycle_transition_legal(NX_LC_PAUSED, NX_LC_DESTROYED));

    /* No resurrection. */
    ASSERT(!nx_lifecycle_transition_legal(NX_LC_DESTROYED, NX_LC_READY));
    ASSERT(!nx_lifecycle_transition_legal(NX_LC_DESTROYED, NX_LC_ACTIVE));

    /* No going backwards. */
    ASSERT(!nx_lifecycle_transition_legal(NX_LC_READY,  NX_LC_UNINIT));
    ASSERT(!nx_lifecycle_transition_legal(NX_LC_ACTIVE, NX_LC_UNINIT));
    ASSERT(!nx_lifecycle_transition_legal(NX_LC_INIT,   NX_LC_UNINIT));

    /* No self-loops. */
    ASSERT(!nx_lifecycle_transition_legal(NX_LC_READY,  NX_LC_READY));
    ASSERT(!nx_lifecycle_transition_legal(NX_LC_ACTIVE, NX_LC_ACTIVE));
}

TEST(state_name_covers_every_enum)
{
    ASSERT(strcmp(nx_lifecycle_state_name(NX_LC_UNINIT),    "uninit")    == 0);
    ASSERT(strcmp(nx_lifecycle_state_name(NX_LC_INIT),      "init")      == 0);
    ASSERT(strcmp(nx_lifecycle_state_name(NX_LC_READY),     "ready")     == 0);
    ASSERT(strcmp(nx_lifecycle_state_name(NX_LC_ACTIVE),    "active")    == 0);
    ASSERT(strcmp(nx_lifecycle_state_name(NX_LC_PAUSED),    "paused")    == 0);
    ASSERT(strcmp(nx_lifecycle_state_name(NX_LC_DESTROYED), "destroyed") == 0);
    ASSERT(strcmp(nx_lifecycle_state_name((enum nx_lifecycle_state)99),
                  "unknown") == 0);
}

/* --- Individual verb success + rejection ------------------------------- */

TEST(init_drives_uninit_to_ready)
{
    nx_graph_reset();
    COMP(rr, "sched_rr");
    nx_component_register(&rr);
    ASSERT_EQ_U(rr.state, NX_LC_UNINIT);

    ASSERT_EQ_U(nx_component_init(&rr), NX_OK);
    ASSERT_EQ_U(rr.state, NX_LC_READY);

    /* Second init call is illegal — we're already past UNINIT. */
    ASSERT_EQ_U(nx_component_init(&rr), NX_ESTATE);
    ASSERT_EQ_U(rr.state, NX_LC_READY);
}

TEST(enable_requires_ready)
{
    nx_graph_reset();
    COMP(rr, "sched_rr");
    nx_component_register(&rr);

    /* UNINIT → ACTIVE not allowed. */
    ASSERT_EQ_U(nx_component_enable(&rr), NX_ESTATE);
    ASSERT_EQ_U(rr.state, NX_LC_UNINIT);

    nx_component_init(&rr);
    ASSERT_EQ_U(nx_component_enable(&rr), NX_OK);
    ASSERT_EQ_U(rr.state, NX_LC_ACTIVE);

    /* Double-enable rejected. */
    ASSERT_EQ_U(nx_component_enable(&rr), NX_ESTATE);
}

TEST(pause_and_resume_toggle_active_and_paused)
{
    nx_graph_reset();
    COMP(rr, "sched_rr");
    nx_component_register(&rr);
    nx_component_init(&rr);
    nx_component_enable(&rr);

    ASSERT_EQ_U(nx_component_pause(&rr), NX_OK);
    ASSERT_EQ_U(rr.state, NX_LC_PAUSED);

    /* Double-pause rejected. */
    ASSERT_EQ_U(nx_component_pause(&rr), NX_ESTATE);

    ASSERT_EQ_U(nx_component_resume(&rr), NX_OK);
    ASSERT_EQ_U(rr.state, NX_LC_ACTIVE);

    /* Resume from ACTIVE is illegal. */
    ASSERT_EQ_U(nx_component_resume(&rr), NX_ESTATE);
}

TEST(disable_accepts_active_and_paused)
{
    nx_graph_reset();
    COMP(rr, "sched_rr");
    nx_component_register(&rr);
    nx_component_init(&rr);
    nx_component_enable(&rr);

    /* ACTIVE → READY. */
    ASSERT_EQ_U(nx_component_disable(&rr), NX_OK);
    ASSERT_EQ_U(rr.state, NX_LC_READY);

    /* READY → disable is illegal. */
    ASSERT_EQ_U(nx_component_disable(&rr), NX_ESTATE);

    /* Go ACTIVE → PAUSED → READY via disable. */
    nx_component_enable(&rr);
    nx_component_pause(&rr);
    ASSERT_EQ_U(nx_component_disable(&rr), NX_OK);
    ASSERT_EQ_U(rr.state, NX_LC_READY);
}

TEST(destroy_requires_ready)
{
    nx_graph_reset();
    COMP(rr, "sched_rr");
    nx_component_register(&rr);
    nx_component_init(&rr);
    nx_component_enable(&rr);

    /* Cannot destroy from ACTIVE — must disable first. */
    ASSERT_EQ_U(nx_component_destroy(&rr), NX_ESTATE);
    ASSERT_EQ_U(rr.state, NX_LC_ACTIVE);

    nx_component_disable(&rr);
    ASSERT_EQ_U(nx_component_destroy(&rr), NX_OK);
    ASSERT_EQ_U(rr.state, NX_LC_DESTROYED);

    /* Everything fails from DESTROYED. */
    ASSERT_EQ_U(nx_component_init(&rr),    NX_ESTATE);
    ASSERT_EQ_U(nx_component_enable(&rr),  NX_ESTATE);
    ASSERT_EQ_U(nx_component_pause(&rr),   NX_ESTATE);
    ASSERT_EQ_U(nx_component_resume(&rr),  NX_ESTATE);
    ASSERT_EQ_U(nx_component_disable(&rr), NX_ESTATE);
    ASSERT_EQ_U(nx_component_destroy(&rr), NX_ESTATE);
}

/* --- Argument validation ---------------------------------------------- */

TEST(lifecycle_api_rejects_null_component)
{
    ASSERT_EQ_U(nx_component_init(NULL),    NX_EINVAL);
    ASSERT_EQ_U(nx_component_enable(NULL),  NX_EINVAL);
    ASSERT_EQ_U(nx_component_pause(NULL),   NX_EINVAL);
    ASSERT_EQ_U(nx_component_resume(NULL),  NX_EINVAL);
    ASSERT_EQ_U(nx_component_disable(NULL), NX_EINVAL);
    ASSERT_EQ_U(nx_component_destroy(NULL), NX_EINVAL);
}

TEST(lifecycle_api_rejects_unregistered_component)
{
    nx_graph_reset();
    struct nx_component orphan = { .manifest_id = "x", .instance_id = "y" };

    /* Transition-matrix check is satisfied (UNINIT → READY), but state_set
     * will not find the component in the registry → NX_ENOENT. */
    ASSERT_EQ_U(nx_component_init(&orphan), NX_ENOENT);
}

/* --- Event/change-log plumbing still fires ---------------------------- */

struct state_ev_cap {
    int                    count;
    enum nx_lifecycle_state last_from;
    enum nx_lifecycle_state last_to;
};

static void state_ev_cb(const struct nx_graph_event *ev, void *ctx)
{
    if (ev->type != NX_EV_COMPONENT_STATE) return;
    struct state_ev_cap *c = ctx;
    c->count++;
    c->last_from = ev->u.state.from;
    c->last_to   = ev->u.state.to;
}

TEST(lifecycle_transitions_emit_state_events_and_bump_gen)
{
    nx_graph_reset();
    COMP(rr, "sched_rr");
    nx_component_register(&rr);

    struct state_ev_cap cap = { 0 };
    nx_graph_subscribe(state_ev_cb, &cap);

    uint64_t g0 = nx_graph_generation();
    ASSERT_EQ_U(nx_component_init(&rr), NX_OK);
    ASSERT_EQ_U(cap.count, 1);
    ASSERT_EQ_U(cap.last_from, NX_LC_UNINIT);
    ASSERT_EQ_U(cap.last_to,   NX_LC_READY);
    ASSERT(nx_graph_generation() > g0);

    uint64_t g1 = nx_graph_generation();
    nx_component_enable(&rr);
    nx_component_pause(&rr);
    nx_component_resume(&rr);
    nx_component_disable(&rr);
    nx_component_destroy(&rr);
    ASSERT_EQ_U(cap.count, 6);
    ASSERT_EQ_U(cap.last_from, NX_LC_READY);
    ASSERT_EQ_U(cap.last_to,   NX_LC_DESTROYED);
    ASSERT(nx_graph_generation() > g1);

    /* Rejected transitions must NOT fire events or bump generation. */
    uint64_t g2 = nx_graph_generation();
    int count_before = cap.count;
    ASSERT_EQ_U(nx_component_enable(&rr), NX_ESTATE);
    ASSERT_EQ_U(cap.count, count_before);
    ASSERT_EQ_U(nx_graph_generation(), g2);
}

/* --- Happy-path full cycle -------------------------------------------- */

TEST(full_happy_path_runs_clean)
{
    nx_graph_reset();
    COMP(rr, "sched_rr");
    nx_component_register(&rr);

    ASSERT_EQ_U(nx_component_init(&rr),    NX_OK);
    ASSERT_EQ_U(nx_component_enable(&rr),  NX_OK);
    ASSERT_EQ_U(nx_component_pause(&rr),   NX_OK);
    ASSERT_EQ_U(nx_component_resume(&rr),  NX_OK);
    ASSERT_EQ_U(nx_component_disable(&rr), NX_OK);
    ASSERT_EQ_U(nx_component_destroy(&rr), NX_OK);
    ASSERT_EQ_U(rr.state, NX_LC_DESTROYED);
}

/* --- 100-cycle leaves no residue -------------------------------------- */

TEST(lifecycle_cycle_100_iterations_clean)
{
    nx_graph_reset();
    COMP(rr, "sched_rr");

    for (int i = 0; i < 100; i++) {
        rr.state = NX_LC_UNINIT;            /* caller-owned reset */
        ASSERT_EQ_U(nx_component_register(&rr),   NX_OK);
        ASSERT_EQ_U(nx_component_init(&rr),       NX_OK);
        ASSERT_EQ_U(nx_component_enable(&rr),     NX_OK);
        ASSERT_EQ_U(nx_component_disable(&rr),    NX_OK);
        ASSERT_EQ_U(nx_component_destroy(&rr),    NX_OK);
        ASSERT_EQ_U(nx_component_unregister(&rr), NX_OK);
    }

    ASSERT_EQ_U(nx_graph_component_count(), 0);
    ASSERT_EQ_U(nx_graph_slot_count(),      0);
}
