#include "test_runner.h"
#include "framework/component.h"
#include "framework/hook.h"
#include "framework/ipc.h"
#include "framework/registry.h"

/*
 * Host-side tests for framework/hook.c — slice 3.8.
 *
 * The hook framework is self-contained (per-hook-point chains of
 * observers + filters); these tests exercise it in isolation.  The
 * cross-module tests that check hooks firing from real IPC / lifecycle
 * call sites live in component_pause_test.c / component_ipc_test.c.
 */

/* --- Simple counting observer ----------------------------------------- */

struct counter { int calls; enum nx_hook_point last_point; };

static enum nx_hook_action count_cb(struct nx_hook_context *ctx, void *user)
{
    struct counter *c = user;
    c->calls++;
    c->last_point = ctx->point;
    return NX_HOOK_CONTINUE;
}

static enum nx_hook_action abort_cb(struct nx_hook_context *ctx, void *user)
{
    (void)ctx; (void)user;
    return NX_HOOK_ABORT;
}

/* --- Basics ----------------------------------------------------------- */

TEST(hook_register_and_dispatch_invokes_callback)
{
    nx_hook_reset();

    struct counter c = { 0 };
    struct nx_hook h = {
        .point = NX_HOOK_IPC_SEND, .priority = 0,
        .fn = count_cb, .user = &c, .name = "tracer",
    };
    ASSERT_EQ_U(nx_hook_register(&h), NX_OK);
    ASSERT_EQ_U(nx_hook_chain_length(NX_HOOK_IPC_SEND), 1);

    struct nx_hook_context ctx = { .point = NX_HOOK_IPC_SEND };
    ASSERT_EQ_U(nx_hook_dispatch(&ctx), NX_HOOK_CONTINUE);
    ASSERT_EQ_U(c.calls, 1);
    ASSERT_EQ_U(c.last_point, NX_HOOK_IPC_SEND);

    nx_hook_reset();
}

TEST(hook_register_rejects_bad_inputs)
{
    nx_hook_reset();
    ASSERT_EQ_U(nx_hook_register(NULL), NX_EINVAL);

    struct nx_hook no_fn = { .point = NX_HOOK_IPC_SEND };
    ASSERT_EQ_U(nx_hook_register(&no_fn), NX_EINVAL);

    struct nx_hook bad_point = {
        .point = NX_HOOK_POINT_COUNT,   /* sentinel, not a real point */
        .fn = count_cb,
    };
    ASSERT_EQ_U(nx_hook_register(&bad_point), NX_EINVAL);

    struct counter c = { 0 };
    struct nx_hook h = {
        .point = NX_HOOK_IPC_SEND, .fn = count_cb, .user = &c,
    };
    ASSERT_EQ_U(nx_hook_register(&h), NX_OK);
    /* Same pointer registered twice → EEXIST. */
    ASSERT_EQ_U(nx_hook_register(&h), NX_EEXIST);
    nx_hook_reset();
}

/* Tag + recording callback for the priority-ordering test. */
struct prio_tag { int id; };
static int g_prio_order[8];
static int g_prio_order_n;

static enum nx_hook_action prio_record_cb(struct nx_hook_context *ctx,
                                          void *user)
{
    (void)ctx;
    struct prio_tag *t = user;
    if (g_prio_order_n < (int)(sizeof g_prio_order / sizeof g_prio_order[0]))
        g_prio_order[g_prio_order_n++] = t->id;
    return NX_HOOK_CONTINUE;
}

TEST(hook_priority_order_is_ascending_then_stable)
{
    nx_hook_reset();
    g_prio_order_n = 0;

    struct prio_tag tags[4] = { {0}, {1}, {2}, {3} };
    struct nx_hook  hs[4];
    /* Register in priorities: 10, 0, 5, 5 — expected order: 1, 2, 3, 0. */
    int prios[4] = { 10, 0, 5, 5 };
    for (int i = 0; i < 4; i++) {
        hs[i] = (struct nx_hook){
            .point    = NX_HOOK_IPC_SEND,
            .priority = prios[i],
            .fn       = prio_record_cb,
            .user     = &tags[i],
        };
        ASSERT_EQ_U(nx_hook_register(&hs[i]), NX_OK);
    }

    struct nx_hook_context ctx = { .point = NX_HOOK_IPC_SEND };
    nx_hook_dispatch(&ctx);

    ASSERT_EQ_U(g_prio_order_n, 4);
    ASSERT_EQ_U(g_prio_order[0], 1);   /* prio 0  */
    ASSERT_EQ_U(g_prio_order[1], 2);   /* prio 5, registered first */
    ASSERT_EQ_U(g_prio_order[2], 3);   /* prio 5, registered second (stable) */
    ASSERT_EQ_U(g_prio_order[3], 0);   /* prio 10 */

    nx_hook_reset();
}

TEST(hook_abort_short_circuits_chain)
{
    nx_hook_reset();
    struct counter c = { 0 };

    struct nx_hook first = {
        .point = NX_HOOK_IPC_SEND, .priority = 0, .fn = abort_cb,
    };
    struct nx_hook second = {
        .point = NX_HOOK_IPC_SEND, .priority = 1, .fn = count_cb, .user = &c,
    };
    nx_hook_register(&first);
    nx_hook_register(&second);

    struct nx_hook_context ctx = { .point = NX_HOOK_IPC_SEND };
    ASSERT_EQ_U(nx_hook_dispatch(&ctx), NX_HOOK_ABORT);
    ASSERT_EQ_U(c.calls, 0);       /* never reached the second hook */

    nx_hook_reset();
}

