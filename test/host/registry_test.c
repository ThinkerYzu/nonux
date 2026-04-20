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

/* --- Change events: subscriber dispatch --------------------------------- */

struct ev_capture {
    int    count;
    struct nx_graph_event last;
};

static void capture_cb(const struct nx_graph_event *ev, void *ctx)
{
    struct ev_capture *c = ctx;
    c->count++;
    c->last = *ev;
}

TEST(subscribe_then_register_slot_fires_slot_created_event)
{
    nx_graph_reset();
    struct ev_capture cap = { 0 };
    ASSERT_EQ_U(nx_graph_subscribe(capture_cb, &cap), NX_OK);

    SLOT(scheduler, "scheduler");
    nx_slot_register(&scheduler);

    ASSERT_EQ_U(cap.count, 1);
    ASSERT_EQ_U(cap.last.type, NX_EV_SLOT_CREATED);
    ASSERT(cap.last.u.slot.name && strcmp(cap.last.u.slot.name, "scheduler") == 0);
    ASSERT(cap.last.generation > 0);
}

TEST(subscribe_observes_full_event_taxonomy)
{
    nx_graph_reset();
    struct ev_capture cap = { 0 };
    nx_graph_subscribe(capture_cb, &cap);

    SLOT(scheduler, "scheduler");
    COMP(rr, "sched_rr");

    nx_slot_register(&scheduler);                       /* SLOT_CREATED */
    ASSERT_EQ_U(cap.last.type, NX_EV_SLOT_CREATED);

    nx_component_register(&rr);                         /* COMPONENT_REGISTERED */
    ASSERT_EQ_U(cap.last.type, NX_EV_COMPONENT_REGISTERED);
    ASSERT(strcmp(cap.last.u.comp.manifest_id, "sched_rr") == 0);

    nx_slot_swap(&scheduler, &rr);                      /* SLOT_SWAPPED */
    ASSERT_EQ_U(cap.last.type, NX_EV_SLOT_SWAPPED);
    ASSERT_NULL(cap.last.u.swap.old_manifest);
    ASSERT(strcmp(cap.last.u.swap.new_manifest, "sched_rr") == 0);

    nx_component_state_set(&rr, NX_LC_ACTIVE);          /* COMPONENT_STATE */
    ASSERT_EQ_U(cap.last.type, NX_EV_COMPONENT_STATE);
    ASSERT_EQ_U(cap.last.u.state.from, NX_LC_UNINIT);
    ASSERT_EQ_U(cap.last.u.state.to,   NX_LC_ACTIVE);

    int err = NX_OK;
    SLOT(vfs, "vfs");
    nx_slot_register(&vfs);                             /* SLOT_CREATED */
    struct nx_connection *c =
        nx_connection_register(&scheduler, &vfs, NX_CONN_ASYNC, false,
                               NX_PAUSE_QUEUE, &err);   /* CONNECTION_ADDED */
    ASSERT_EQ_U(cap.last.type, NX_EV_CONNECTION_ADDED);
    ASSERT(strcmp(cap.last.u.conn.from_slot, "scheduler") == 0);
    ASSERT(strcmp(cap.last.u.conn.to_slot,   "vfs") == 0);

    nx_connection_retune(c, NX_CONN_SYNC, true);        /* CONNECTION_RETUNED */
    ASSERT_EQ_U(cap.last.type, NX_EV_CONNECTION_RETUNED);
    ASSERT_EQ_U(cap.last.u.conn.mode, NX_CONN_SYNC);
    ASSERT(cap.last.u.conn.stateful);

    nx_connection_unregister(c);                        /* CONNECTION_REMOVED */
    ASSERT_EQ_U(cap.last.type, NX_EV_CONNECTION_REMOVED);

    nx_slot_swap(&scheduler, NULL);                     /* SLOT_SWAPPED */
    ASSERT_EQ_U(cap.last.type, NX_EV_SLOT_SWAPPED);
    ASSERT_NULL(cap.last.u.swap.new_manifest);

    nx_component_unregister(&rr);                       /* COMPONENT_UNREGISTERED */
    ASSERT_EQ_U(cap.last.type, NX_EV_COMPONENT_UNREGISTERED);

    nx_slot_unregister(&vfs);
    nx_slot_unregister(&scheduler);                     /* SLOT_DESTROYED */
    ASSERT_EQ_U(cap.last.type, NX_EV_SLOT_DESTROYED);

    /* 12 events: 2x SLOT_CREATED, COMPONENT_REGISTERED, 2x SLOT_SWAPPED,
     * COMPONENT_STATE, CONNECTION_ADDED/RETUNED/REMOVED,
     * COMPONENT_UNREGISTERED, 2x SLOT_DESTROYED. */
    ASSERT_EQ_U(cap.count, 12);
}

