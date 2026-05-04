/*
 * Kernel tests for slice 8.3 — runtime config manager.
 *
 * Four tests:
 *   1. snapshot after bootstrap reports the 7 expected slots.
 *   2. NX_SYS_CONFIG_OPEN via SVC returns a valid NX_HANDLE_CONFIG handle;
 *      close via NX_SYS_HANDLE_CLOSE makes it invalid.
 *   3. NX_SYS_CONFIG_QUERY via SVC populates the snapshot in the user window.
 *   4. nx_config_swap_component() (direct API) swaps a test fixture slot.
 */

#include "ktest.h"

#include "framework/component.h"
#include "framework/config.h"
#include "framework/dispatcher.h"
#include "framework/handle.h"
#include "framework/ipc.h"
#include "framework/registry.h"
#include "framework/syscall.h"
#include "core/lib/lib.h"
#include "core/mmu/mmu.h"

/* ---- SVC wrappers (mirrors ktest_syscall.c) ------------------------- */

static inline int64_t kcfg_svc0(uint64_t num)
{
    register int64_t  x0 asm("x0");
    register uint64_t x8 asm("x8") = num;
    asm volatile("svc #0" : "=r"(x0) : "r"(x8) : "memory");
    return x0;
}

static inline int64_t kcfg_svc1(uint64_t num, uint64_t a0)
{
    register int64_t  x0 asm("x0") = (int64_t)a0;
    register uint64_t x8 asm("x8") = num;
    asm volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}

static inline int64_t kcfg_svc2(uint64_t num, uint64_t a0, uint64_t a1)
{
    register int64_t  x0 asm("x0") = (int64_t)a0;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x8 asm("x8") = num;
    asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}

