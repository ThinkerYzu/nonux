/*
 * Host tests for components/sched_rr/ (slice 4.3).
 *
 * Three groups of coverage:
 *
 *   1. Conformance — seven TEST()s wrapping the universal helpers
 *      from test/host/conformance/conformance_scheduler.{h,c}.  If
 *      any fail, sched_rr is NOT allowed to bind to the scheduler
 *      slot; the build gate flips once Phase 8 adds `make verify-
 *      scheduler-conformance` or similar.
 *
 *   2. Lifecycle cycling — 100× init→enable→disable→destroy with
 *      zero residue, observable via the component's internal
 *      counters.  The mem_track layer fails the test if anything
 *      leaks.
 *
 *   3. Hook enum smoke test — confirms NX_HOOK_CONTEXT_SWITCH is
 *      registered as a valid hook point with chain length 0.  No
 *      dispatch yet (that's slice 4.4).
 */

#include "test_runner.h"

#include "conformance/conformance_scheduler.h"
#include "framework/component.h"
#include "framework/hook.h"
#include "framework/registry.h"
#include "interfaces/scheduler.h"

#include <stdlib.h>
#include <string.h>

/* Exported by components/sched_rr/sched_rr.c. */
extern const struct nx_scheduler_ops     sched_rr_scheduler_ops;
extern const struct nx_component_ops     sched_rr_component_ops;
extern const struct nx_component_descriptor sched_rr_descriptor;

/*
 * The component's state struct is deliberately private.  Tests here
 * therefore stick to externally-observable behaviour: ops return
 * codes, runqueue behaviour via `scheduler_ops`, memory-tracking
 * layer's leak detection.  Kernel-side ktest_sched_bootstrap mirrors
 * the struct locally (same technique as ktest_bootstrap.c for
 * uart_pl011) to peek at counters on the bound instance.
 */

/* --- factory used by the conformance harness --- */

static void *sched_rr_fixture_create(void)
{
    void *state = calloc(1, sched_rr_descriptor.state_size);
    if (!state) return NULL;
    if (sched_rr_component_ops.init(state) != NX_OK) {
        free(state);
        return NULL;
    }
    return state;
}

static void sched_rr_fixture_destroy(void *self)
{
    sched_rr_component_ops.destroy(self);
    free(self);
}

static const struct nx_scheduler_fixture sched_rr_fixture = {
    .ops     = &sched_rr_scheduler_ops,
    .create  = sched_rr_fixture_create,
    .destroy = sched_rr_fixture_destroy,
};

/* --- 1. Conformance --------------------------------------------------- */

TEST(sched_rr_conformance_pick_on_empty_returns_null)
{
    nx_conformance_scheduler_pick_on_empty_returns_null(&sched_rr_fixture);
}

TEST(sched_rr_conformance_enqueue_then_pick_returns_task)
{
    nx_conformance_scheduler_enqueue_then_pick_returns_task(&sched_rr_fixture);
}

TEST(sched_rr_conformance_dequeue_removes)
{
    nx_conformance_scheduler_dequeue_removes(&sched_rr_fixture);
}

TEST(sched_rr_conformance_dequeue_nonexistent_returns_enoent)
{
    nx_conformance_scheduler_dequeue_nonexistent_returns_enoent(&sched_rr_fixture);
}

TEST(sched_rr_conformance_set_priority_returns_consistent_status)
{
    nx_conformance_scheduler_set_priority_returns_consistent_status(&sched_rr_fixture);
}

TEST(sched_rr_conformance_roundtrip_100_residue_free)
{
    nx_conformance_scheduler_roundtrip_100_residue_free(&sched_rr_fixture);
}

TEST(sched_rr_conformance_borrow_preserves_task_pointer)
{
    nx_conformance_scheduler_borrow_preserves_task_pointer(&sched_rr_fixture);
}

/* --- 2. Lifecycle cycling -------------------------------------------- */

TEST(sched_rr_lifecycle_100_cycles_leave_no_residue)
{
    /* Exercise the full init → enable → disable → destroy cycle 100
     * times against a single state buffer, asserting each verb
     * returns NX_OK.  If sched_rr ever allocates per-enable state
     * without freeing it in disable/destroy, the mem_track layer
     * surfaces a leak at test-end. */
    void *state = calloc(1, sched_rr_descriptor.state_size);
    ASSERT_NOT_NULL(state);

    enum { N = 100 };
    for (int i = 0; i < N; i++) {
        ASSERT_EQ_U(sched_rr_component_ops.init(state),    NX_OK);
        ASSERT_EQ_U(sched_rr_component_ops.enable(state),  NX_OK);
        ASSERT_EQ_U(sched_rr_component_ops.disable(state), NX_OK);
        sched_rr_component_ops.destroy(state);
    }

    free(state);
}

TEST(sched_rr_init_yields_pickable_empty_queue)
{
    /* After a fresh init the queue must be empty and pick_next must
     * return NULL — proves init did the nx_list_init() initialisation
     * of the runqueue (without inspecting the private struct shape). */
    void *state = calloc(1, sched_rr_descriptor.state_size);
    ASSERT_NOT_NULL(state);

    ASSERT_EQ_U(sched_rr_component_ops.init(state), NX_OK);
    ASSERT_NULL(sched_rr_scheduler_ops.pick_next(state));

    sched_rr_component_ops.destroy(state);
    free(state);
}

/* --- 3. Hook enum smoke test ----------------------------------------- */

static enum nx_hook_action context_switch_noop_hook(struct nx_hook_context *c,
                                                    void *user)
{
    (void)c; (void)user;
    return NX_HOOK_CONTINUE;
}

TEST(sched_rr_hook_context_switch_point_is_empty_initially)
{
    /* NX_HOOK_CONTEXT_SWITCH enum added in slice 4.3; no dispatch
     * site fires it until 4.4.  Chain length must start at zero and
     * the point must be within NX_HOOK_POINT_COUNT (otherwise
     * nx_hook_chain_length returns 0 due to the bounds check, which
     * would hide a bad enum addition).  Register a no-op hook to
     * prove the chain accepts the point, then unregister. */
    ASSERT(NX_HOOK_CONTEXT_SWITCH < NX_HOOK_POINT_COUNT);
    ASSERT_EQ_U(nx_hook_chain_length(NX_HOOK_CONTEXT_SWITCH), 0);

    static struct nx_hook h;
    h.point    = NX_HOOK_CONTEXT_SWITCH;
    h.priority = 0;
    h.fn       = context_switch_noop_hook;
    h.user     = NULL;
    h.name     = "slice_4_3_smoke";

    ASSERT_EQ_U(nx_hook_register(&h), NX_OK);
    ASSERT_EQ_U(nx_hook_chain_length(NX_HOOK_CONTEXT_SWITCH), 1);

    nx_hook_unregister(&h);
    ASSERT_EQ_U(nx_hook_chain_length(NX_HOOK_CONTEXT_SWITCH), 0);
}
