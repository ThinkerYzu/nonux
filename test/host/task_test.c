/*
 * Host-side tests for core/sched/task.{c,h} + core/sched/sched.c.
 *
 * Host cannot exercise the ARM64 context-switch assembly (cpu_switch_to
 * lives in context.S and needs ARM64 registers / TPIDR_EL1).  The
 * round-trip + callee-saved tests live in test/kernel/ktest_context.c.
 *
 * What we verify here:
 *   - nx_task_create populates cpu_ctx so the first switch would land at
 *     entry(arg): x19 == entry, x20 == arg, SP inside the allocated
 *     kstack, 16-byte aligned.
 *   - Task name is copied with truncation.
 *   - Each task gets a unique nonzero id.
 *   - nx_task_destroy releases everything (no leaks via mem_track).
 *   - preempt_disable / enable nest correctly on the host-stubbed
 *     "current" task.
 *   - Per-task `caller_slot` (slice 8.0a.5) registers + clones edges
 *     when posix_shim is bound, soft-skips otherwise.
 *
 * Slice 8.0a.5 made `nx_task_create` read the registry (to look up
 * `posix_shim`).  Earlier component tests deliberately leave
 * stack-allocated `nx_slot` structs in the registry past their
 * stack frame's lifetime, so every test here calls `nx_graph_reset()`
 * up front to start from a clean slate.
 */

#include "test_runner.h"

#include "core/sched/task.h"
#include "core/sched/sched.h"
#include "core/sched/waitq.h"
#include "framework/component.h"
#include "framework/registry.h"

static void dummy_entry(void *arg) { (void)arg; }

TEST(task_create_populates_cpu_ctx_for_first_switch)
{
    nx_graph_reset();
    int arg_sentinel = 0;
    struct nx_task *t = nx_task_create("t0", dummy_entry, &arg_sentinel, 1);
    ASSERT_NOT_NULL(t);

    ASSERT_EQ_U(t->cpu_ctx.x19, (uint64_t)(uintptr_t)dummy_entry);
    ASSERT_EQ_U(t->cpu_ctx.x20, (uint64_t)(uintptr_t)&arg_sentinel);

    uintptr_t sp_top = (uintptr_t)t->kstack_base + t->kstack_size;
    sp_top &= ~(uintptr_t)0xf;
    ASSERT_EQ_U(t->cpu_ctx.sp, (uint64_t)sp_top);
    ASSERT_EQ_U(t->cpu_ctx.sp & 0xf, 0);

    ASSERT_EQ_U(t->state, NX_TASK_READY);
    ASSERT_EQ_U(t->preempt_count, 0);
    ASSERT_EQ_U(t->need_resched, 0);

    nx_task_destroy(t);
}

TEST(task_create_truncates_long_name)
{
    nx_graph_reset();
    /* Name longer than NX_TASK_NAME_MAX must truncate with a NUL at the
     * last slot. */
    const char *long_name = "abcdefghijklmnopqrstuvwxyz";
    struct nx_task *t = nx_task_create(long_name, dummy_entry, NULL, 1);
    ASSERT_NOT_NULL(t);

    ASSERT_EQ_U(t->name[NX_TASK_NAME_MAX - 1], '\0');
    /* First NX_TASK_NAME_MAX-1 bytes match the long name. */
    for (int i = 0; i < NX_TASK_NAME_MAX - 1; i++)
        ASSERT_EQ_U(t->name[i], long_name[i]);

    nx_task_destroy(t);
}

TEST(task_create_assigns_unique_nonzero_ids)
{
    nx_graph_reset();
    struct nx_task *a = nx_task_create("a", dummy_entry, NULL, 1);
    struct nx_task *b = nx_task_create("b", dummy_entry, NULL, 1);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);

    ASSERT(a->id != 0);
    ASSERT(b->id != 0);
    ASSERT(a->id != b->id);

    nx_task_destroy(a);
    nx_task_destroy(b);
}

TEST(task_create_rejects_null_entry)
{
    nx_graph_reset();
    struct nx_task *t = nx_task_create("bad", NULL, NULL, 1);
    ASSERT_NULL(t);
}

