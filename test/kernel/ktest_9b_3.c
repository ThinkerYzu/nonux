/*
 * Slice 9b.3 — Route read/write/seek through slot_call_blocking.
 *
 * Tests for nx_char_device_write / nx_char_device_read (the new
 * char_device_call.c wrappers that use nx_slot_call_blocking) and
 * for nx_vfs_write / nx_vfs_read / nx_vfs_seek (confirming the VFS
 * path still works end-to-end via the RESOURCE-handle routing).
 *
 * Because nx_slot_call_blocking requires caller_slot_active, tests
 * that exercise these wrappers directly spawn a fresh kthread.
 * Spawned kthreads always get wire_caller_slot (posix_shim is live
 * after bootstrap), so they can issue blocking slot calls.  Each
 * kthread writes its result into a shared g_9b3 struct and sets
 * g_9b3.done; the KTEST runner yields until done is observed.
 *
 * Tests:
 *   1. char_device_slot_is_active — slot registry sanity (no kthread)
 *   2. char_device_write_returns_byte_count
 *   3. char_device_write_zero_len_returns_zero
 *   4. char_device_read_returns_preinjected_bytes
 *   5. char_device_read_eof_returns_zero
 *   6. char_device_read_partial_less_than_cap
 *   7. char_device_write_increments_console_write_calls
 *   8. vfs_write_read_roundtrip_via_slot_call_blocking
 *   9. vfs_seek_repositions_via_slot_call_blocking
 *  10. char_device_read_exact_byte_content_matches
 */

#include "ktest.h"

#include "framework/char_device_call.h"
#include "framework/console.h"
#include "framework/registry.h"
#include "framework/vfs_call.h"
#include "interfaces/vfs.h"
#include "core/sched/sched.h"
#include "core/sched/task.h"

#include <stdatomic.h>

/* ---- Shared kthread result state ----------------------------------- */

static _Atomic int g_9b3_done;
static int64_t     g_9b3_rc;
static char        g_9b3_buf[32];

#define WAIT_DONE(max_yields)                                   \
    do {                                                        \
        int _i;                                                 \
        for (_i = 0; _i < (max_yields); _i++) {                \
            if (atomic_load(&g_9b3_done)) break;                \
            nx_task_yield();                                    \
        }                                                       \
    } while (0)

/* ---- Kthread body types ------------------------------------------- */

struct char_write_arg {
    const char *msg;
    size_t      len;
};

static void char_write_kthread(void *arg)
{
    const struct char_write_arg *a = (const struct char_write_arg *)arg;
    struct nx_slot *cs = nx_slot_lookup("char_device.serial");
    g_9b3_rc = nx_char_device_write(cs, a->msg, a->len);
    atomic_store(&g_9b3_done, 1);
}

static void char_read_kthread(void *arg)
{
    size_t cap = (size_t)(uintptr_t)arg;
    struct nx_slot *cs = nx_slot_lookup("char_device.serial");
    g_9b3_rc = nx_char_device_read(cs, 0, g_9b3_buf, cap);
    atomic_store(&g_9b3_done, 1);
}

/* ---- Test 1: slot registry ---------------------------------------- */

KTEST(char_device_slot_is_active_after_bootstrap)
{
    struct nx_slot *cs = nx_slot_lookup("char_device.serial");
    KASSERT_NOT_NULL(cs);
    KASSERT_NOT_NULL(cs->active);
    KASSERT_EQ_U((uint64_t)cs->active->state, (uint64_t)NX_LC_ACTIVE);
    KASSERT(strcmp(cs->active->manifest_id, "uart_pl011") == 0);
}

/* ---- Test 2: char_device_write returns byte count ----------------- */

KTEST(char_device_write_returns_byte_count)
{
    struct nx_slot *cs = nx_slot_lookup("char_device.serial");
    KASSERT_NOT_NULL(cs);

    static const struct char_write_arg arg2 = { "hello", 5 };
    atomic_store(&g_9b3_done, 0);
    g_9b3_rc = -999;
    struct nx_task *t = sched_spawn_kthread("9b3-w2", char_write_kthread,
                                            (void *)&arg2, NULL);
    KASSERT_NOT_NULL(t);
    WAIT_DONE(1024);
    KASSERT(atomic_load(&g_9b3_done));
    KASSERT_EQ_U((uint64_t)g_9b3_rc, (uint64_t)5);
}

/* ---- Test 3: char_device_write with zero length ------------------- */

KTEST(char_device_write_zero_len_returns_zero)
{
    struct nx_slot *cs = nx_slot_lookup("char_device.serial");
    KASSERT_NOT_NULL(cs);

    static const struct char_write_arg arg3 = { "", 0 };
    atomic_store(&g_9b3_done, 0);
    g_9b3_rc = -999;
    struct nx_task *t = sched_spawn_kthread("9b3-w3", char_write_kthread,
                                            (void *)&arg3, NULL);
    KASSERT_NOT_NULL(t);
    WAIT_DONE(1024);
    KASSERT(atomic_load(&g_9b3_done));
    KASSERT_EQ_U((uint64_t)g_9b3_rc, (uint64_t)0);
}

