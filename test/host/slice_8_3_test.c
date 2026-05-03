/*
 * Host-side tests for slice 8.3 — runtime config manager.
 *
 * Ten tests:
 *   1.  nx_config_open allocates a NX_HANDLE_CONFIG handle.
 *   2.  Closing the config handle invalidates it.
 *   3.  nx_config_query_snapshot on empty graph returns 0 slots.
 *   4.  nx_config_query_snapshot reports registered slots correctly.
 *   5.  nx_config_swap_component returns NX_ENOENT for unknown slot.
 *   6.  nx_config_swap_component returns NX_ENOENT when no READY component.
 *   7.  nx_config_swap_component performs an actual swap.
 *   8.  NX_SYS_CONFIG_OPEN syscall returns a positive handle value.
 *   9.  NX_SYS_CONFIG_QUERY syscall populates the snapshot.
 *  10.  NX_SYS_CONFIG_SWAP syscall fires recompose.
 */

#include "test_runner.h"

#include "framework/component.h"
#include "framework/config.h"
#include "framework/handle.h"
#include "framework/hook.h"
#include "framework/ipc.h"
#include "framework/registry.h"
#include "framework/recompose.h"
#include "framework/syscall.h"

#include <string.h>

/* ---- Trap frame shim (mirrors syscall_test.c) ---------------------- */

struct trap_frame_83 {
    uint64_t x[31];
    uint64_t sp_el0;
    uint64_t pc;
    uint64_t pstate;
};

#define CALL_DISPATCH_83(tf) \
    nx_syscall_dispatch((struct trap_frame *)(tf))

static void reset_frame_83(struct trap_frame_83 *tf)
{
    memset(tf, 0, sizeof *tf);
}

/* ---- Minimal component ops for fixture ----------------------------- */

static int cfg_handle(void *self, struct nx_ipc_message *msg)
    { (void)self; (void)msg; return NX_OK; }
static int cfg_pause_hook(void *self) { (void)self; return NX_OK; }
static int cfg_pause(void *self)      { (void)self; return NX_OK; }
static int cfg_resume(void *self)     { (void)self; return NX_OK; }

static const struct nx_component_ops cfg_comp_ops = {
    .handle_msg = cfg_handle,
    .pause_hook = cfg_pause_hook,
    .pause      = cfg_pause,
    .resume     = cfg_resume,
};
static const struct nx_component_descriptor cfg_comp_desc_a = {
    .name = "cfg_test_comp_a",
    .ops  = &cfg_comp_ops,
};
static const struct nx_component_descriptor cfg_comp_desc_b = {
    .name = "cfg_test_comp_b",
    .ops  = &cfg_comp_ops,
};

static void suite_reset(void)
{
    nx_graph_reset();
    nx_ipc_reset();
    nx_hook_reset();
    nx_syscall_reset_for_test();
}

/* ====================================================================
 * Test 1 — open allocates NX_HANDLE_CONFIG
 * ==================================================================== */

TEST(config_open_allocates_config_handle)
{
    suite_reset();

    struct nx_handle_table t;
    nx_handle_table_init(&t);

    nx_handle_t h = NX_HANDLE_INVALID;
    int rc = nx_config_open(&t, &h);
    ASSERT_EQ_U((uint64_t)rc, (uint64_t)NX_OK);
    ASSERT(h != NX_HANDLE_INVALID);

    enum nx_handle_type type;
    rc = nx_handle_lookup(&t, h, &type, NULL, NULL);
    ASSERT_EQ_U((uint64_t)rc, (uint64_t)NX_OK);
    ASSERT_EQ_U((uint64_t)type, (uint64_t)NX_HANDLE_CONFIG);
}

/* ====================================================================
 * Test 2 — closing the handle invalidates it
 * ==================================================================== */

TEST(config_close_invalidates_handle)
{
    suite_reset();

    struct nx_handle_table t;
    nx_handle_table_init(&t);

    nx_handle_t h = NX_HANDLE_INVALID;
    ASSERT_EQ_U((uint64_t)nx_config_open(&t, &h), (uint64_t)NX_OK);
    ASSERT_EQ_U((uint64_t)nx_handle_close(&t, h), (uint64_t)NX_OK);

    int rc = nx_handle_lookup(&t, h, NULL, NULL, NULL);
    ASSERT_EQ_U((uint64_t)rc, (uint64_t)NX_ENOENT);
}

/* ====================================================================
 * Test 3 — snapshot on empty graph
 * ==================================================================== */

TEST(config_snapshot_empty_graph_zero_slots)
{
    suite_reset();

    struct nx_config_snapshot snap;
    int rc = nx_config_query_snapshot(&snap);
    ASSERT_EQ_U((uint64_t)rc, (uint64_t)NX_OK);
    ASSERT_EQ_U((uint64_t)snap.num_slots, 0);
}