TEST(preempt_disable_and_enable_nest_on_current)
{
    nx_graph_reset();
    struct nx_task *t = nx_task_create("ctl", dummy_entry, NULL, 1);
    ASSERT_NOT_NULL(t);

    nx_task_set_current_for_test(t);
    ASSERT_EQ_U(nx_preempt_count(), 0);

    nx_preempt_disable();
    ASSERT_EQ_U(nx_preempt_count(), 1);
    nx_preempt_disable();
    ASSERT_EQ_U(nx_preempt_count(), 2);
    nx_preempt_enable();
    ASSERT_EQ_U(nx_preempt_count(), 1);
    nx_preempt_enable();
    ASSERT_EQ_U(nx_preempt_count(), 0);

    /* Underflow guard — enable with zero count stays at zero. */
    nx_preempt_enable();
    ASSERT_EQ_U(nx_preempt_count(), 0);

    nx_task_set_current_for_test(NULL);
    nx_task_destroy(t);
}

TEST(preempt_count_without_current_is_zero)
{
    nx_task_set_current_for_test(NULL);
    ASSERT_EQ_U(nx_preempt_count(), 0);
    /* These calls must be safe with no current — they early-return. */
    nx_preempt_disable();
    nx_preempt_enable();
    ASSERT_EQ_U(nx_preempt_count(), 0);
}

/* --- slice 8.0a.5: per-task caller_slot lifecycle ------------------- */
/*
 * Fixture: synthesize a "posix_shim"-shaped composition the way a
 * real boot would leave it — a `posix_shim` slot bound to a
 * registered component, plus N service slots (vfs / scheduler /
 * memory.page_alloc / char_device.serial) and `posix_shim → svc`
 * edges with the same mode/stateful/policy as the production
 * manifest.  Every test in this group calls `setup_posix_shim_fixture`
 * after `nx_graph_reset` so each test starts from a clean
 * fixture-only registry.
 */

#define POSIX_SHIM_DEP_COUNT 4

struct posix_shim_fixture {
    struct nx_slot      posix_slot;
    struct nx_slot      svc_slots[POSIX_SHIM_DEP_COUNT];
    struct nx_component posix_comp;
};

static void setup_posix_shim_fixture(struct posix_shim_fixture *f)
{
    /* Use string literals so name pointers survive the test's stack
     * frame (the registry stores them by reference).  These match the
     * manifest in components/posix_shim/manifest.json. */
    static const char *const dep_names[POSIX_SHIM_DEP_COUNT] = {
        "vfs", "scheduler", "memory.page_alloc", "char_device.serial",
    };
    static const enum nx_conn_mode    dep_modes [POSIX_SHIM_DEP_COUNT] = {
        NX_CONN_ASYNC, NX_CONN_ASYNC, NX_CONN_ASYNC, NX_CONN_ASYNC,
    };
    static const bool                 dep_stful [POSIX_SHIM_DEP_COUNT] = {
        true, false, false, false,    /* vfs is stateful per manifest */
    };
    static const enum nx_pause_policy dep_polcy [POSIX_SHIM_DEP_COUNT] = {
        NX_PAUSE_QUEUE, NX_PAUSE_QUEUE, NX_PAUSE_QUEUE, NX_PAUSE_QUEUE,
    };

    f->posix_slot = (struct nx_slot){
        .name        = "posix_shim",
        .iface       = "posix_shim",
        .mutability  = NX_MUT_HOT,
        .concurrency = NX_CONC_SHARED,
    };
    f->posix_comp = (struct nx_component){
        .manifest_id = "posix_shim",
        .instance_id = "0",
        .state       = NX_LC_ACTIVE,
    };
    ASSERT_EQ_U(nx_slot_register(&f->posix_slot),       NX_OK);
    ASSERT_EQ_U(nx_component_register(&f->posix_comp),  NX_OK);
    ASSERT_EQ_U(nx_slot_swap(&f->posix_slot, &f->posix_comp), NX_OK);

    for (int i = 0; i < POSIX_SHIM_DEP_COUNT; i++) {
        f->svc_slots[i] = (struct nx_slot){
            .name        = dep_names[i],
            .iface       = "svc",
            .mutability  = NX_MUT_HOT,
            .concurrency = NX_CONC_SHARED,
        };
        ASSERT_EQ_U(nx_slot_register(&f->svc_slots[i]), NX_OK);
        int err = NX_OK;
        struct nx_connection *c = nx_connection_register(
            &f->posix_slot, &f->svc_slots[i],
            dep_modes[i], dep_stful[i], dep_polcy[i], &err);
        ASSERT_NOT_NULL(c);
        ASSERT_EQ_U(err, NX_OK);
    }
}