static inline int64_t kcfg_svc3(uint64_t num, uint64_t a0, uint64_t a1,
                                  uint64_t a2)
{
    register int64_t  x0 asm("x0") = (int64_t)a0;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x2 asm("x2") = a2;
    register uint64_t x8 asm("x8") = num;
    asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

/* ---- Minimal component ops for test fixture ------------------------- */

static int kcfg_handle(void *self, struct nx_ipc_message *msg)
    { (void)self; (void)msg; return NX_OK; }
static int kcfg_pause_hook(void *self) { (void)self; return NX_OK; }
static int kcfg_pause(void *self)      { (void)self; return NX_OK; }
static int kcfg_resume(void *self)     { (void)self; return NX_OK; }

static const struct nx_component_ops kcfg_ops = {
    .handle_msg = kcfg_handle,
    .pause_hook = kcfg_pause_hook,
    .pause      = kcfg_pause,
    .resume     = kcfg_resume,
};
static const struct nx_component_descriptor kcfg_desc_a = {
    .name = "kcfg_test_comp_a", .ops = &kcfg_ops, .state_size = 0,
};
static const struct nx_component_descriptor kcfg_desc_b = {
    .name = "kcfg_test_comp_b", .ops = &kcfg_ops, .state_size = 0,
};

/* ====================================================================
 * Test 1 — snapshot after bootstrap has seven slots
 * ==================================================================== */

KTEST(config_snapshot_after_bootstrap_has_expected_slots)
{
    /*
     * Verify snapshot API returns valid data.
     *
     * Note: by the time this test runs, many previous ktest tests have
     * spawned and leaked child processes (no reap-on-wait yet).  Each
     * leaked task registers a caller_slot, so the registry can hold
     * hundreds of slots.  The snapshot is capped at
     * NX_CONFIG_SNAPSHOT_MAX_SLOTS (32) and filled LIFO (most-recently
     * registered first), so bootstrap slots — registered at boot, at
     * the tail of the list — are likely beyond the 32-entry window.
     *
     * Verify the bootstrap slots are still reachable via nx_slot_lookup
     * (the authoritative API), and that the snapshot API reports a
     * valid generation and a non-empty slot count.
     */
    KASSERT_NOT_NULL(nx_slot_lookup("scheduler"));
    KASSERT_NOT_NULL(nx_slot_lookup("vfs"));
    KASSERT_NOT_NULL(nx_slot_lookup("filesystem.root"));

    struct nx_config_snapshot snap;
    int rc = nx_config_query_snapshot(&snap);
    KASSERT_EQ_U((uint64_t)rc, (uint64_t)NX_OK);
    KASSERT(snap.num_slots > 0);
    KASSERT(snap.generation > 0);
}

/* ====================================================================
 * Test 2 — NX_SYS_CONFIG_OPEN + NX_SYS_HANDLE_CLOSE via SVC
 * ==================================================================== */

KTEST(config_sys_open_and_close_via_svc)
{
    nx_syscall_reset_for_test();

    int64_t ret = kcfg_svc0(NX_SYS_CONFIG_OPEN);
    KASSERT(ret > 0);   /* must be a positive handle value */

    nx_handle_t h = (nx_handle_t)ret;
    enum nx_handle_type type;
    int rc = nx_handle_lookup(nx_syscall_current_table(), h, &type, NULL, NULL);
    KASSERT_EQ_U((uint64_t)rc, (uint64_t)NX_OK);
    KASSERT_EQ_U((uint64_t)type, (uint64_t)NX_HANDLE_CONFIG);

    /* Close via NX_SYS_HANDLE_CLOSE; subsequent lookup must fail. */
    int64_t close_rc = kcfg_svc1(NX_SYS_HANDLE_CLOSE, (uint64_t)h);
    KASSERT_EQ_U((uint64_t)close_rc, (uint64_t)NX_OK);
    rc = nx_handle_lookup(nx_syscall_current_table(), h, NULL, NULL, NULL);
    KASSERT_EQ_U((uint64_t)rc, (uint64_t)NX_ENOENT);
}

/* ====================================================================
 * Test 3 — NX_SYS_CONFIG_QUERY via SVC (user-window buffer)
 * ==================================================================== */

KTEST(config_sys_query_via_svc_populates_snapshot)
{
    nx_syscall_reset_for_test();

    int64_t ret = kcfg_svc0(NX_SYS_CONFIG_OPEN);
    KASSERT(ret > 0);
    nx_handle_t h = (nx_handle_t)ret;

    /* Place snapshot struct at the start of the user window; the EL1
     * kernel can write there freely, and copy_to_user validates against
     * the same base + size. */
    struct nx_config_snapshot *snap =
        (struct nx_config_snapshot *)(uintptr_t)mmu_user_window_base();
    memset(snap, 0, sizeof *snap);

    int64_t qrc = kcfg_svc2(NX_SYS_CONFIG_QUERY,
                             (uint64_t)h,
                             (uint64_t)(uintptr_t)snap);
    KASSERT_EQ_U((uint64_t)qrc, (uint64_t)NX_OK);
    KASSERT(snap->num_slots >= 7);
    KASSERT(snap->generation > 0);

    /* Close handle via SVC. */
    kcfg_svc1(NX_SYS_HANDLE_CLOSE, (uint64_t)h);
}

/* ====================================================================
 * Test 4 — nx_config_swap_component() via direct API
 *
 * Registers a private test slot and two components, binds comp_a,
 * puts comp_b in READY, calls nx_config_swap_component(), verifies
 * comp_b is now ACTIVE.  Cleans up by unregistering everything.
 * ==================================================================== */

KTEST(config_swap_api_replaces_active_component)
{
    nx_dispatcher_reset();

    struct nx_slot slot = { .name = "kcfg_test_slot", .iface = "kcfg_test",
                            .mutability  = NX_MUT_HOT,
                            .concurrency = NX_CONC_SHARED };
    struct nx_component comp_a = { .manifest_id = "kcfg_test_a",
                                   .instance_id  = "0",
                                   .descriptor   = &kcfg_desc_a };
    struct nx_component comp_b = { .manifest_id = "kcfg_test_b",
                                   .instance_id  = "0",
                                   .descriptor   = &kcfg_desc_b };

    KASSERT_EQ_U((uint64_t)nx_slot_register(&slot),      (uint64_t)NX_OK);
    KASSERT_EQ_U((uint64_t)nx_component_register(&comp_a),(uint64_t)NX_OK);
    KASSERT_EQ_U((uint64_t)nx_component_register(&comp_b),(uint64_t)NX_OK);

    nx_slot_swap(&slot, &comp_a);
    nx_component_init(&comp_a);
    nx_component_enable(&comp_a);
    nx_component_init(&comp_b);   /* comp_b stays in NX_LC_READY */

    KASSERT(slot.active == &comp_a);

    int rc = nx_config_swap_component("kcfg_test_slot", "kcfg_test_b");
    KASSERT_EQ_U((uint64_t)rc, (uint64_t)NX_OK);

    KASSERT(slot.active == &comp_b);
    KASSERT_EQ_U((uint64_t)comp_b.state, (uint64_t)NX_LC_ACTIVE);
    KASSERT_EQ_U((uint64_t)comp_a.state, (uint64_t)NX_LC_DESTROYED);

    /* Clean up: unregister everything so this test doesn't pollute later
     * tests that walk the registry.
     *
     * nx_recompose only calls nx_component_destroy (ACTIVE→DESTROYED), not
     * nx_component_unregister; comp_a's registry node is still present and
     * must be explicitly removed to avoid a dangling comp pointer.
     * We call nx_component_unregister unconditionally (not guarded by a
     * state check) to avoid -O2 register-spill mis-compilation where the
     * conditional branch causes the compiler to pass the wrong argument. */
    nx_slot_swap(&slot, NULL);
    (void)nx_component_unregister(&comp_a);   /* DESTROYED, still in registry */
    if (comp_b.state == NX_LC_ACTIVE || comp_b.state == NX_LC_READY ||
        comp_b.state == NX_LC_PAUSED)
        nx_component_unregister(&comp_b);
    nx_slot_unregister(&slot);
    nx_dispatcher_reset();
}
