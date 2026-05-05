/*
 * Host-side tests for slice 9b.2 — Handle entry redesign.
 *
 * Coverage:
 *   1.  nx_handle_alloc_resource creates a NX_HANDLE_RESOURCE entry.
 *   2.  nx_handle_alloc_resource with id=0 (console) succeeds.
 *   3.  nx_handle_alloc_resource stores id and target correctly.
 *   4.  nx_handle_entry_get returns the live entry for a valid handle.
 *   5.  nx_handle_entry_get returns NULL for a closed handle.
 *   6.  nx_handle_entry_get returns NULL for NX_HANDLE_INVALID.
 *   7.  nx_handle_close frees a RESOURCE handle and clears target.
 *   8.  Pre-installed console handles in nx_process_create are RESOURCE type.
 */

#include "test_runner.h"

#include "framework/handle.h"
#include "framework/process.h"
#include "framework/registry.h"

#include <string.h>

static void table_reset(struct nx_handle_table *t)
{
    memset(t, 0, sizeof *t);
    nx_handle_table_init(t);
}

/* fake slot pointers — handle.c only compares addresses, never dereferences */
static struct nx_slot *fake_vfs   = (struct nx_slot *)(uintptr_t)0x1000u;
static struct nx_slot *fake_char  = (struct nx_slot *)(uintptr_t)0x2000u;

/* --- 1. alloc_resource creates NX_HANDLE_RESOURCE -------------------- */

TEST(handle_alloc_resource_type_is_resource)
{
    struct nx_handle_table t;
    table_reset(&t);

    nx_handle_t h = NX_HANDLE_INVALID;
    ASSERT_EQ_U(nx_handle_alloc_resource(&t, NX_RIGHT_READ, 7, fake_vfs, &h),
                NX_OK);
    ASSERT(h != NX_HANDLE_INVALID);

    enum nx_handle_type type;
    ASSERT_EQ_U(nx_handle_lookup(&t, h, &type, NULL, NULL), NX_OK);
    ASSERT_EQ_U((unsigned)type, (unsigned)NX_HANDLE_RESOURCE);
    ASSERT_EQ_U(nx_handle_table_count(&t), 1);
}

/* --- 2. id=0 (console singleton) is accepted ------------------------- */

TEST(handle_alloc_resource_id_zero_succeeds)
{
    struct nx_handle_table t;
    table_reset(&t);

    nx_handle_t h = NX_HANDLE_INVALID;
    ASSERT_EQ_U(nx_handle_alloc_resource(&t, NX_RIGHT_WRITE, 0, fake_char, &h),
                NX_OK);
    ASSERT(h != NX_HANDLE_INVALID);
    ASSERT_EQ_U(nx_handle_table_count(&t), 1);
}

/* --- 3. id and target stored correctly -------------------------------- */

TEST(handle_alloc_resource_id_and_target_accessible_via_entry_get)
{
    struct nx_handle_table t;
    table_reset(&t);

    nx_handle_t h = NX_HANDLE_INVALID;
    ASSERT_EQ_U(nx_handle_alloc_resource(&t, NX_RIGHT_READ | NX_RIGHT_WRITE,
                                         42, fake_vfs, &h), NX_OK);

    const struct nx_handle_entry *e = nx_handle_entry_get(&t, h);
    ASSERT(e != NULL);
    ASSERT_EQ_U((unsigned)e->type, (unsigned)NX_HANDLE_RESOURCE);
    ASSERT_EQ_U(e->id, 42u);
    ASSERT_EQ_PTR(e->target, fake_vfs);
    ASSERT_EQ_U(e->rights, NX_RIGHT_READ | NX_RIGHT_WRITE);
}

/* --- 4. nx_handle_entry_get valid handle ----------------------------- */

TEST(handle_entry_get_returns_live_entry_for_valid_handle)
{
    struct nx_handle_table t;
    table_reset(&t);

    int dummy = 1;
    nx_handle_t h = NX_HANDLE_INVALID;
    nx_handle_alloc(&t, NX_HANDLE_CHANNEL, NX_RIGHT_READ | NX_RIGHT_WRITE,
                    &dummy, &h);

    const struct nx_handle_entry *e = nx_handle_entry_get(&t, h);
    ASSERT(e != NULL);
    ASSERT_EQ_U((unsigned)e->type, (unsigned)NX_HANDLE_CHANNEL);
    ASSERT_EQ_PTR(e->object, &dummy);
}

/* --- 5. nx_handle_entry_get closed handle → NULL --------------------- */

TEST(handle_entry_get_returns_null_for_closed_handle)
{
    struct nx_handle_table t;
    table_reset(&t);

    nx_handle_t h = NX_HANDLE_INVALID;
    nx_handle_alloc_resource(&t, NX_RIGHT_READ, 1, fake_vfs, &h);
    nx_handle_close(&t, h);

    ASSERT_EQ_PTR(nx_handle_entry_get(&t, h), NULL);
}

/* --- 6. nx_handle_entry_get for NX_HANDLE_INVALID → NULL ------------- */

TEST(handle_entry_get_returns_null_for_invalid_handle)
{
    struct nx_handle_table t;
    table_reset(&t);

    ASSERT_EQ_PTR(nx_handle_entry_get(&t, NX_HANDLE_INVALID), NULL);
}

/* --- 7. close clears target field ------------------------------------ */

TEST(handle_close_clears_resource_target)
{
    struct nx_handle_table t;
    table_reset(&t);

    nx_handle_t h = NX_HANDLE_INVALID;
    nx_handle_alloc_resource(&t, NX_RIGHT_READ, 3, fake_vfs, &h);

    size_t idx = (h & 0xFFu) - 1u;
    ASSERT_EQ_PTR(t.entries[idx].target, fake_vfs);

    nx_handle_close(&t, h);
    ASSERT_EQ_PTR(t.entries[idx].target, NULL);
    ASSERT_EQ_U(nx_handle_table_count(&t), 0);
}

/* --- 8. nx_process_create pre-installs RESOURCE console handles ------ */

TEST(process_create_pre_installs_resource_console_handles)
{
    struct nx_process *p = nx_process_create("9b2_test");
    ASSERT(p != NULL);

    /* Slots 0/1/2 = STDOUT/STDERR/STDIN — must be RESOURCE type, id=0. */
    for (size_t i = 0; i < 3; i++) {
        const struct nx_handle_entry *e = &p->handles.entries[i];
        ASSERT_EQ_U((unsigned)e->type, (unsigned)NX_HANDLE_RESOURCE);
        ASSERT_EQ_U(e->id, 0u);
    }

    /* stdout/stderr (slots 0/1) carry WRITE right; stdin (slot 2) carries READ. */
    ASSERT_EQ_U(p->handles.entries[0].rights & NX_RIGHT_WRITE, NX_RIGHT_WRITE);
    ASSERT_EQ_U(p->handles.entries[1].rights & NX_RIGHT_WRITE, NX_RIGHT_WRITE);
    ASSERT_EQ_U(p->handles.entries[2].rights & NX_RIGHT_READ,  NX_RIGHT_READ);

    ASSERT_EQ_U(nx_handle_table_count(&p->handles), 3);

    nx_process_destroy(p);
}