/* ====================================================================
 * Test 4 — snapshot reports registered slots
 * ==================================================================== */

TEST(config_snapshot_reports_registered_slots)
{
    suite_reset();

    struct nx_slot slot_a = { .name = "cfg_slot_a", .iface = "test",
                              .mutability = NX_MUT_HOT,
                              .concurrency = NX_CONC_SHARED };
    struct nx_slot slot_b = { .name = "cfg_slot_b", .iface = "test",
                              .mutability = NX_MUT_HOT,
                              .concurrency = NX_CONC_SHARED };
    struct nx_component comp_a = { .manifest_id = "cfg_impl_a",
                                   .instance_id = "0",
                                   .descriptor  = &cfg_comp_desc_a };

    nx_slot_register(&slot_a);
    nx_slot_register(&slot_b);
    nx_component_register(&comp_a);
    nx_slot_swap(&slot_a, &comp_a);
    nx_component_init(&comp_a);
    nx_component_enable(&comp_a);

    struct nx_config_snapshot snap;
    ASSERT_EQ_U((uint64_t)nx_config_query_snapshot(&snap), (uint64_t)NX_OK);
    ASSERT_EQ_U((uint64_t)snap.num_slots, 2);

    /* generation must be > 0 after several mutations */
    ASSERT(snap.generation != 0);

    /* find slot_a in the snapshot and check impl name */
    int found = 0;
    for (uint32_t i = 0; i < snap.num_slots; i++) {
        if (strcmp(snap.slots[i].slot_name, "cfg_slot_a") == 0) {
            ASSERT(strcmp(snap.slots[i].impl_name, "cfg_impl_a") == 0);
            found = 1;
        }
    }
    ASSERT_EQ_U((uint64_t)found, 1);

    /* slot_b is unbound — impl_name should be empty */
    for (uint32_t i = 0; i < snap.num_slots; i++) {
        if (strcmp(snap.slots[i].slot_name, "cfg_slot_b") == 0)
            ASSERT_EQ_U((uint64_t)snap.slots[i].impl_name[0], 0);
    }
}

/* ====================================================================
 * Test 5 — swap returns NX_ENOENT for unknown slot
 * ==================================================================== */

TEST(config_swap_unknown_slot_returns_enoent)
{
    suite_reset();
    int rc = nx_config_swap_component("no_such_slot", "no_such_impl");
    ASSERT_EQ_U((uint64_t)rc, (uint64_t)NX_ENOENT);
}

/* ====================================================================
 * Test 6 — swap returns NX_ENOENT when no READY component
 * ==================================================================== */

TEST(config_swap_no_ready_comp_returns_enoent)
{
    suite_reset();

    struct nx_slot slot = { .name = "cfg_swap_slot", .iface = "test",
                            .mutability = NX_MUT_HOT,
                            .concurrency = NX_CONC_SHARED };
    nx_slot_register(&slot);

    /* no component registered at all */
    int rc = nx_config_swap_component("cfg_swap_slot", "cfg_impl_b");
    ASSERT_EQ_U((uint64_t)rc, (uint64_t)NX_ENOENT);
}

/* ====================================================================
 * Test 7 — swap performs actual recompose
 * ==================================================================== */

TEST(config_swap_component_replaces_active)
{
    suite_reset();

    struct nx_slot slot = { .name = "cfg_live_slot", .iface = "test",
                            .mutability  = NX_MUT_HOT,
                            .concurrency = NX_CONC_SHARED };
    struct nx_component comp_a = { .manifest_id = "cfg_live_a",
                                   .instance_id = "0",
                                   .descriptor  = &cfg_comp_desc_a };
    struct nx_component comp_b = { .manifest_id = "cfg_live_b",
                                   .instance_id = "0",
                                   .descriptor  = &cfg_comp_desc_b };

    nx_slot_register(&slot);
    nx_component_register(&comp_a);
    nx_component_register(&comp_b);

    /* bind comp_a as the active impl */
    nx_slot_swap(&slot, &comp_a);
    nx_component_init(&comp_a);
    nx_component_enable(&comp_a);

    /* put comp_b in READY so the config manager can find it */
    nx_component_init(&comp_b);

    ASSERT_EQ_PTR(slot.active, &comp_a);

    int rc = nx_config_swap_component("cfg_live_slot", "cfg_live_b");
    ASSERT_EQ_U((uint64_t)rc, (uint64_t)NX_OK);

    /* comp_b should now be ACTIVE behind the slot */
    ASSERT_EQ_PTR(slot.active, &comp_b);
    ASSERT_EQ_U((uint64_t)comp_b.state, (uint64_t)NX_LC_ACTIVE);
}

/* ====================================================================
 * Test 8 — NX_SYS_CONFIG_OPEN syscall
 * ==================================================================== */

