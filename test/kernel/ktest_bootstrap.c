#include "ktest.h"
#include "core/sched/task.h"
#include "core/sched/sched.h"
#include "framework/bootstrap.h"
#include "framework/component.h"
#include "framework/dispatcher.h"
#include "framework/hook.h"
#include "framework/ipc.h"
#include "framework/registry.h"
#include "framework/slot_call.h"
#include "interfaces/char_device_msg.h"

/*
 * Kernel-side coverage for slice 3.9a.
 *
 * `ktest_main` runs AFTER `boot_main` has called `nx_framework_
 * bootstrap()`, so the composition is already up when these tests
 * execute.  They assert that the expected slots / components /
 * bindings are in place — i.e. the boot walker actually ran and
 * drove every descriptor through init → enable.
 */

/* Tiny state struct defined in components/uart_pl011/uart_pl011.c.
 * Declared here (not in a header — component state is private by
 * design) so the test can sanity-check that init/enable actually
 * fired on the bound instance. */
struct uart_pl011_state {
    unsigned init_called;
    unsigned enable_called;
    unsigned messages_handled;
};

KTEST(bootstrap_registers_expected_slot)
{
    struct nx_slot *s = nx_slot_lookup("char_device.serial");
    KASSERT_NOT_NULL(s);
    KASSERT(strcmp(s->iface, "char_device") == 0);
}

KTEST(bootstrap_binds_uart_to_its_slot)
{
    struct nx_slot *s = nx_slot_lookup("char_device.serial");
    KASSERT_NOT_NULL(s);
    KASSERT_NOT_NULL(s->active);
    KASSERT(strcmp(s->active->manifest_id, "uart_pl011") == 0);
    KASSERT_EQ_U(s->active->state, NX_LC_ACTIVE);
}

KTEST(bootstrap_invoked_component_init_and_enable)
{
    struct nx_slot *s = nx_slot_lookup("char_device.serial");
    KASSERT_NOT_NULL(s);
    KASSERT_NOT_NULL(s->active);
    KASSERT_NOT_NULL(s->active->impl);
    const struct uart_pl011_state *us = s->active->impl;
    KASSERT_EQ_U(us->init_called,   1);
    KASSERT_EQ_U(us->enable_called, 1);
}

KTEST(bootstrap_component_count_matches_descriptor_section)
{
    /* Slice 3.9a wires a single real component (uart_pl011).  As more
     * land this count grows; the assertion stays "at least 1" so new
     * descriptors don't break this test. */
    KASSERT(nx_graph_component_count() >= 1);
}

KTEST(bootstrap_snapshot_json_contains_bound_impl)
{
    /* Bumped 2048 → 4096 in slice 8.0a.4; bumped 4096 → 8192 in slice
     * 8.0a.8.  Each slice 8.0a.6–8.0a.8 ktest spawns a kthread via
     * sched_spawn_kthread; each kthread registers its own caller_slot +
     * 4 cloned edges through wire_caller_slot, growing the graph by ~250
     * bytes of JSON per kthread.  3 un-destroyed kthreads (one per ktest)
     * accumulate before this test runs, pushing JSON past 4 KiB.
     * 8192 provides ~4 KiB of headroom for future kthread growth. */
    static char buf[8192];
    struct nx_graph_snapshot *snap = nx_graph_snapshot_take();
    KASSERT_NOT_NULL(snap);

    int rc = nx_graph_snapshot_to_json(snap, buf, sizeof buf);
    KASSERT(rc > 0);

    /* Buffer must be NUL-terminated and contain the slot name and
     * the bound manifest.  strstr isn't in core/lib, so scan manually. */
    const char *needle_slot = "\"name\":\"char_device.serial\"";
    const char *needle_impl = "\"manifest\":\"uart_pl011\"";
    int found_slot = 0, found_impl = 0;
    for (size_t i = 0; buf[i]; i++) {
        if (!found_slot && strncmp(&buf[i], needle_slot, strlen(needle_slot)) == 0)
            found_slot = 1;
        if (!found_impl && strncmp(&buf[i], needle_impl, strlen(needle_impl)) == 0)
            found_impl = 1;
    }
    KASSERT(found_slot);
    KASSERT(found_impl);

    nx_graph_snapshot_put(snap);
}