TEST(caller_slot_skipped_when_posix_shim_absent)
{
    /* Plain task_create with an empty registry must not register
     * anything — host tests that don't run framework_bootstrap rely
     * on this soft-skip. */
    nx_graph_reset();
    size_t slots_before = nx_graph_slot_count();
    size_t conns_before = nx_graph_connection_count();

    struct nx_task *t = nx_task_create("nopshim", dummy_entry, NULL, 1);
    ASSERT_NOT_NULL(t);
    ASSERT(t->caller_slot_active == false);
    ASSERT_EQ_U(nx_graph_slot_count(),       slots_before);
    ASSERT_EQ_U(nx_graph_connection_count(), conns_before);

    nx_task_destroy(t);
    ASSERT_EQ_U(nx_graph_slot_count(),       slots_before);
    ASSERT_EQ_U(nx_graph_connection_count(), conns_before);
}

TEST(caller_slot_registers_and_clones_edges_when_posix_shim_bound)
{
    nx_graph_reset();
    struct posix_shim_fixture f;
    setup_posix_shim_fixture(&f);

    size_t slots_before = nx_graph_slot_count();
    size_t conns_before = nx_graph_connection_count();

    struct nx_task *t = nx_task_create("svc", dummy_entry, NULL, 1);
    ASSERT_NOT_NULL(t);
    ASSERT(t->caller_slot_active == true);

    /* +1 slot (the caller_slot itself), +POSIX_SHIM_DEP_COUNT edges
     * (one parallel clone per posix_shim outgoing edge). */
    ASSERT_EQ_U(nx_graph_slot_count(),       slots_before + 1);
    ASSERT_EQ_U(nx_graph_connection_count(), conns_before + POSIX_SHIM_DEP_COUNT);

    /* Slot is bound to the same component instance as posix_shim. */
    ASSERT(t->caller_slot.active == &f.posix_comp);

    /* Slot looks up by its synthesized name (`task#<id>`). */
    struct nx_slot *looked_up = nx_slot_lookup(t->caller_slot_name);
    ASSERT(looked_up == &t->caller_slot);

    nx_task_destroy(t);
    /* Tear-down restores both counts. */
    ASSERT_EQ_U(nx_graph_slot_count(),       slots_before);
    ASSERT_EQ_U(nx_graph_connection_count(), conns_before);
}

/* Walk-helper context for caller_slot_inherits_edge_attributes. */
struct edge_capture {
    struct nx_connection *edges[POSIX_SHIM_DEP_COUNT * 2];
    size_t                n;
};
static void capture_edge_cb(struct nx_connection *c, void *ctx)
{
    struct edge_capture *ec = ctx;
    if (ec->n < sizeof ec->edges / sizeof ec->edges[0])
        ec->edges[ec->n++] = c;
}

TEST(caller_slot_inherits_edge_attributes)
{
    nx_graph_reset();
    struct posix_shim_fixture f;
    setup_posix_shim_fixture(&f);

    struct nx_task *t = nx_task_create("attrs", dummy_entry, NULL, 1);
    ASSERT_NOT_NULL(t);

    struct edge_capture parent = { 0 }, child = { 0 };
    nx_slot_foreach_dependency(&f.posix_slot,    capture_edge_cb, &parent);
    nx_slot_foreach_dependency(&t->caller_slot,  capture_edge_cb, &child);

    ASSERT_EQ_U(parent.n, POSIX_SHIM_DEP_COUNT);
    ASSERT_EQ_U(child.n,  POSIX_SHIM_DEP_COUNT);

    /* For every parent edge, find a child edge with the same to_slot
     * and identical mode/stateful/policy.  Order isn't guaranteed —
     * both lists are LIFO under the registry's chain semantics. */
    for (size_t i = 0; i < parent.n; i++) {
        bool found = false;
        for (size_t j = 0; j < child.n; j++) {
            if (child.edges[j]->to_slot != parent.edges[i]->to_slot) continue;
            ASSERT_EQ_U(child.edges[j]->mode,     parent.edges[i]->mode);
            ASSERT(child.edges[j]->stateful  ==   parent.edges[i]->stateful);
            ASSERT_EQ_U(child.edges[j]->policy,   parent.edges[i]->policy);
            ASSERT(child.edges[j]->from_slot == &t->caller_slot);
            found = true;
            break;
        }
        ASSERT(found);
    }

    nx_task_destroy(t);
}