TEST(unsubscribe_stops_delivery)
{
    nx_graph_reset();
    struct ev_capture cap = { 0 };
    nx_graph_subscribe(capture_cb, &cap);

    SLOT(a, "a");
    nx_slot_register(&a);
    ASSERT_EQ_U(cap.count, 1);

    nx_graph_unsubscribe(capture_cb, &cap);
    nx_slot_unregister(&a);
    ASSERT_EQ_U(cap.count, 1);                          /* unchanged */
}

TEST(subscribe_duplicate_returns_eexist)
{
    nx_graph_reset();
    struct ev_capture cap = { 0 };
    ASSERT_EQ_U(nx_graph_subscribe(capture_cb, &cap), NX_OK);
    ASSERT_EQ_U(nx_graph_subscribe(capture_cb, &cap), NX_EEXIST);
}

TEST(subscribe_distinct_ctx_is_not_a_duplicate)
{
    nx_graph_reset();
    struct ev_capture a = { 0 }, b = { 0 };
    ASSERT_EQ_U(nx_graph_subscribe(capture_cb, &a), NX_OK);
    ASSERT_EQ_U(nx_graph_subscribe(capture_cb, &b), NX_OK);

    SLOT(s, "s");
    nx_slot_register(&s);
    ASSERT_EQ_U(a.count, 1);
    ASSERT_EQ_U(b.count, 1);
}

TEST(subscribe_null_callback_is_einval)
{
    nx_graph_reset();
    ASSERT_EQ_U(nx_graph_subscribe(NULL, NULL), NX_EINVAL);
}

TEST(unsubscribe_unknown_pair_is_a_noop)
{
    nx_graph_reset();
    struct ev_capture cap = { 0 };
    nx_graph_unsubscribe(capture_cb, &cap);             /* must not crash */
}

/* --- Change log: append + read + ring overflow --------------------------- */

TEST(change_log_starts_empty_after_reset)
{
    nx_graph_reset();
    ASSERT_EQ_U(nx_change_log_total(), 0);
    ASSERT_EQ_U(nx_change_log_size(),  0);
}

TEST(change_log_records_each_mutation)
{
    nx_graph_reset();
    SLOT(s, "s");
    nx_slot_register(&s);
    nx_slot_unregister(&s);

    ASSERT_EQ_U(nx_change_log_total(), 2);

    struct nx_graph_event evs[8];
    size_t n = nx_change_log_read(0, evs, 8);
    ASSERT_EQ_U(n, 2);
    ASSERT_EQ_U(evs[0].type, NX_EV_SLOT_CREATED);
    ASSERT_EQ_U(evs[1].type, NX_EV_SLOT_DESTROYED);
    ASSERT(evs[0].generation < evs[1].generation);
}

TEST(change_log_read_filters_by_since_gen)
{
    nx_graph_reset();
    SLOT(a, "a"); SLOT(b, "b"); SLOT(c, "c");
    nx_slot_register(&a);                  /* gen=1 */
    nx_slot_register(&b);                  /* gen=2 */
    uint64_t after_b = nx_graph_generation();
    nx_slot_register(&c);                  /* gen=3 */

    struct nx_graph_event evs[8];
    size_t n = nx_change_log_read(after_b, evs, 8);
    ASSERT_EQ_U(n, 1);
    ASSERT(strcmp(evs[0].u.slot.name, "c") == 0);
}

TEST(change_log_overflow_keeps_most_recent)
{
    nx_graph_reset();
    /* The log has fixed capacity (256).  Generate >capacity events and
     * confirm the OLDEST get dropped, NEWEST retained. */
    SLOT(s, "s");
    for (int i = 0; i < 300; i++) {
        nx_slot_register(&s);
        nx_slot_unregister(&s);
    }
    /* 600 events appended; ring holds the last 256. */
    ASSERT_EQ_U(nx_change_log_total(), 600);
    ASSERT_EQ_U(nx_change_log_size(),  256);

    struct nx_graph_event evs[256];
    size_t n = nx_change_log_read(0, evs, 256);
    ASSERT_EQ_U(n, 256);
    /* The oldest retained event has generation == 600 - 256 + 1 = 345. */
    ASSERT_EQ_U(evs[0].generation,   345);
    ASSERT_EQ_U(evs[255].generation, 600);
}

TEST(change_log_read_max_caps_output)
{
    nx_graph_reset();
    SLOT(s, "s");
    nx_slot_register(&s);
    nx_slot_unregister(&s);
    nx_slot_register(&s);
    nx_slot_unregister(&s);

    struct nx_graph_event evs[2];
    size_t n = nx_change_log_read(0, evs, 2);
    ASSERT_EQ_U(n, 2);          /* not 4 — capped at max */
}

