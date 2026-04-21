#include "test_runner.h"
#include "framework/component.h"
#include "framework/registry.h"

#include "gen/sample_deps.h"
#include "gen/trivial_deps.h"

/*
 * Host-side tests for the slice 3.4 dependency-injection mechanism:
 *   - generator output compiles (these tests wouldn't link otherwise)
 *   - NX_COMPONENT_REGISTER produces a correct descriptor
 *   - nx_resolve_deps writes slot pointers and registers connection edges
 *   - missing required / missing optional are handled as specified
 *
 * Each test calls nx_graph_reset() so slot lookups and connection
 * counts start from a clean state.
 */

/* --- Sample component: has one required and one optional dep ---------- */

struct sample_state {
    struct sample_deps deps;    /* generated */
    int                scratch; /* rest of the state struct */
};

/* All-NULL ops table — this test exercises only the descriptor shape
 * and resolve_deps, not any ops-driven behaviour. */
static const struct nx_component_ops sample_ops = { 0 };

NX_COMPONENT_REGISTER(sample,
                      struct sample_state,
                      deps,
                      &sample_ops,
                      SAMPLE_DEPS_TABLE);

extern const struct nx_component_descriptor sample_descriptor;

/* --- Trivial component: no deps --------------------------------------- */

struct trivial_state {
    struct trivial_deps deps;   /* generated — contains placeholder */
    int                 whatever;
};

static const struct nx_component_ops trivial_ops = { 0 };

NX_COMPONENT_REGISTER_NO_DEPS(trivial,
                              struct trivial_state,
                              deps,
                              &trivial_ops);

extern const struct nx_component_descriptor trivial_descriptor;

/* --- Descriptor shape -------------------------------------------------- */

TEST(descriptor_fields_match_manifest)
{
    ASSERT(strcmp(sample_descriptor.name, "sample") == 0);
    ASSERT_EQ_U(sample_descriptor.state_size, sizeof(struct sample_state));
    ASSERT_EQ_U(sample_descriptor.deps_offset,
                offsetof(struct sample_state, deps));
    ASSERT_EQ_U(sample_descriptor.n_deps, 2);
    ASSERT_EQ_PTR(sample_descriptor.ops, &sample_ops);

    /* Deps are emitted in sorted order: "stats" after "timer" would be
     * alphabetical except that requires come before optionals. */
    const struct nx_dep_descriptor *d0 = &sample_descriptor.deps[0];
    const struct nx_dep_descriptor *d1 = &sample_descriptor.deps[1];

    ASSERT(strcmp(d0->name, "timer") == 0);
    ASSERT(d0->required);
    ASSERT(d0->version_req && strcmp(d0->version_req, ">=0.1.0") == 0);
    ASSERT_EQ_U(d0->mode,     NX_CONN_ASYNC);
    ASSERT_EQ_U(d0->stateful, false);
    ASSERT_EQ_U(d0->policy,   NX_PAUSE_QUEUE);
    ASSERT_EQ_U(d0->offset,
                offsetof(struct sample_state, deps.timer));

    ASSERT(strcmp(d1->name, "stats") == 0);
    ASSERT(!d1->required);
    ASSERT_NULL(d1->version_req);    /* fixture omits "version" for stats */
    ASSERT_EQ_U(d1->mode,     NX_CONN_SYNC);
    ASSERT_EQ_U(d1->stateful, true);
    ASSERT_EQ_U(d1->policy,   NX_PAUSE_QUEUE);
    ASSERT_EQ_U(d1->offset,
                offsetof(struct sample_state, deps.stats));
}

TEST(trivial_descriptor_has_zero_deps)
{
    ASSERT(strcmp(trivial_descriptor.name, "trivial") == 0);
    ASSERT_EQ_U(trivial_descriptor.n_deps, 0);
    ASSERT_NULL(trivial_descriptor.deps);
    ASSERT_EQ_U(trivial_descriptor.state_size, sizeof(struct trivial_state));
}

/* --- Resolution: happy path ------------------------------------------- */

TEST(resolve_deps_writes_pointers_and_registers_edges)
{
    nx_graph_reset();

    struct nx_slot self_slot = { .name = "scheduler", .iface = "scheduler" };
    struct nx_slot timer     = { .name = "timer",     .iface = "timer" };
    struct nx_slot stats     = { .name = "stats",     .iface = "stats" };
    ASSERT_EQ_U(nx_slot_register(&self_slot), NX_OK);
    ASSERT_EQ_U(nx_slot_register(&timer),     NX_OK);
    ASSERT_EQ_U(nx_slot_register(&stats),     NX_OK);

    struct sample_state state = { 0 };
    ASSERT_EQ_U(nx_resolve_deps(&sample_descriptor, &self_slot, &state),
                NX_OK);

    ASSERT_EQ_PTR(state.deps.timer, &timer);
    ASSERT_EQ_PTR(state.deps.stats, &stats);
    ASSERT_EQ_U(nx_graph_connection_count(), 2);
}