/* ---- Test 4: char_device_read with pre-injected bytes ------------- */

KTEST(char_device_read_returns_preinjected_bytes)
{
    nx_console_reset_for_test();
    struct nx_slot *cs = nx_slot_lookup("char_device.serial");
    KASSERT_NOT_NULL(cs);

    size_t pushed = nx_console_test_inject_bytes("world", 5);
    KASSERT_EQ_U(pushed, (uint64_t)5);

    atomic_store(&g_9b3_done, 0);
    g_9b3_rc = -999;
    struct nx_task *t = sched_spawn_kthread("9b3-r4", char_read_kthread,
                                            (void *)(uintptr_t)16, NULL);
    KASSERT_NOT_NULL(t);
    WAIT_DONE(2048);
    KASSERT(atomic_load(&g_9b3_done));
    KASSERT_EQ_U((uint64_t)g_9b3_rc, (uint64_t)5);
    KASSERT_EQ_U((unsigned char)g_9b3_buf[0], (unsigned char)'w');
    KASSERT_EQ_U((unsigned char)g_9b3_buf[4], (unsigned char)'d');
}

/* ---- Test 5: char_device_read with EOF signal --------------------- */

KTEST(char_device_read_eof_returns_zero)
{
    nx_console_reset_for_test();
    struct nx_slot *cs = nx_slot_lookup("char_device.serial");
    KASSERT_NOT_NULL(cs);

    nx_console_test_inject_eof();

    atomic_store(&g_9b3_done, 0);
    g_9b3_rc = -999;
    struct nx_task *t = sched_spawn_kthread("9b3-r5", char_read_kthread,
                                            (void *)(uintptr_t)16, NULL);
    KASSERT_NOT_NULL(t);
    WAIT_DONE(2048);
    KASSERT(atomic_load(&g_9b3_done));
    KASSERT_EQ_U((uint64_t)g_9b3_rc, (uint64_t)0);
}

/* ---- Test 6: char_device_read partial (fewer bytes than cap) ------ */

KTEST(char_device_read_partial_less_than_cap)
{
    nx_console_reset_for_test();
    struct nx_slot *cs = nx_slot_lookup("char_device.serial");
    KASSERT_NOT_NULL(cs);

    /* Inject 3 bytes; cap = 8 — read returns exactly 3. */
    size_t pushed = nx_console_test_inject_bytes("abc", 3);
    KASSERT_EQ_U(pushed, (uint64_t)3);

    atomic_store(&g_9b3_done, 0);
    g_9b3_rc = -999;
    struct nx_task *t = sched_spawn_kthread("9b3-r6", char_read_kthread,
                                            (void *)(uintptr_t)8, NULL);
    KASSERT_NOT_NULL(t);
    WAIT_DONE(2048);
    KASSERT(atomic_load(&g_9b3_done));
    KASSERT_EQ_U((uint64_t)g_9b3_rc, (uint64_t)3);
    KASSERT_EQ_U((unsigned char)g_9b3_buf[0], (unsigned char)'a');
    KASSERT_EQ_U((unsigned char)g_9b3_buf[2], (unsigned char)'c');
}

/* ---- Test 7: write increments observable console counter ---------- */

KTEST(char_device_write_increments_console_write_calls)
{
    nx_console_reset_for_test();
    struct nx_slot *cs = nx_slot_lookup("char_device.serial");
    KASSERT_NOT_NULL(cs);

    uint64_t before = nx_console_write_calls();

    static const struct char_write_arg arg7 = { "nx", 2 };
    atomic_store(&g_9b3_done, 0);
    g_9b3_rc = -999;
    struct nx_task *t = sched_spawn_kthread("9b3-w7", char_write_kthread,
                                            (void *)&arg7, NULL);
    KASSERT_NOT_NULL(t);
    WAIT_DONE(1024);
    KASSERT(atomic_load(&g_9b3_done));
    KASSERT_EQ_U((uint64_t)g_9b3_rc, (uint64_t)2);

    uint64_t after = nx_console_write_calls();
    KASSERT(after > before);
}

/* ---- Tests 8–9: VFS roundtrip and seek via slot_call_blocking ----- */

static int64_t g_9b3_vfs_rc;
static char    g_9b3_vfs_buf[32];