/* ---- slice 3.9b.2: NX_HOOK_SLOT_SWAPPED runs end-to-end on kernel --- */

struct swap_capture {
    int                  fires;
    struct nx_slot      *slot;
    struct nx_component *old_impl;
    struct nx_component *new_impl;
};

static enum nx_hook_action capture_swap_hook(struct nx_hook_context *ctx,
                                             void                   *user)
{
    struct swap_capture *cap = user;
    cap->fires++;
    cap->slot     = ctx->u.swap.slot;
    cap->old_impl = ctx->u.swap.old_impl;
    cap->new_impl = ctx->u.swap.new_impl;
    return NX_HOOK_CONTINUE;
}

KTEST(slot_swap_fans_out_to_hook_chain_on_kernel)
{
    /* Register a fresh test-only slot so we don't perturb the live
     * bootstrap composition.  Pick something the dispatcher won't
     * also touch. */
    static struct nx_slot      s = { .name = "ktest.swap_probe", .iface = "x" };
    static struct nx_component a = { .manifest_id = "ktest_swap", .instance_id = "a" };
    static struct nx_component b = { .manifest_id = "ktest_swap", .instance_id = "b" };
    KASSERT_EQ_U(nx_slot_register(&s), NX_OK);
    KASSERT_EQ_U(nx_component_register(&a), NX_OK);
    KASSERT_EQ_U(nx_component_register(&b), NX_OK);

    static struct swap_capture cap;
    cap = (struct swap_capture){ 0 };
    static struct nx_hook h;
    h = (struct nx_hook){
        .point = NX_HOOK_SLOT_SWAPPED,
        .fn    = capture_swap_hook,
        .user  = &cap,
    };
    KASSERT_EQ_U(nx_hook_register(&h), NX_OK);

    KASSERT_EQ_U(nx_slot_swap(&s, &a), NX_OK);
    KASSERT_EQ_U(cap.fires, 1);
    KASSERT(cap.slot == &s);
    KASSERT(cap.old_impl == 0);
    KASSERT(cap.new_impl == &a);

    KASSERT_EQ_U(nx_slot_swap(&s, &b), NX_OK);
    KASSERT_EQ_U(cap.fires, 2);
    KASSERT(cap.old_impl == &a);
    KASSERT(cap.new_impl == &b);

    /* Tear down: hook off, slot back to NULL active, registry lookups
     * for later tests still find `s` but its active is NULL so no
     * dispatch path touches it. */
    nx_hook_unregister(&h);
    (void)nx_slot_swap(&s, 0);
}

/* ---- slice 8.0a.5: per-task caller_slot wiring on the kernel build ---- */

/*
 * In the live kernel composition `posix_shim` is bound, so every task
 * created via `nx_task_create` registers a `caller_slot` and clones
 * posix_shim's 4 outgoing edges.  These tests assert end-to-end on
 * the running kernel — host coverage in `task_test.c` already nails
 * down the create/destroy pairing under a synthesized fixture.
 */

struct task_slot_edge_count {
    struct nx_task *task;
    int             count;
};

static void count_outgoing_cb(struct nx_connection *c, void *ctx)
{
    struct task_slot_edge_count *acc = ctx;
    if (c && c->from_slot == &acc->task->caller_slot)
        acc->count++;
}

static void ktest_dummy_entry(void *arg) { (void)arg; }

KTEST(bootstrap_caller_slot_create_destroy_round_trip_in_kernel)
{
    /* End-to-end on the live kernel composition: posix_shim is bound,
     * so a fresh `nx_task_create` registers the caller_slot, clones
     * posix_shim's 4 outgoing edges, and `nx_task_destroy` unwinds
     * cleanly.  Mirrors task_test.c's host coverage for the path that
     * matters most — `wire_caller_slot` must work in the real kernel
     * registry, not just under a synthesized fixture. */
    size_t slots_before = nx_graph_slot_count();
    size_t conns_before = nx_graph_connection_count();

    struct nx_task *t = nx_task_create("ktest_pcs", ktest_dummy_entry,
                                       NULL, 1);
    KASSERT_NOT_NULL(t);
    KASSERT(t->caller_slot_active);
    KASSERT_NOT_NULL(t->caller_slot.active);
    KASSERT(strcmp(t->caller_slot.active->manifest_id, "posix_shim") == 0);
    KASSERT_EQ_U(nx_graph_slot_count(),       slots_before + 1);
    KASSERT_EQ_U(nx_graph_connection_count(), conns_before + 4);

    /* Slot looks up by its synthesized name. */
    struct nx_slot *looked_up = nx_slot_lookup(t->caller_slot_name);
    KASSERT(looked_up == &t->caller_slot);

    /* Edges all originate from this caller_slot. */
    struct task_slot_edge_count acc = { .task = t, .count = 0 };
    nx_slot_foreach_dependency(&t->caller_slot, count_outgoing_cb, &acc);
    KASSERT_EQ_U(acc.count, 4);

    nx_task_destroy(t);
    KASSERT_EQ_U(nx_graph_slot_count(),       slots_before);
    KASSERT_EQ_U(nx_graph_connection_count(), conns_before);
}