TEST(change_log_read_with_null_or_zero_returns_zero)
{
    nx_graph_reset();
    SLOT(s, "s"); nx_slot_register(&s);
    struct nx_graph_event one;
    ASSERT_EQ_U(nx_change_log_read(0, NULL, 1), 0);
    ASSERT_EQ_U(nx_change_log_read(0, &one, 0), 0);
}

TEST(graph_reset_clears_subscribers_and_log)
{
    nx_graph_reset();
    struct ev_capture cap = { 0 };
    nx_graph_subscribe(capture_cb, &cap);

    SLOT(s, "s"); nx_slot_register(&s);
    ASSERT(nx_change_log_total() > 0);
    ASSERT_EQ_U(cap.count, 1);

    nx_graph_reset();
    ASSERT_EQ_U(nx_change_log_total(), 0);

    /* Subscriber gone — register again, count must stay at 1. */
    SLOT(s2, "s2"); nx_slot_register(&s2);
    ASSERT_EQ_U(cap.count, 1);
}

/* --- Snapshot ----------------------------------------------------------- */

TEST(snapshot_of_empty_registry_is_empty)
{
    nx_graph_reset();
    struct nx_graph_snapshot *s = nx_graph_snapshot_take();
    ASSERT_NOT_NULL(s);
    ASSERT_EQ_U(nx_graph_snapshot_generation(s),       0);
    ASSERT_EQ_U(nx_graph_snapshot_slot_count(s),       0);
    ASSERT_EQ_U(nx_graph_snapshot_component_count(s),  0);
    ASSERT_EQ_U(nx_graph_snapshot_connection_count(s), 0);
    nx_graph_snapshot_put(s);
}

TEST(snapshot_captures_current_state)
{
    nx_graph_reset();
    SLOT(scheduler, "scheduler");
    SLOT(vfs,       "vfs");
    COMP(rr,        "sched_rr");
    nx_slot_register(&scheduler);
    nx_slot_register(&vfs);
    nx_component_register(&rr);
    nx_slot_swap(&scheduler, &rr);
    int err = NX_OK;
    nx_connection_register(&scheduler, &vfs, NX_CONN_ASYNC, false,
                           NX_PAUSE_QUEUE, &err);

    struct nx_graph_snapshot *s = nx_graph_snapshot_take();
    ASSERT_EQ_U(nx_graph_snapshot_slot_count(s),       2);
    ASSERT_EQ_U(nx_graph_snapshot_component_count(s),  1);
    ASSERT_EQ_U(nx_graph_snapshot_connection_count(s), 1);

    /* Find the scheduler slot in the snapshot — order is unspecified. */
    bool saw_scheduler = false;
    for (size_t i = 0; i < 2; i++) {
        const struct nx_snapshot_slot *sl = nx_graph_snapshot_slot(s, i);
        if (strcmp(sl->name, "scheduler") == 0) {
            saw_scheduler = true;
            ASSERT(sl->active_manifest && strcmp(sl->active_manifest, "sched_rr") == 0);
            ASSERT(sl->active_instance && strcmp(sl->active_instance, "rr") == 0);
        }
    }
    ASSERT(saw_scheduler);
    nx_graph_snapshot_put(s);
}

TEST(snapshot_is_stable_against_subsequent_mutation)
{
    nx_graph_reset();
    SLOT(s1, "i"); SLOT(s2, "i");
    nx_slot_register(&s1);
    struct nx_graph_snapshot *snap = nx_graph_snapshot_take();
    uint64_t gen_at_take = nx_graph_snapshot_generation(snap);

    nx_slot_register(&s2);                      /* mutate after take */

    ASSERT_EQ_U(nx_graph_snapshot_slot_count(snap), 1);   /* still 1 */
    ASSERT_EQ_U(nx_graph_snapshot_generation(snap), gen_at_take);
    ASSERT(nx_graph_generation() > gen_at_take);

    nx_graph_snapshot_put(snap);
}

TEST(snapshot_refcount_keeps_alive_until_last_put)
{
    nx_graph_reset();
    SLOT(s, "s"); nx_slot_register(&s);

    struct nx_graph_snapshot *snap = nx_graph_snapshot_take();
    nx_graph_snapshot_retain(snap);   /* simulate a second holder */

    nx_graph_snapshot_put(snap);      /* refcount 1, still alive */
    ASSERT_EQ_U(nx_graph_snapshot_slot_count(snap), 1);
    nx_graph_snapshot_put(snap);      /* refcount 0 → freed */
    /* Past this point snap is invalid — don't dereference. */
}

TEST(snapshot_put_null_is_a_noop)
{
    nx_graph_snapshot_put(NULL);    /* must not crash */
}