TEST(caller_slot_multiple_tasks_each_get_independent_slot_and_edges)
{
    nx_graph_reset();
    struct posix_shim_fixture f;
    setup_posix_shim_fixture(&f);

    size_t slots_before = nx_graph_slot_count();
    size_t conns_before = nx_graph_connection_count();

    struct nx_task *a = nx_task_create("a", dummy_entry, NULL, 1);
    struct nx_task *b = nx_task_create("b", dummy_entry, NULL, 1);
    struct nx_task *c = nx_task_create("c", dummy_entry, NULL, 1);
    ASSERT_NOT_NULL(a); ASSERT_NOT_NULL(b); ASSERT_NOT_NULL(c);

    /* Three new slots, 3*POSIX_SHIM_DEP_COUNT new edges. */
    ASSERT_EQ_U(nx_graph_slot_count(),
                slots_before + 3);
    ASSERT_EQ_U(nx_graph_connection_count(),
                conns_before + 3 * POSIX_SHIM_DEP_COUNT);

    /* Names are distinct (task#<id> with monotonic id). */
    ASSERT(strcmp(a->caller_slot_name, b->caller_slot_name) != 0);
    ASSERT(strcmp(b->caller_slot_name, c->caller_slot_name) != 0);
    ASSERT(strcmp(a->caller_slot_name, c->caller_slot_name) != 0);

    /* Destroy in reverse order — each unwires its own edges + slot. */
    nx_task_destroy(c);
    ASSERT_EQ_U(nx_graph_slot_count(),       slots_before + 2);
    ASSERT_EQ_U(nx_graph_connection_count(), conns_before + 2 * POSIX_SHIM_DEP_COUNT);
    nx_task_destroy(b);
    ASSERT_EQ_U(nx_graph_slot_count(),       slots_before + 1);
    ASSERT_EQ_U(nx_graph_connection_count(), conns_before + 1 * POSIX_SHIM_DEP_COUNT);
    nx_task_destroy(a);
    ASSERT_EQ_U(nx_graph_slot_count(),       slots_before);
    ASSERT_EQ_U(nx_graph_connection_count(), conns_before);
}

TEST(caller_slot_destroy_after_partial_fixture_no_registry_leak)
{
    /* Defensive coverage for the rollback path: posix_shim's slot is
     * bound but has zero outgoing edges (the rare bring-up window
     * before deps are wired).  task_create still succeeds — the
     * clone-edges loop has nothing to clone — and task_destroy must
     * unregister the caller_slot cleanly. */
    nx_graph_reset();
    struct nx_slot      posix_slot = {
        .name = "posix_shim", .iface = "posix_shim",
        .mutability = NX_MUT_HOT, .concurrency = NX_CONC_SHARED,
    };
    struct nx_component posix_comp = {
        .manifest_id = "posix_shim", .instance_id = "0",
        .state = NX_LC_ACTIVE,
    };
    ASSERT_EQ_U(nx_slot_register(&posix_slot),         NX_OK);
    ASSERT_EQ_U(nx_component_register(&posix_comp),    NX_OK);
    ASSERT_EQ_U(nx_slot_swap(&posix_slot, &posix_comp), NX_OK);

    size_t slots_before = nx_graph_slot_count();
    size_t conns_before = nx_graph_connection_count();

    struct nx_task *t = nx_task_create("noedges", dummy_entry, NULL, 1);
    ASSERT_NOT_NULL(t);
    ASSERT(t->caller_slot_active == true);
    ASSERT_EQ_U(nx_graph_slot_count(),       slots_before + 1);
    ASSERT_EQ_U(nx_graph_connection_count(), conns_before);

    nx_task_destroy(t);
    ASSERT_EQ_U(nx_graph_slot_count(),       slots_before);
    ASSERT_EQ_U(nx_graph_connection_count(), conns_before);
}

TEST(caller_slot_reply_waitq_initialized_for_blocking_call_path)
{
    /* Slice 8.0a.6 will park the task on `reply_waitq` while waiting
     * for a dispatcher reply; assert the list head is self-linked
     * (init'd) and the in-flight reply slot is empty before any call. */
    nx_graph_reset();
    struct nx_task *t = nx_task_create("rwq", dummy_entry, NULL, 1);
    ASSERT_NOT_NULL(t);

    /* Self-linked empty list — `nx_waitq_init` writes prev=next=&head. */
    ASSERT(t->reply_waitq.waiters.n.next == &t->reply_waitq.waiters.n);
    ASSERT(t->reply_waitq.waiters.n.prev == &t->reply_waitq.waiters.n);

    ASSERT(t->in_flight_reply_buf     == NULL);
    ASSERT_EQ_U(t->in_flight_reply_buf_len, 0);
    ASSERT_EQ_U(t->in_flight_reply_rc,      0);

    nx_task_destroy(t);
}