TEST(hook_unregister_removes_from_chain)
{
    nx_hook_reset();
    struct counter c = { 0 };
    struct nx_hook h = {
        .point = NX_HOOK_IPC_SEND, .fn = count_cb, .user = &c,
    };
    nx_hook_register(&h);
    ASSERT_EQ_U(nx_hook_chain_length(NX_HOOK_IPC_SEND), 1);

    nx_hook_unregister(&h);
    ASSERT_EQ_U(nx_hook_chain_length(NX_HOOK_IPC_SEND), 0);

    /* Unregister twice = no-op. */
    nx_hook_unregister(&h);
    ASSERT_EQ_U(nx_hook_chain_length(NX_HOOK_IPC_SEND), 0);

    nx_hook_reset();
}

/* --- Unregister-during-dispatch: mark-then-sweep ---------------------- */

struct self_unreg {
    struct nx_hook *self;
    int             calls;
};

static enum nx_hook_action self_unreg_cb(struct nx_hook_context *ctx,
                                         void *user)
{
    (void)ctx;
    struct self_unreg *su = user;
    su->calls++;
    nx_hook_unregister(su->self);
    return NX_HOOK_CONTINUE;
}

TEST(hook_self_unregister_during_dispatch_is_safe)
{
    nx_hook_reset();

    struct self_unreg su = { 0 };
    struct nx_hook h = {
        .point = NX_HOOK_IPC_SEND, .fn = self_unreg_cb, .user = &su,
    };
    su.self = &h;
    nx_hook_register(&h);

    /* Also register a second hook so the chain iteration has somewhere
     * to go after `h` marks itself dead. */
    struct counter after = { 0 };
    struct nx_hook trailer = {
        .point = NX_HOOK_IPC_SEND, .priority = 10,
        .fn = count_cb, .user = &after,
    };
    nx_hook_register(&trailer);

    struct nx_hook_context ctx = { .point = NX_HOOK_IPC_SEND };
    nx_hook_dispatch(&ctx);
    ASSERT_EQ_U(su.calls, 1);
    ASSERT_EQ_U(after.calls, 1);

    /* Sweep happened at end-of-dispatch — chain length is now 1. */
    ASSERT_EQ_U(nx_hook_chain_length(NX_HOOK_IPC_SEND), 1);

    /* Second dispatch reaches only `trailer`. */
    nx_hook_dispatch(&ctx);
    ASSERT_EQ_U(su.calls, 1);
    ASSERT_EQ_U(after.calls, 2);

    nx_hook_reset();
}

struct peer_unreg {
    struct nx_hook *peer;
    int             calls;
};

static enum nx_hook_action peer_unreg_cb(struct nx_hook_context *ctx,
                                         void *user)
{
    (void)ctx;
    struct peer_unreg *pu = user;
    pu->calls++;
    nx_hook_unregister(pu->peer);
    return NX_HOOK_CONTINUE;
}

TEST(hook_unregister_peer_during_dispatch_defers_removal)
{
    nx_hook_reset();

    struct counter peer = { 0 };
    struct nx_hook peer_hook = {
        .point = NX_HOOK_IPC_SEND, .priority = 10,
        .fn = count_cb, .user = &peer,
    };
    struct peer_unreg pu = { .peer = &peer_hook };
    struct nx_hook trigger = {
        .point = NX_HOOK_IPC_SEND, .priority = 0,
        .fn = peer_unreg_cb, .user = &pu,
    };
    nx_hook_register(&trigger);
    nx_hook_register(&peer_hook);

    struct nx_hook_context ctx = { .point = NX_HOOK_IPC_SEND };
    nx_hook_dispatch(&ctx);

    /* `peer_hook` was marked dead before we got to it in the same
     * dispatch — it should NOT have run.  After return, sweep removes
     * it from the chain. */
    ASSERT_EQ_U(pu.calls, 1);
    ASSERT_EQ_U(peer.calls, 0);
    ASSERT_EQ_U(nx_hook_chain_length(NX_HOOK_IPC_SEND), 1);

    nx_hook_reset();
}

/* --- Dispatch for empty / invalid contexts ---------------------------- */

TEST(hook_dispatch_on_empty_chain_is_continue)
{
    nx_hook_reset();
    struct nx_hook_context ctx = { .point = NX_HOOK_IPC_RECV };
    ASSERT_EQ_U(nx_hook_dispatch(&ctx), NX_HOOK_CONTINUE);
}

TEST(hook_dispatch_ignores_null_and_out_of_range_points)
{
    nx_hook_reset();
    ASSERT_EQ_U(nx_hook_dispatch(NULL), NX_HOOK_CONTINUE);

    struct nx_hook_context bad = { .point = NX_HOOK_POINT_COUNT };
    ASSERT_EQ_U(nx_hook_dispatch(&bad), NX_HOOK_CONTINUE);
}

/* --- Reset clears every chain ---------------------------------------- */

TEST(hook_reset_clears_every_chain)
{
    nx_hook_reset();
    struct counter a = { 0 }, b = { 0 };
    struct nx_hook ha = {
        .point = NX_HOOK_IPC_SEND, .fn = count_cb, .user = &a,
    };
    struct nx_hook hb = {
        .point = NX_HOOK_COMPONENT_PAUSE, .fn = count_cb, .user = &b,
    };
    nx_hook_register(&ha);
    nx_hook_register(&hb);
    ASSERT_EQ_U(nx_hook_chain_length(NX_HOOK_IPC_SEND), 1);
    ASSERT_EQ_U(nx_hook_chain_length(NX_HOOK_COMPONENT_PAUSE), 1);

    nx_hook_reset();
    ASSERT_EQ_U(nx_hook_chain_length(NX_HOOK_IPC_SEND), 0);
    ASSERT_EQ_U(nx_hook_chain_length(NX_HOOK_COMPONENT_PAUSE), 0);
}