TEST(config_sys_open_returns_valid_handle)
{
    suite_reset();

    struct trap_frame_83 tf;
    reset_frame_83(&tf);
    tf.x[8] = NX_SYS_CONFIG_OPEN;
    CALL_DISPATCH_83(&tf);

    /* Return value in x0 is the handle — must be positive */
    int64_t ret = (int64_t)tf.x[0];
    ASSERT(ret > 0);

    nx_handle_t h = (nx_handle_t)ret;
    enum nx_handle_type type;
    int rc = nx_handle_lookup(nx_syscall_current_table(), h, &type, NULL, NULL);
    ASSERT_EQ_U((uint64_t)rc, (uint64_t)NX_OK);
    ASSERT_EQ_U((uint64_t)type, (uint64_t)NX_HANDLE_CONFIG);
}

/* ====================================================================
 * Test 9 — NX_SYS_CONFIG_QUERY syscall populates snapshot
 * ==================================================================== */

TEST(config_sys_query_populates_snapshot)
{
    suite_reset();

    struct nx_slot slot = { .name = "cfg_sys_slot", .iface = "test",
                            .mutability = NX_MUT_HOT,
                            .concurrency = NX_CONC_SHARED };
    struct nx_component comp = { .manifest_id = "cfg_sys_impl",
                                 .instance_id = "0",
                                 .descriptor  = &cfg_comp_desc_a };
    nx_slot_register(&slot);
    nx_component_register(&comp);
    nx_slot_swap(&slot, &comp);
    nx_component_init(&comp);
    nx_component_enable(&comp);

    /* open a config handle via syscall */
    struct trap_frame_83 tf;
    reset_frame_83(&tf);
    tf.x[8] = NX_SYS_CONFIG_OPEN;
    CALL_DISPATCH_83(&tf);
    nx_handle_t h = (nx_handle_t)(int64_t)tf.x[0];
    ASSERT((int64_t)h > 0);

    /* query via syscall — snapshot lives on the stack */
    struct nx_config_snapshot snap;
    memset(&snap, 0, sizeof snap);

    reset_frame_83(&tf);
    tf.x[8] = NX_SYS_CONFIG_QUERY;
    tf.x[0] = (uint64_t)h;
    tf.x[1] = (uint64_t)(uintptr_t)&snap;
    CALL_DISPATCH_83(&tf);
    ASSERT_EQ_U(tf.x[0], (uint64_t)(int64_t)NX_OK);

    ASSERT_EQ_U((uint64_t)snap.num_slots, 1);
    ASSERT(strcmp(snap.slots[0].slot_name, "cfg_sys_slot") == 0);
    ASSERT(strcmp(snap.slots[0].impl_name, "cfg_sys_impl") == 0);
}

/* ====================================================================
 * Test 10 — NX_SYS_CONFIG_SWAP syscall fires recompose
 * ==================================================================== */

TEST(config_sys_swap_replaces_active)
{
    suite_reset();

    struct nx_slot slot = { .name = "cfg_swap_sys", .iface = "test",
                            .mutability  = NX_MUT_HOT,
                            .concurrency = NX_CONC_SHARED };
    struct nx_component comp_a = { .manifest_id = "cfg_swap_sys_a",
                                   .instance_id = "0",
                                   .descriptor  = &cfg_comp_desc_a };
    struct nx_component comp_b = { .manifest_id = "cfg_swap_sys_b",
                                   .instance_id = "0",
                                   .descriptor  = &cfg_comp_desc_b };

    nx_slot_register(&slot);
    nx_component_register(&comp_a);
    nx_component_register(&comp_b);
    nx_slot_swap(&slot, &comp_a);
    nx_component_init(&comp_a);
    nx_component_enable(&comp_a);
    nx_component_init(&comp_b);   /* READY */

    /* open config handle */
    struct trap_frame_83 tf;
    reset_frame_83(&tf);
    tf.x[8] = NX_SYS_CONFIG_OPEN;
    CALL_DISPATCH_83(&tf);
    nx_handle_t h = (nx_handle_t)(int64_t)tf.x[0];
    ASSERT((int64_t)h > 0);

    /* swap via syscall */
    const char *slot_str = "cfg_swap_sys";
    const char *impl_str = "cfg_swap_sys_b";

    reset_frame_83(&tf);
    tf.x[8] = NX_SYS_CONFIG_SWAP;
    tf.x[0] = (uint64_t)h;
    tf.x[1] = (uint64_t)(uintptr_t)slot_str;
    tf.x[2] = (uint64_t)(uintptr_t)impl_str;
    CALL_DISPATCH_83(&tf);
    ASSERT_EQ_U(tf.x[0], (uint64_t)(int64_t)NX_OK);

    ASSERT_EQ_PTR(slot.active, &comp_b);
    ASSERT_EQ_U((uint64_t)comp_b.state, (uint64_t)NX_LC_ACTIVE);
}