TEST(resolve_deps_with_optional_dep_missing_still_succeeds)
{
    nx_graph_reset();

    struct nx_slot self_slot = { .name = "scheduler", .iface = "scheduler" };
    struct nx_slot timer     = { .name = "timer",     .iface = "timer" };
    /* stats NOT registered. */
    nx_slot_register(&self_slot);
    nx_slot_register(&timer);

    struct sample_state state = { 0 };
    ASSERT_EQ_U(nx_resolve_deps(&sample_descriptor, &self_slot, &state),
                NX_OK);

    ASSERT_EQ_PTR(state.deps.timer, &timer);
    ASSERT_NULL(state.deps.stats);               /* optional missing */
    ASSERT_EQ_U(nx_graph_connection_count(), 1); /* only timer edge */
}

TEST(resolve_deps_with_missing_required_returns_enoent)
{
    nx_graph_reset();

    struct nx_slot self_slot = { .name = "scheduler", .iface = "scheduler" };
    /* timer NOT registered — required dep missing. */
    nx_slot_register(&self_slot);

    struct sample_state state = { 0 };
    ASSERT_EQ_U(nx_resolve_deps(&sample_descriptor, &self_slot, &state),
                NX_ENOENT);

    ASSERT_NULL(state.deps.timer);
    ASSERT_NULL(state.deps.stats);
    ASSERT_EQ_U(nx_graph_connection_count(), 0);
}

TEST(resolve_deps_with_null_self_slot_emits_boot_edges)
{
    nx_graph_reset();

    struct nx_slot timer = { .name = "timer", .iface = "timer" };
    nx_slot_register(&timer);

    struct sample_state state = { 0 };
    /* Boot edge: self_slot = NULL signals external/boot entry. */
    ASSERT_EQ_U(nx_resolve_deps(&sample_descriptor, NULL, &state), NX_OK);

    ASSERT_EQ_PTR(state.deps.timer, &timer);
    ASSERT_EQ_U(nx_graph_connection_count(), 1);
}

TEST(resolve_deps_on_trivial_descriptor_is_a_noop)
{
    nx_graph_reset();
    struct trivial_state state = { 0 };
    /* n_deps == 0: the loop body doesn't run, self_slot never matters. */
    ASSERT_EQ_U(nx_resolve_deps(&trivial_descriptor, NULL, &state), NX_OK);
    ASSERT_EQ_U(nx_graph_connection_count(), 0);
}

/* --- Argument validation ---------------------------------------------- */

TEST(resolve_deps_rejects_null_args)
{
    struct sample_state state = { 0 };
    ASSERT_EQ_U(nx_resolve_deps(NULL, NULL, &state),                NX_EINVAL);
    ASSERT_EQ_U(nx_resolve_deps(&sample_descriptor, NULL, NULL),    NX_EINVAL);
}

/* --- Connection edge wiring ------------------------------------------- */

struct conn_filter_ctx {
    struct nx_slot *from;
    struct nx_slot *to;
    int             count;
    enum nx_conn_mode mode;
    bool              stateful;
};

static void count_matching_conn(struct nx_connection *c, void *ctx)
{
    struct conn_filter_ctx *f = ctx;
    if (c->from_slot == f->from && c->to_slot == f->to) {
        f->count++;
        f->mode     = c->mode;
        f->stateful = c->stateful;
    }
}

TEST(resolve_deps_preserves_mode_and_stateful_from_manifest)
{
    nx_graph_reset();

    struct nx_slot self_slot = { .name = "scheduler", .iface = "scheduler" };
    struct nx_slot timer     = { .name = "timer",     .iface = "timer" };
    struct nx_slot stats     = { .name = "stats",     .iface = "stats" };
    nx_slot_register(&self_slot);
    nx_slot_register(&timer);
    nx_slot_register(&stats);

    struct sample_state state = { 0 };
    nx_resolve_deps(&sample_descriptor, &self_slot, &state);

    /* Timer: async + stateless. */
    struct conn_filter_ctx f_timer = {
        .from = &self_slot, .to = &timer, .count = 0,
    };
    nx_graph_foreach_connection(count_matching_conn, &f_timer);
    ASSERT_EQ_U(f_timer.count, 1);
    ASSERT_EQ_U(f_timer.mode,     NX_CONN_ASYNC);
    ASSERT_EQ_U(f_timer.stateful, false);

    /* Stats: sync + stateful (fixture overrides both defaults). */
    struct conn_filter_ctx f_stats = {
        .from = &self_slot, .to = &stats, .count = 0,
    };
    nx_graph_foreach_connection(count_matching_conn, &f_stats);
    ASSERT_EQ_U(f_stats.count, 1);
    ASSERT_EQ_U(f_stats.mode,     NX_CONN_SYNC);
    ASSERT_EQ_U(f_stats.stateful, true);
}