TEST(snapshot_index_out_of_range_returns_null)
{
    nx_graph_reset();
    struct nx_graph_snapshot *s = nx_graph_snapshot_take();
    ASSERT_NULL(nx_graph_snapshot_slot(s, 0));
    ASSERT_NULL(nx_graph_snapshot_component(s, 0));
    ASSERT_NULL(nx_graph_snapshot_connection(s, 0));
    nx_graph_snapshot_put(s);
}

/* --- JSON serialization -------------------------------------------------- */

/* Tiny substring check — we don't pull in regex on the host. */
static bool contains(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

TEST(snapshot_to_json_empty_shape)
{
    nx_graph_reset();
    struct nx_graph_snapshot *s = nx_graph_snapshot_take();
    char buf[256];
    int n = nx_graph_snapshot_to_json(s, buf, sizeof buf);
    ASSERT(n > 0);
    ASSERT(contains(buf, "\"slots\":[]"));
    ASSERT(contains(buf, "\"components\":[]"));
    ASSERT(contains(buf, "\"connections\":[]"));
    ASSERT(contains(buf, "\"generation\":0"));
    nx_graph_snapshot_put(s);
}

TEST(snapshot_to_json_full_shape)
{
    nx_graph_reset();
    SLOT(scheduler, "scheduler");
    COMP(rr, "sched_rr");
    nx_slot_register(&scheduler);
    nx_component_register(&rr);
    nx_slot_swap(&scheduler, &rr);

    struct nx_graph_snapshot *s = nx_graph_snapshot_take();
    char buf[2048];
    int n = nx_graph_snapshot_to_json(s, buf, sizeof buf);
    ASSERT(n > 0);
    ASSERT(contains(buf, "\"name\":\"scheduler\""));
    ASSERT(contains(buf, "\"iface\":\"scheduler\""));
    ASSERT(contains(buf, "\"mutability\":\"hot\""));
    ASSERT(contains(buf, "\"concurrency\":\"shared\""));
    ASSERT(contains(buf, "\"manifest\":\"sched_rr\""));
    ASSERT(contains(buf, "\"instance\":\"rr\""));
    ASSERT(contains(buf, "\"state\":\"uninit\""));
    nx_graph_snapshot_put(s);
}

TEST(snapshot_to_json_truncated_returns_enomem)
{
    nx_graph_reset();
    SLOT(s, "s"); nx_slot_register(&s);
    struct nx_graph_snapshot *snap = nx_graph_snapshot_take();
    char tiny[16];
    int n = nx_graph_snapshot_to_json(snap, tiny, sizeof tiny);
    ASSERT_EQ_U((unsigned long long)n, (unsigned long long)NX_ENOMEM);
    /* Buffer is still NUL-terminated. */
    ASSERT_EQ_U(tiny[sizeof tiny - 1], 0);
    nx_graph_snapshot_put(snap);
}

TEST(snapshot_to_json_escapes_special_chars)
{
    nx_graph_reset();
    struct nx_slot s = { .name = "with\"quote",
                         .iface = "back\\slash",
                         .mutability  = NX_MUT_HOT,
                         .concurrency = NX_CONC_SHARED };
    nx_slot_register(&s);
    struct nx_graph_snapshot *snap = nx_graph_snapshot_take();
    char buf[1024];
    int n = nx_graph_snapshot_to_json(snap, buf, sizeof buf);
    ASSERT(n > 0);
    ASSERT(contains(buf, "with\\\"quote"));
    ASSERT(contains(buf, "back\\\\slash"));
    nx_graph_snapshot_put(snap);
}

TEST(change_log_to_json_emits_each_event)
{
    nx_graph_reset();
    SLOT(scheduler, "scheduler");
    COMP(rr, "sched_rr");
    nx_slot_register(&scheduler);
    nx_component_register(&rr);
    nx_slot_swap(&scheduler, &rr);

    char buf[4096];
    int n = nx_change_log_to_json(buf, sizeof buf);
    ASSERT(n > 0);
    ASSERT(contains(buf, "\"total\":3"));
    ASSERT(contains(buf, "\"held\":3"));
    ASSERT(contains(buf, "\"type\":\"slot_created\""));
    ASSERT(contains(buf, "\"type\":\"component_registered\""));
    ASSERT(contains(buf, "\"type\":\"slot_swapped\""));
    ASSERT(contains(buf, "\"new_manifest\":\"sched_rr\""));
}

TEST(change_log_to_json_handles_null_optional_strings)
{
    nx_graph_reset();
    SLOT(s, "s");
    nx_slot_register(&s);
    /* boot edge: from_slot is NULL */
    int err = NX_OK;
    nx_connection_register(NULL, &s, NX_CONN_SYNC, false, NX_PAUSE_QUEUE, &err);

    char buf[2048];
    int n = nx_change_log_to_json(buf, sizeof buf);
    ASSERT(n > 0);
    ASSERT(contains(buf, "\"from\":null"));
    ASSERT(contains(buf, "\"to\":\"s\""));
}