static void vfs_roundtrip_kthread(void *arg)
{
    (void)arg;
    struct nx_slot *vfs = nx_slot_lookup("vfs");

    uint32_t wid = nx_vfs_open(vfs, "/ktest_9b3_rw",
                               NX_VFS_OPEN_WRITE | NX_VFS_OPEN_CREATE);
    if (wid == 0) { g_9b3_vfs_rc = -1; atomic_store(&g_9b3_done, 1); return; }

    int64_t wrote = nx_vfs_write(vfs, wid, "slice9b3", 8);
    nx_vfs_close(vfs, wid);

    uint32_t rid = nx_vfs_open(vfs, "/ktest_9b3_rw", NX_VFS_OPEN_READ);
    if (rid == 0) { g_9b3_vfs_rc = -2; atomic_store(&g_9b3_done, 1); return; }

    int64_t got = nx_vfs_read(vfs, rid, g_9b3_vfs_buf, sizeof g_9b3_vfs_buf);
    nx_vfs_close(vfs, rid);

    g_9b3_vfs_rc = (wrote == 8 && got == 8) ? got : -3;
    atomic_store(&g_9b3_done, 1);
}

KTEST(vfs_write_read_roundtrip_via_slot_call_blocking)
{
    struct nx_slot *vfs = nx_slot_lookup("vfs");
    KASSERT_NOT_NULL(vfs);

    atomic_store(&g_9b3_done, 0);
    g_9b3_vfs_rc = -999;
    struct nx_task *t = sched_spawn_kthread("9b3-vfs8", vfs_roundtrip_kthread,
                                            NULL, NULL);
    KASSERT_NOT_NULL(t);
    WAIT_DONE(4096);
    KASSERT(atomic_load(&g_9b3_done));
    KASSERT_EQ_U((uint64_t)g_9b3_vfs_rc, (uint64_t)8);
    for (int i = 0; i < 8; i++)
        KASSERT_EQ_U((unsigned char)g_9b3_vfs_buf[i],
                     (unsigned char)"slice9b3"[i]);
}

static void vfs_seek_kthread(void *arg)
{
    (void)arg;
    struct nx_slot *vfs = nx_slot_lookup("vfs");

    uint32_t wid = nx_vfs_open(vfs, "/ktest_9b3_sk",
                               NX_VFS_OPEN_WRITE | NX_VFS_OPEN_CREATE);
    if (wid == 0) { g_9b3_vfs_rc = -1; atomic_store(&g_9b3_done, 1); return; }
    nx_vfs_write(vfs, wid, "abcde", 5);
    nx_vfs_close(vfs, wid);

    uint32_t rid = nx_vfs_open(vfs, "/ktest_9b3_sk", NX_VFS_OPEN_READ);
    if (rid == 0) { g_9b3_vfs_rc = -2; atomic_store(&g_9b3_done, 1); return; }

    /* Seek to offset 2 (absolute) then read 3 bytes — expect "cde". */
    int64_t pos = nx_vfs_seek(vfs, rid, 2, NX_VFS_SEEK_SET);
    int64_t got = nx_vfs_read(vfs, rid, g_9b3_vfs_buf, 3);
    nx_vfs_close(vfs, rid);

    g_9b3_vfs_rc = (pos == 2 && got == 3) ? got : -3;
    atomic_store(&g_9b3_done, 1);
}

KTEST(vfs_seek_repositions_via_slot_call_blocking)
{
    struct nx_slot *vfs = nx_slot_lookup("vfs");
    KASSERT_NOT_NULL(vfs);

    atomic_store(&g_9b3_done, 0);
    g_9b3_vfs_rc = -999;
    struct nx_task *t = sched_spawn_kthread("9b3-vfs9", vfs_seek_kthread,
                                            NULL, NULL);
    KASSERT_NOT_NULL(t);
    WAIT_DONE(4096);
    KASSERT(atomic_load(&g_9b3_done));
    KASSERT_EQ_U((uint64_t)g_9b3_vfs_rc, (uint64_t)3);
    KASSERT_EQ_U((unsigned char)g_9b3_vfs_buf[0], (unsigned char)'c');
    KASSERT_EQ_U((unsigned char)g_9b3_vfs_buf[2], (unsigned char)'e');
}

/* ---- Test 10: exact byte content match after read ----------------- */

KTEST(char_device_read_exact_byte_content_matches)
{
    nx_console_reset_for_test();
    struct nx_slot *cs = nx_slot_lookup("char_device.serial");
    KASSERT_NOT_NULL(cs);

    static const char payload[] = "test123";
    size_t pushed = nx_console_test_inject_bytes(payload,
                                                 sizeof payload - 1);
    KASSERT_EQ_U(pushed, (uint64_t)(sizeof payload - 1));

    atomic_store(&g_9b3_done, 0);
    g_9b3_rc = -999;
    struct nx_task *t = sched_spawn_kthread("9b3-r10", char_read_kthread,
                                            (void *)(uintptr_t)16, NULL);
    KASSERT_NOT_NULL(t);
    WAIT_DONE(2048);
    KASSERT(atomic_load(&g_9b3_done));
    KASSERT_EQ_U((uint64_t)g_9b3_rc, (uint64_t)(sizeof payload - 1));
    for (size_t i = 0; i < sizeof payload - 1; i++)
        KASSERT_EQ_U((unsigned char)g_9b3_buf[i],
                     (unsigned char)payload[i]);
}