/* ---- slice 8.0a.6: blocking-call round-trip on the live composition ---- *
 *
 * The first end-to-end exercise of `nx_slot_call_blocking`.  A fresh
 * kthread (real `nx_task_current()`, `caller_slot_active == true`)
 * issues a blocking call to the `char_device.serial` slot — bound to
 * `uart_pl011`, which has a real `handle_msg` returning 0.  The
 * dispatcher kthread runs the request, posts the reply, runs
 * `posix_shim_handle_msg` to copy the reply payload into the caller's
 * `in_flight_reply_buf` and wake the caller's `reply_waitq`; the
 * caller resumes and `nx_slot_call_blocking` returns rc.
 *
 * The ktest runs in idle-task context and yields the CPU until the
 * caller kthread sets `g_blocking_call_finished`.  Bounded yield budget
 * matches the existing argv_push / fork ktest convention.
 */

static struct nx_task *g_blocking_call_task;
static volatile int    g_blocking_call_finished;
static volatile int    g_blocking_call_rc;
static volatile size_t g_blocking_call_pool_at_start;
static volatile size_t g_blocking_call_pool_after_call;

static void blocking_call_kthread(void *arg)
{
    (void)arg;
    struct nx_task *me = nx_task_current();
    struct nx_slot *uart = nx_slot_lookup("char_device.serial");
    if (!me || !uart || !me->caller_slot_active) {
        g_blocking_call_rc       = -999;
        g_blocking_call_finished = 1;
        for (;;) nx_task_yield();
    }

    g_blocking_call_pool_at_start =
        nx_dispatcher_reply_pool_in_use_for_test();

    static char    reply_buf[NX_REPLY_PAYLOAD_MAX];
    /* Properly-typed request: NX_CHAR_DEVICE_OP_WRITE expects a
     * nx_char_device_msg_write (buf + len).  The dispatch writes the
     * reply struct in-place at msg->payload, so the buffer must be
     * writable and large enough for nx_char_device_reply_write (16 B).
     * Sending 0 bytes to uart avoids any actual UART I/O. */
    static const uint8_t uart_nul_byte = 0;
    static struct nx_char_device_msg_write payload_write = {
        .buf = 0,  /* filled at runtime below — not a compile-time constant */
        .len = 0,
    };
    payload_write.buf = (uint64_t)(uintptr_t)&uart_nul_byte;

    struct nx_ipc_message msg = {
        .src_slot    = &me->caller_slot,
        .dst_slot    = uart,
        .msg_type    = NX_CHAR_DEVICE_OP_WRITE,
        .flags       = 0,
        .payload     = &payload_write,
        .payload_len = (uint32_t)sizeof(payload_write),
        .n_caps      = 0,
        .caps        = NULL,
    };

    int rc = nx_slot_call_blocking(uart, &msg, reply_buf, sizeof reply_buf);

    g_blocking_call_pool_after_call =
        nx_dispatcher_reply_pool_in_use_for_test();
    g_blocking_call_rc       = rc;
    g_blocking_call_finished = 1;

    for (;;) nx_task_yield();
}

