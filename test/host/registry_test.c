#include "test_runner.h"
#include "framework/registry.h"

/*
 * Host-side tests for framework/registry.c.  Each test calls nx_graph_reset()
 * up front so it starts on a clean global state regardless of what the
 * previous test left around.  Caller-owned slot/component structs are static
 * inside each test (or zero-initialised auto vars) — the registry only
 * bookkeeps pointers.
 */

#define SLOT(N, IFACE)                                                  \
    struct nx_slot N = { .name = #N, .iface = IFACE,                    \
                         .mutability = NX_MUT_HOT,                      \
                         .concurrency = NX_CONC_SHARED }

#define COMP(N, MFEST)                                                  \
    struct nx_component N = { .manifest_id = MFEST, .instance_id = #N }

/* --- Slot registration --------------------------------------------------- */

TEST(registry_starts_empty)
{
    nx_graph_reset();
    ASSERT_EQ_U(nx_graph_slot_count(), 0);
    ASSERT_EQ_U(nx_graph_component_count(), 0);
    ASSERT_EQ_U(nx_graph_connection_count(), 0);
    ASSERT_EQ_U(nx_graph_generation(), 0);
}

TEST(slot_register_and_lookup)
{
    nx_graph_reset();
    SLOT(scheduler, "scheduler");
    ASSERT_EQ_U(nx_slot_register(&scheduler), NX_OK);
    ASSERT_EQ_PTR(nx_slot_lookup("scheduler"), &scheduler);
    ASSERT_EQ_U(nx_graph_slot_count(), 1);
    ASSERT(nx_graph_generation() > 0);
}

TEST(slot_register_duplicate_name_fails)
{
    nx_graph_reset();
    SLOT(scheduler, "scheduler");
    struct nx_slot dup = { .name = "scheduler", .iface = "scheduler" };
    ASSERT_EQ_U(nx_slot_register(&scheduler), NX_OK);
    ASSERT_EQ_U(nx_slot_register(&dup),       NX_EEXIST);
    ASSERT_EQ_U(nx_graph_slot_count(), 1);
}

TEST(slot_register_null_inputs_fail)
{
    nx_graph_reset();
    ASSERT_EQ_U(nx_slot_register(NULL), NX_EINVAL);
    struct nx_slot anon = { .name = NULL, .iface = "x" };
    ASSERT_EQ_U(nx_slot_register(&anon), NX_EINVAL);
    ASSERT_EQ_U(nx_graph_slot_count(), 0);
}

TEST(slot_lookup_misses_return_null)
{
    nx_graph_reset();
    ASSERT_NULL(nx_slot_lookup("nope"));
}

TEST(slot_unregister_removes_entry)
{
    nx_graph_reset();
    SLOT(vfs, "vfs");
    nx_slot_register(&vfs);
    ASSERT_EQ_U(nx_slot_unregister(&vfs), NX_OK);
    ASSERT_NULL(nx_slot_lookup("vfs"));
    ASSERT_EQ_U(nx_graph_slot_count(), 0);
}

TEST(slot_unregister_with_active_connection_returns_ebusy)
{
    nx_graph_reset();
    SLOT(a, "a");
    SLOT(b, "b");
    nx_slot_register(&a);
    nx_slot_register(&b);
    int err = NX_OK;
    nx_connection_register(&a, &b, NX_CONN_ASYNC, false, NX_PAUSE_QUEUE, &err);
    ASSERT_EQ_U(err, NX_OK);

    /* Both endpoints are pinned by the live connection. */
    ASSERT_EQ_U(nx_slot_unregister(&a), NX_EBUSY);
    ASSERT_EQ_U(nx_slot_unregister(&b), NX_EBUSY);
}

/* --- Component registration --------------------------------------------- */

TEST(component_register_and_lookup)
{
    nx_graph_reset();
    COMP(rr, "sched_rr");
    ASSERT_EQ_U(nx_component_register(&rr), NX_OK);
    ASSERT_EQ_PTR(nx_component_lookup("sched_rr", "rr"), &rr);
    ASSERT_EQ_U(nx_graph_component_count(), 1);
}

TEST(component_register_duplicate_pair_fails)
{
    nx_graph_reset();
    COMP(rr, "sched_rr");
    struct nx_component dup = { .manifest_id = "sched_rr", .instance_id = "rr" };
    ASSERT_EQ_U(nx_component_register(&rr),  NX_OK);
    ASSERT_EQ_U(nx_component_register(&dup), NX_EEXIST);
}

TEST(component_register_distinct_instances_of_same_manifest)
{
    nx_graph_reset();
    struct nx_component a = { .manifest_id = "uart_pl011", .instance_id = "uart0" };
    struct nx_component b = { .manifest_id = "uart_pl011", .instance_id = "uart1" };
    ASSERT_EQ_U(nx_component_register(&a), NX_OK);
    ASSERT_EQ_U(nx_component_register(&b), NX_OK);
    ASSERT_EQ_U(nx_graph_component_count(), 2);
}

TEST(component_state_set_updates_state_and_bumps_gen)
{
    nx_graph_reset();
    COMP(rr, "sched_rr");
    nx_component_register(&rr);
    uint64_t before = nx_graph_generation();
    ASSERT_EQ_U(nx_component_state_set(&rr, NX_LC_ACTIVE), NX_OK);
    ASSERT_EQ_U(rr.state, NX_LC_ACTIVE);
    ASSERT(nx_graph_generation() > before);
}

TEST(component_unregister_blocked_while_active_in_a_slot)
{
    nx_graph_reset();
    SLOT(scheduler, "scheduler");
    COMP(rr, "sched_rr");
    nx_slot_register(&scheduler);
    nx_component_register(&rr);
    nx_slot_swap(&scheduler, &rr);

    ASSERT_EQ_U(nx_component_unregister(&rr), NX_EBUSY);

    /* Unbind first, then unregister works. */
    nx_slot_swap(&scheduler, NULL);
    ASSERT_EQ_U(nx_component_unregister(&rr), NX_OK);
}

/* --- Slot swap ---------------------------------------------------------- */

TEST(slot_swap_sets_active_and_bumps_gen)
{
    nx_graph_reset();
    SLOT(scheduler, "scheduler");
    COMP(rr, "sched_rr");
    nx_slot_register(&scheduler);
    nx_component_register(&rr);

    uint64_t before = nx_graph_generation();
    ASSERT_EQ_U(nx_slot_swap(&scheduler, &rr), NX_OK);
    ASSERT_EQ_PTR(scheduler.active, &rr);
    ASSERT(nx_graph_generation() > before);
}

TEST(slot_swap_to_unknown_component_fails)
{
    nx_graph_reset();
    SLOT(scheduler, "scheduler");
    COMP(rr, "sched_rr");   /* not registered */
    nx_slot_register(&scheduler);

    ASSERT_EQ_U(nx_slot_swap(&scheduler, &rr), NX_ENOENT);
    ASSERT_NULL(scheduler.active);
}

/* --- Connection register / retune / unregister -------------------------- */

TEST(connection_register_basic)
{
    nx_graph_reset();
    SLOT(posix, "posix_shim");
    SLOT(vfs,   "vfs");
    nx_slot_register(&posix);
    nx_slot_register(&vfs);

    int err = -42;
    struct nx_connection *c =
        nx_connection_register(&posix, &vfs, NX_CONN_ASYNC, false,
                               NX_PAUSE_QUEUE, &err);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ_U(err, NX_OK);
    ASSERT_EQ_PTR(c->from_slot, &posix);
    ASSERT_EQ_PTR(c->to_slot,   &vfs);
    ASSERT_EQ_U(nx_graph_connection_count(), 1);
    ASSERT(c->installed_gen > 0);
}

TEST(connection_register_with_null_from_is_a_boot_edge)
{
    nx_graph_reset();
    SLOT(scheduler, "scheduler");
    nx_slot_register(&scheduler);

    int err = NX_OK;
    struct nx_connection *c =
        nx_connection_register(NULL, &scheduler, NX_CONN_SYNC, false,
                               NX_PAUSE_QUEUE, &err);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ_U(err, NX_OK);
    ASSERT_NULL(c->from_slot);
    ASSERT_EQ_U(nx_graph_connection_count(), 1);
}

TEST(connection_register_null_to_fails)
{
    nx_graph_reset();
    int err = NX_OK;
    ASSERT_NULL(nx_connection_register(NULL, NULL, NX_CONN_ASYNC, false,
                                       NX_PAUSE_QUEUE, &err));
    ASSERT_EQ_U(err, NX_EINVAL);
}

TEST(connection_register_unknown_slot_fails)
{
    nx_graph_reset();
    SLOT(known, "k");
    nx_slot_register(&known);
    SLOT(stranger, "s");   /* not registered */

    int err = NX_OK;
    ASSERT_NULL(nx_connection_register(&known, &stranger, NX_CONN_ASYNC, false,
                                       NX_PAUSE_QUEUE, &err));
    ASSERT_EQ_U(err, NX_ENOENT);
}

TEST(connection_retune_changes_mode_and_stateful)
{
    nx_graph_reset();
    SLOT(a, "a"); SLOT(b, "b");
    nx_slot_register(&a); nx_slot_register(&b);

    int err = NX_OK;
    struct nx_connection *c =
        nx_connection_register(&a, &b, NX_CONN_ASYNC, false,
                               NX_PAUSE_QUEUE, &err);

    uint64_t before = nx_graph_generation();
    ASSERT_EQ_U(nx_connection_retune(c, NX_CONN_SYNC, true), NX_OK);
    ASSERT_EQ_U(c->mode, NX_CONN_SYNC);
    ASSERT(c->stateful);
    ASSERT(nx_graph_generation() > before);
}

TEST(connection_unregister_removes_from_lists)
{
    nx_graph_reset();
    SLOT(a, "a"); SLOT(b, "b");
    nx_slot_register(&a); nx_slot_register(&b);

    int err = NX_OK;
    struct nx_connection *c =
        nx_connection_register(&a, &b, NX_CONN_ASYNC, false,
                               NX_PAUSE_QUEUE, &err);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ_U(nx_graph_connection_count(), 1);

    ASSERT_EQ_U(nx_connection_unregister(c), NX_OK);
    ASSERT_EQ_U(nx_graph_connection_count(), 0);

    /* Slots are now free of edges, so unregister succeeds. */
    ASSERT_EQ_U(nx_slot_unregister(&a), NX_OK);
    ASSERT_EQ_U(nx_slot_unregister(&b), NX_OK);
}

/* --- Traversal ---------------------------------------------------------- */

struct count_ctx { size_t n; };

static void count_slot_cb(struct nx_slot *s, void *ctx)
{
    (void)s; ((struct count_ctx *)ctx)->n++;
}
static void count_comp_cb(struct nx_component *c, void *ctx)
{
    (void)c; ((struct count_ctx *)ctx)->n++;
}
static void count_conn_cb(struct nx_connection *c, void *ctx)
{
    (void)c; ((struct count_ctx *)ctx)->n++;
}

TEST(graph_foreach_visits_every_slot)
{
    nx_graph_reset();
    SLOT(s1, "i"); SLOT(s2, "i"); SLOT(s3, "i");
    nx_slot_register(&s1); nx_slot_register(&s2); nx_slot_register(&s3);

    struct count_ctx ctx = { 0 };
    nx_graph_foreach_slot(count_slot_cb, &ctx);
    ASSERT_EQ_U(ctx.n, 3);
}

TEST(graph_foreach_visits_every_component_and_connection)
{
    nx_graph_reset();
    SLOT(a, "a"); SLOT(b, "b"); SLOT(c, "c");
    COMP(impl, "impl");
    nx_slot_register(&a); nx_slot_register(&b); nx_slot_register(&c);
    nx_component_register(&impl);

    int err = NX_OK;
    nx_connection_register(&a, &b, NX_CONN_ASYNC, false, NX_PAUSE_QUEUE, &err);
    nx_connection_register(&a, &c, NX_CONN_ASYNC, false, NX_PAUSE_QUEUE, &err);

    struct count_ctx cc = { 0 };
    nx_graph_foreach_component(count_comp_cb, &cc);
    ASSERT_EQ_U(cc.n, 1);

    struct count_ctx ec = { 0 };
    nx_graph_foreach_connection(count_conn_cb, &ec);
    ASSERT_EQ_U(ec.n, 2);
}

TEST(slot_neighbourhood_walks_in_and_out_edges)
{
    nx_graph_reset();
    SLOT(posix, "posix_shim");
    SLOT(vfs,   "vfs");
    SLOT(blk,   "block_device");
    nx_slot_register(&posix);
    nx_slot_register(&vfs);
    nx_slot_register(&blk);

    int err = NX_OK;
    nx_connection_register(&posix, &vfs, NX_CONN_ASYNC, false, NX_PAUSE_QUEUE, &err);
    nx_connection_register(&vfs,   &blk, NX_CONN_ASYNC, false, NX_PAUSE_QUEUE, &err);

    /* Who depends on vfs? — posix. */
    struct count_ctx in = { 0 };
    nx_slot_foreach_dependent(&vfs, count_conn_cb, &in);
    ASSERT_EQ_U(in.n, 1);

    /* What does vfs depend on? — blk. */
    struct count_ctx out = { 0 };
    nx_slot_foreach_dependency(&vfs, count_conn_cb, &out);
    ASSERT_EQ_U(out.n, 1);

    /* posix has no dependents and one dependency. */
    struct count_ctx posix_in = { 0 };
    nx_slot_foreach_dependent(&posix, count_conn_cb, &posix_in);
    ASSERT_EQ_U(posix_in.n, 0);
    struct count_ctx posix_out = { 0 };
    nx_slot_foreach_dependency(&posix, count_conn_cb, &posix_out);
    ASSERT_EQ_U(posix_out.n, 1);
}

/* --- Generation counter ------------------------------------------------- */

TEST(generation_monotonically_increases)
{
    nx_graph_reset();
    uint64_t g0 = nx_graph_generation();

    SLOT(a, "a"); SLOT(b, "b");
    nx_slot_register(&a);    uint64_t g1 = nx_graph_generation();
    nx_slot_register(&b);    uint64_t g2 = nx_graph_generation();

    int err = NX_OK;
    struct nx_connection *c =
        nx_connection_register(&a, &b, NX_CONN_ASYNC, false, NX_PAUSE_QUEUE, &err);
    uint64_t g3 = nx_graph_generation();
    nx_connection_retune(c, NX_CONN_SYNC, false);
    uint64_t g4 = nx_graph_generation();
    nx_connection_unregister(c);
    uint64_t g5 = nx_graph_generation();

    ASSERT(g0 < g1 && g1 < g2 && g2 < g3 && g3 < g4 && g4 < g5);
}

/* --- Cycling: register/unregister N times leaves no residue ------------- */

TEST(register_unregister_cycle_leaves_no_residue)
{
    nx_graph_reset();
    SLOT(slot, "i");
    COMP(impl, "impl");

    for (int i = 0; i < 100; i++) {
        ASSERT_EQ_U(nx_slot_register(&slot),       NX_OK);
        ASSERT_EQ_U(nx_component_register(&impl),  NX_OK);
        int err = NX_OK;
        struct nx_connection *c =
            nx_connection_register(NULL, &slot, NX_CONN_ASYNC, false,
                                   NX_PAUSE_QUEUE, &err);
        ASSERT_NOT_NULL(c);

        ASSERT_EQ_U(nx_connection_unregister(c),   NX_OK);
        ASSERT_EQ_U(nx_component_unregister(&impl),NX_OK);
        ASSERT_EQ_U(nx_slot_unregister(&slot),     NX_OK);
    }
    ASSERT_EQ_U(nx_graph_slot_count(),       0);
    ASSERT_EQ_U(nx_graph_component_count(),  0);
    ASSERT_EQ_U(nx_graph_connection_count(), 0);
}
