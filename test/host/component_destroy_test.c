#include "test_runner.h"
#include "framework/component.h"
#include "framework/registry.h"

/*
 * Host-side tests for the slice-3.8 destroy guard.
 *
 * `nx_component_destroy` must refuse with NX_EBUSY if the component is
 * still bound as any slot's `active`.  The guard complements
 * `nx_component_unregister`'s long-standing check and makes
 * "destroy-while-bound" impossible regardless of call order.
 */

#define COMP(N, MFEST)                                                  \
    struct nx_component N = { .manifest_id = MFEST, .instance_id = #N }

TEST(destroy_refuses_while_still_bound_as_active)
{
    nx_graph_reset();

    struct nx_slot s = { .name = "s", .iface = "x" };
    COMP(rr, "sched_rr");
    nx_slot_register(&s);
    nx_component_register(&rr);

    /* Bind, then walk to READY without unbinding. */
    nx_slot_swap(&s, &rr);
    nx_component_init(&rr);
    nx_component_enable(&rr);
    nx_component_disable(&rr);                  /* ACTIVE → READY */

    ASSERT_EQ_U(rr.state, NX_LC_READY);
    ASSERT(nx_component_is_bound(&rr));

    /* Guard fires: registry still has s.active == &rr. */
    ASSERT_EQ_U(nx_component_destroy(&rr), NX_EBUSY);
    ASSERT_EQ_U(rr.state, NX_LC_READY);         /* state unchanged */
}

TEST(destroy_succeeds_after_unbind)
{
    nx_graph_reset();

    struct nx_slot s = { .name = "s", .iface = "x" };
    COMP(rr, "sched_rr");
    nx_slot_register(&s);
    nx_component_register(&rr);
    nx_slot_swap(&s, &rr);
    nx_component_init(&rr);
    nx_component_enable(&rr);
    nx_component_disable(&rr);

    /* Unbind — register-is-active-anywhere now returns false. */
    nx_slot_swap(&s, NULL);
    ASSERT(!nx_component_is_bound(&rr));

    ASSERT_EQ_U(nx_component_destroy(&rr), NX_OK);
    ASSERT_EQ_U(rr.state, NX_LC_DESTROYED);
}

TEST(destroy_on_never_bound_component_succeeds)
{
    /* Registered but never bound to a slot — guard is a no-op. */
    nx_graph_reset();
    COMP(rr, "sched_rr");
    nx_component_register(&rr);
    nx_component_init(&rr);                     /* UNINIT → READY */
    ASSERT(!nx_component_is_bound(&rr));
    ASSERT_EQ_U(nx_component_destroy(&rr), NX_OK);
    ASSERT_EQ_U(rr.state, NX_LC_DESTROYED);
}

TEST(destroy_guard_blocks_even_when_bound_to_multiple_slots)
{
    /* Contrived but legal: two slots both point at the same component
     * via swap(). `nx_component_is_bound` walks every slot; the guard
     * must catch either. */
    nx_graph_reset();

    struct nx_slot sa = { .name = "sa", .iface = "x" };
    struct nx_slot sb = { .name = "sb", .iface = "x" };
    COMP(rr, "multi");
    nx_slot_register(&sa);
    nx_slot_register(&sb);
    nx_component_register(&rr);
    nx_slot_swap(&sa, &rr);
    nx_slot_swap(&sb, &rr);
    nx_component_init(&rr);
    nx_component_enable(&rr);
    nx_component_disable(&rr);

    ASSERT_EQ_U(nx_component_destroy(&rr), NX_EBUSY);

    /* Unbind one — still bound to the other, still refused. */
    nx_slot_swap(&sa, NULL);
    ASSERT(nx_component_is_bound(&rr));
    ASSERT_EQ_U(nx_component_destroy(&rr), NX_EBUSY);

    /* Unbind the last one — guard releases. */
    nx_slot_swap(&sb, NULL);
    ASSERT_EQ_U(nx_component_destroy(&rr), NX_OK);
}