KTEST(slot_call_blocking_round_trip_via_uart_returns_handler_rc)
{
    g_blocking_call_finished       = 0;
    g_blocking_call_rc             = 0xdead;
    g_blocking_call_pool_at_start  = 0;
    g_blocking_call_pool_after_call = 0;

    g_blocking_call_task = sched_spawn_kthread("ktest_blocking",
                                               blocking_call_kthread,
                                               NULL, NULL);
    KASSERT_NOT_NULL(g_blocking_call_task);

    /* Yield until the call returns.  Generous bound — the round trip
     * is two voluntary task switches plus a dispatcher iteration; we
     * just need to yield often enough to let the dispatcher run. */
    const int max_yields = 4096;
    int reached = 0;
    for (int i = 0; i < max_yields; i++) {
        if (g_blocking_call_finished) { reached = 1; break; }
        nx_task_yield();
    }
    KASSERT(reached);

    /* uart_pl011_handle_msg returns 0 on success — that's the rc the
     * caller observes after the round-trip. */
    KASSERT_EQ_U(g_blocking_call_rc, 0);

    /* Pool entry was returned after delivery.  Other tests in the
     * suite may have left entries in flight (e.g. earlier kthreads
     * that issued blocking calls during this same test pass), so
     * assert "did not grow" rather than "exact zero". */
    KASSERT_EQ_U(g_blocking_call_pool_after_call,
                 g_blocking_call_pool_at_start);

    /* Quiesce the kthread by removing it from the runqueue (still
     * yielding in its tail loop is fine — the scheduler skips
     * dequeued tasks). */
    const struct nx_scheduler_ops *ops = sched_ops_for_test();
    void *self = sched_self_for_test();
    if (ops && self) ops->dequeue(self, g_blocking_call_task);
}

/* ---- slice 8.0a.8: hook inspector fires during blocking call ----------- *
 *
 * Register an observe-only hook on NX_HOOK_IPC_SEND before the blocking
 * call.  The hook increments a counter; the call result must be identical
 * to the baseline test (rc == 0 from uart_pl011).  Verifies that the
 * inspector infrastructure described in hook_inspector.h works in the
 * live kernel composition and that observe-only hooks don't alter results.
 */

static volatile int g_hook_inspector_fire_count;

static enum nx_hook_action ktest_observe_hook_fn(struct nx_hook_context *ctx,
                                                  void *user)
{
    (void)ctx; (void)user;
    __atomic_fetch_add(&g_hook_inspector_fire_count, 1, __ATOMIC_RELAXED);
    return NX_HOOK_CONTINUE;
}

static struct nx_task   *g_hi_kthread;
static volatile int      g_hi_finished;
static volatile int      g_hi_rc;

static void hook_inspector_kthread(void *arg)
{
    (void)arg;
    struct nx_task *me = nx_task_current();
    struct nx_slot *uart = nx_slot_lookup("char_device.serial");
    if (!me || !uart || !me->caller_slot_active) {
        g_hi_rc       = -999;
        g_hi_finished = 1;
        for (;;) nx_task_yield();
    }

    static char reply_buf_hi[NX_REPLY_PAYLOAD_MAX];
    /* Same fix as blocking_call_kthread: send a properly-typed
     * nx_char_device_msg_write so nx_char_device_dispatch reads a
     * valid len field and uart_write does not access garbage memory. */
    static const uint8_t uart_nul_byte_hi = 0;
    static struct nx_char_device_msg_write payload_hi = {
        .buf = 0,  /* filled at runtime below */
        .len = 0,
    };
    payload_hi.buf = (uint64_t)(uintptr_t)&uart_nul_byte_hi;

    struct nx_ipc_message msg = {
        .src_slot    = &me->caller_slot,
        .dst_slot    = uart,
        .msg_type    = NX_CHAR_DEVICE_OP_WRITE,
        .flags       = 0,
        .payload     = &payload_hi,
        .payload_len = (uint32_t)sizeof(payload_hi),
        .n_caps      = 0,
        .caps        = NULL,
    };
    g_hi_rc       = nx_slot_call_blocking(uart, &msg,
                                          reply_buf_hi, sizeof reply_buf_hi);
    g_hi_finished = 1;
    for (;;) nx_task_yield();
}

KTEST(hook_inspector_observe_only_does_not_alter_blocking_call_result)
{
    g_hook_inspector_fire_count = 0;
    g_hi_finished               = 0;
    g_hi_rc                     = 0xdead;

    struct nx_hook obs_hook = {
        .point    = NX_HOOK_IPC_SEND,
        .priority = 50,
        .fn       = ktest_observe_hook_fn,
        .user     = NULL,
        .name     = "ktest_observer",
    };
    KASSERT_EQ_U(nx_hook_register(&obs_hook), NX_OK);

    g_hi_kthread = sched_spawn_kthread("ktest_hi", hook_inspector_kthread,
                                       NULL, NULL);
    KASSERT_NOT_NULL(g_hi_kthread);

    const int max_yields = 4096;
    int reached = 0;
    for (int i = 0; i < max_yields; i++) {
        if (g_hi_finished) { reached = 1; break; }
        nx_task_yield();
    }
    KASSERT(reached);

    /* Observer hook must have fired (IPC_SEND fires inside
     * nx_slot_call_blocking before enqueue). */
    KASSERT(g_hook_inspector_fire_count >= 1);

    /* Result must be identical to the baseline blocking-call test. */
    KASSERT_EQ_U(g_hi_rc, 0);

    nx_hook_unregister(&obs_hook);

    const struct nx_scheduler_ops *ops2 = sched_ops_for_test();
    void *self2 = sched_self_for_test();
    if (ops2 && self2) ops2->dequeue(self2, g_hi_kthread);
}

/* ---- slice 8.0a.8: cap-scan rejects forged cap during blocking call ---- *
 *
 * Build a message whose caps[] contains a slot_ref pointing to
 * filesystem.root — a slot the caller_slot has no outgoing edge to.
 * nx_slot_call_blocking's cap-scan step must reject it with NX_EINVAL
 * before the message is enqueued, and the in_flight_reply_buf must be
 * cleared on the way out.
 */

static struct nx_task   *g_capforge_task;
static volatile int      g_capforge_finished;
static volatile int      g_capforge_rc;
static volatile int      g_capforge_buf_cleared;

static void capforge_kthread(void *arg)
{
    (void)arg;
    struct nx_task *me   = nx_task_current();
    struct nx_slot *uart = nx_slot_lookup("char_device.serial");
    struct nx_slot *fs   = nx_slot_lookup("filesystem.root");

    if (!me || !uart || !fs || !me->caller_slot_active) {
        g_capforge_rc       = -999;
        g_capforge_finished = 1;
        for (;;) nx_task_yield();
    }

    /* filesystem.root is not in caller_slot's outgoing edges — forged. */
    static struct nx_ipc_cap forged_cap = {
        .kind      = NX_CAP_SLOT_REF,
        .ownership = NX_CAP_BORROW,
        .cap_id    = 77,
    };
    forged_cap.u.slot_ref = fs;

    static char reply_buf_cf[NX_REPLY_PAYLOAD_MAX];
    struct nx_ipc_message msg = {
        .src_slot    = &me->caller_slot,
        .dst_slot    = uart,
        .msg_type    = 1,
        .flags       = 0,
        .payload     = NULL,
        .payload_len = 0,
        .n_caps      = 1,
        .caps        = &forged_cap,
    };
    g_capforge_rc          = nx_slot_call_blocking(uart, &msg,
                                                   reply_buf_cf,
                                                   sizeof reply_buf_cf);
    g_capforge_buf_cleared = (me->in_flight_reply_buf == NULL) ? 1 : 0;
    g_capforge_finished    = 1;
    for (;;) nx_task_yield();
}

KTEST(cap_scan_rejects_forged_slot_ref_cap_during_blocking_call)
{
    g_capforge_finished    = 0;
    g_capforge_rc          = 0xdead;
    g_capforge_buf_cleared = 0;

    g_capforge_task = sched_spawn_kthread("ktest_capforge", capforge_kthread,
                                          NULL, NULL);
    KASSERT_NOT_NULL(g_capforge_task);

    const int max_yields = 4096;
    int reached = 0;
    for (int i = 0; i < max_yields; i++) {
        if (g_capforge_finished) { reached = 1; break; }
        nx_task_yield();
    }
    KASSERT(reached);

    /* Cap-scan step must return NX_EINVAL. */
    KASSERT_EQ_U((unsigned)g_capforge_rc, (unsigned)NX_EINVAL);

    /* in_flight_reply_buf cleared on the error path. */
    KASSERT_EQ_U(g_capforge_buf_cleared, 1);

    const struct nx_scheduler_ops *ops3 = sched_ops_for_test();
    void *self3 = sched_self_for_test();
    if (ops3 && self3) ops3->dequeue(self3, g_capforge_task);
}
