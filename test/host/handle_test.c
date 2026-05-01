/*
 * Host-side tests for the handle framework (slice 5.3).
 *
 * Coverage groups:
 *
 *   1. Alloc / lookup / close round-trips.
 *   2. Generation counter: closed handle fails subsequent lookup; slot
 *      reuse after close produces a fresh handle whose old value stays
 *      invalid.
 *   3. Duplicate with rights attenuation, including escalation-is-EPERM.
 *   4. Capacity + empty-table behaviour.
 *   5. 1000-iteration stress to catch refcount / free-scan bugs.
 */

#include "test_runner.h"

#include "framework/handle.h"
#include "framework/process.h"
#include "framework/registry.h"   /* NX_OK / NX_E* */

#include <string.h>

static void table_reset(struct nx_handle_table *t)
{
    memset(t, 0, sizeof *t);
    nx_handle_table_init(t);
}

/* --- 1. Alloc / lookup / close -------------------------------------- */

TEST(handle_alloc_returns_ok_and_nonzero_handle)
{
    struct nx_handle_table t;
    table_reset(&t);

    int dummy = 42;
    nx_handle_t h = NX_HANDLE_INVALID;
    ASSERT_EQ_U(nx_handle_alloc(&t, NX_HANDLE_CHANNEL,
                                NX_RIGHT_READ | NX_RIGHT_WRITE,
                                &dummy, &h), NX_OK);
    ASSERT(h != NX_HANDLE_INVALID);
    ASSERT_EQ_U(nx_handle_table_count(&t), 1);
}

TEST(handle_lookup_returns_alloced_type_rights_object)
{
    struct nx_handle_table t;
    table_reset(&t);

    int dummy = 99;
    nx_handle_t h;
    ASSERT_EQ_U(nx_handle_alloc(&t, NX_HANDLE_VMO,
                                NX_RIGHT_MAP | NX_RIGHT_READ,
                                &dummy, &h), NX_OK);

    enum nx_handle_type type;
    uint32_t rights;
    void *obj;
    ASSERT_EQ_U(nx_handle_lookup(&t, h, &type, &rights, &obj), NX_OK);
    ASSERT_EQ_U((unsigned)type, (unsigned)NX_HANDLE_VMO);
    ASSERT_EQ_U(rights, NX_RIGHT_MAP | NX_RIGHT_READ);
    ASSERT_EQ_PTR(obj, &dummy);
}

TEST(handle_close_makes_subsequent_lookup_return_enoent)
{
    struct nx_handle_table t;
    table_reset(&t);

    int dummy = 1;
    nx_handle_t h;
    nx_handle_alloc(&t, NX_HANDLE_CHANNEL, NX_RIGHT_READ, &dummy, &h);

    ASSERT_EQ_U(nx_handle_close(&t, h), NX_OK);
    ASSERT_EQ_U(nx_handle_table_count(&t), 0);

    /* Second close of the same value must not succeed. */
    ASSERT_EQ_U(nx_handle_close(&t, h), NX_ENOENT);

    /* Lookup of the closed handle value must miss. */
    ASSERT_EQ_U(nx_handle_lookup(&t, h, NULL, NULL, NULL), NX_ENOENT);
}

/* --- 2. Generation counter ------------------------------------------ */

TEST(handle_value_after_reuse_differs_from_pre_close_value)
{
    /* Alloc, close, alloc again.  The second alloc MAY reuse the same
     * slot (expected: yes, since it's the first free slot), but the
     * returned handle value must differ because generation bumped on
     * close.  Pre-close handle values must no longer resolve. */
    struct nx_handle_table t;
    table_reset(&t);

    int dummy_a = 1, dummy_b = 2;
    nx_handle_t h1;
    nx_handle_alloc(&t, NX_HANDLE_CHANNEL, NX_RIGHT_READ, &dummy_a, &h1);
    nx_handle_close(&t, h1);

    nx_handle_t h2;
    nx_handle_alloc(&t, NX_HANDLE_CHANNEL, NX_RIGHT_WRITE, &dummy_b, &h2);
    ASSERT(h1 != h2);

    /* h1 is stale: points to the same slot as h2 but the generation
     * encoded in h1 no longer matches. */
    ASSERT_EQ_U(nx_handle_lookup(&t, h1, NULL, NULL, NULL), NX_ENOENT);

    /* h2 resolves to the new object. */
    void *obj;
    ASSERT_EQ_U(nx_handle_lookup(&t, h2, NULL, NULL, &obj), NX_OK);
    ASSERT_EQ_PTR(obj, &dummy_b);
}

/* --- 3. Duplicate / rights attenuation ------------------------------ */

TEST(handle_duplicate_with_same_rights_succeeds)
{
    struct nx_handle_table t;
    table_reset(&t);

    int dummy = 0;
    nx_handle_t src;
    nx_handle_alloc(&t, NX_HANDLE_CHANNEL,
                    NX_RIGHT_READ | NX_RIGHT_WRITE | NX_RIGHT_TRANSFER,
                    &dummy, &src);

    nx_handle_t dup;
    ASSERT_EQ_U(nx_handle_duplicate(&t, src,
                                    NX_RIGHT_READ | NX_RIGHT_WRITE | NX_RIGHT_TRANSFER,
                                    &dup), NX_OK);
    ASSERT(dup != src);

    uint32_t rights;
    ASSERT_EQ_U(nx_handle_lookup(&t, dup, NULL, &rights, NULL), NX_OK);
    ASSERT_EQ_U(rights, NX_RIGHT_READ | NX_RIGHT_WRITE | NX_RIGHT_TRANSFER);
}

TEST(handle_duplicate_with_reduced_rights_succeeds)
{
    struct nx_handle_table t;
    table_reset(&t);

    int dummy = 0;
    nx_handle_t src;
    nx_handle_alloc(&t, NX_HANDLE_CHANNEL,
                    NX_RIGHT_READ | NX_RIGHT_WRITE,
                    &dummy, &src);

    nx_handle_t readonly;
    ASSERT_EQ_U(nx_handle_duplicate(&t, src, NX_RIGHT_READ, &readonly), NX_OK);

    uint32_t rights;
    ASSERT_EQ_U(nx_handle_lookup(&t, readonly, NULL, &rights, NULL), NX_OK);
    ASSERT_EQ_U(rights, NX_RIGHT_READ);
}

TEST(handle_duplicate_expanding_rights_returns_eperm)
{
    struct nx_handle_table t;
    table_reset(&t);

    int dummy = 0;
    nx_handle_t src;
    nx_handle_alloc(&t, NX_HANDLE_CHANNEL, NX_RIGHT_READ, &dummy, &src);

    /* Ask for WRITE on top of READ — escalation. */
    nx_handle_t dup;
    ASSERT_EQ_U(nx_handle_duplicate(&t, src,
                                    NX_RIGHT_READ | NX_RIGHT_WRITE,
                                    &dup),
                NX_EPERM);

    /* Ask for TRANSFER instead of READ — completely different bit. */
    ASSERT_EQ_U(nx_handle_duplicate(&t, src, NX_RIGHT_TRANSFER, &dup),
                NX_EPERM);

    /* Table should still hold only the original handle. */
    ASSERT_EQ_U(nx_handle_table_count(&t), 1);
}

TEST(handle_duplicate_of_closed_source_returns_enoent)
{
    struct nx_handle_table t;
    table_reset(&t);

    int dummy = 0;
    nx_handle_t src;
    nx_handle_alloc(&t, NX_HANDLE_CHANNEL, NX_RIGHT_READ, &dummy, &src);
    nx_handle_close(&t, src);

    nx_handle_t dup;
    ASSERT_EQ_U(nx_handle_duplicate(&t, src, NX_RIGHT_READ, &dup),
                NX_ENOENT);
}

/* --- 4. Capacity + edge cases --------------------------------------- */

TEST(handle_alloc_fills_capacity_then_returns_enomem)
{
    struct nx_handle_table t;
    table_reset(&t);

    int dummy = 0;
    nx_handle_t handles[NX_HANDLE_TABLE_CAPACITY];

    for (size_t i = 0; i < NX_HANDLE_TABLE_CAPACITY; i++)
        ASSERT_EQ_U(nx_handle_alloc(&t, NX_HANDLE_CHANNEL, NX_RIGHT_READ,
                                    &dummy, &handles[i]), NX_OK);

    ASSERT_EQ_U(nx_handle_table_count(&t), NX_HANDLE_TABLE_CAPACITY);

    nx_handle_t extra;
    ASSERT_EQ_U(nx_handle_alloc(&t, NX_HANDLE_CHANNEL, NX_RIGHT_READ,
                                &dummy, &extra), NX_ENOMEM);

    /* Freeing one then re-allocating must succeed. */
    nx_handle_close(&t, handles[0]);
    ASSERT_EQ_U(nx_handle_alloc(&t, NX_HANDLE_VMO, NX_RIGHT_MAP,
                                &dummy, &extra), NX_OK);
}

TEST(handle_zero_value_and_null_table_rejected_cleanly)
{
    struct nx_handle_table t;
    table_reset(&t);

    /* Lookup of NX_HANDLE_INVALID is EINVAL, not a segfault. */
    ASSERT_EQ_U(nx_handle_lookup(&t, NX_HANDLE_INVALID, NULL, NULL, NULL),
                NX_EINVAL);

    /* Close of NX_HANDLE_INVALID is EINVAL. */
    ASSERT_EQ_U(nx_handle_close(&t, NX_HANDLE_INVALID), NX_EINVAL);

    /* NULL table on every entry point returns EINVAL, not a crash. */
    int dummy = 0;
    nx_handle_t h;
    int rc = nx_handle_alloc(NULL, NX_HANDLE_CHANNEL, NX_RIGHT_READ, &dummy, &h);
    ASSERT_EQ_U(rc, NX_EINVAL);
    rc = nx_handle_lookup(NULL, 1, NULL, NULL, NULL);
    ASSERT_EQ_U(rc, NX_EINVAL);
    rc = nx_handle_close(NULL, 1);
    ASSERT_EQ_U(rc, NX_EINVAL);
}

TEST(handle_alloc_rejects_invalid_type)
{
    struct nx_handle_table t;
    table_reset(&t);

    int dummy = 0;
    nx_handle_t h;
    int rc;
    rc = nx_handle_alloc(&t, NX_HANDLE_INVALID, NX_RIGHT_READ, &dummy, &h);
    ASSERT_EQ_U(rc, NX_EINVAL);
    rc = nx_handle_alloc(&t, NX_HANDLE_TYPE_COUNT, NX_RIGHT_READ, &dummy, &h);
    ASSERT_EQ_U(rc, NX_EINVAL);
    /* Out-of-range (way past TYPE_COUNT). */
    rc = nx_handle_alloc(&t, (enum nx_handle_type)255,
                         NX_RIGHT_READ, &dummy, &h);
    ASSERT_EQ_U(rc, NX_EINVAL);
}

TEST(handle_alloc_rejects_null_object)
{
    struct nx_handle_table t;
    table_reset(&t);

    nx_handle_t h;
    int rc = nx_handle_alloc(&t, NX_HANDLE_CHANNEL, NX_RIGHT_READ, NULL, &h);
    ASSERT_EQ_U(rc, NX_EINVAL);
}

/* --- 5. Stress ------------------------------------------------------- */

TEST(handle_alloc_close_1000_cycles_is_leak_free)
{
    struct nx_handle_table t;
    table_reset(&t);

    int dummy = 7;
    for (int i = 0; i < 1000; i++) {
        nx_handle_t h;
        ASSERT_EQ_U(nx_handle_alloc(&t, NX_HANDLE_CHANNEL,
                                    NX_RIGHT_READ, &dummy, &h), NX_OK);
        ASSERT_EQ_U(nx_handle_close(&t, h), NX_OK);
    }
    ASSERT_EQ_U(nx_handle_table_count(&t), 0);
}

/* --- 6. Slot wiring (slice 8.0a.7) ---------------------------------- */

TEST(handle_alloc_with_slot_wires_slot_field)
{
    struct nx_handle_table t;
    table_reset(&t);

    int dummy = 1;
    /* Use a stack int as a fake slot pointer — handle.c only compares
     * the pointer value, never dereferences it in these helpers. */
    struct nx_slot *fake_slot = (struct nx_slot *)(uintptr_t)0xABCD1234u;

    nx_handle_t h = NX_HANDLE_INVALID;
    ASSERT_EQ_U(nx_handle_alloc_with_slot(&t, NX_HANDLE_FILE,
                                          NX_RIGHT_READ, &dummy,
                                          fake_slot, &h), NX_OK);
    ASSERT(h != NX_HANDLE_INVALID);

    /* The slot must be stored in the raw entry. */
    size_t idx = (h & 0xFFu) - 1u;
    ASSERT_EQ_PTR(t.entries[idx].slot, fake_slot);
}

TEST(handle_alloc_with_null_slot_leaves_entry_immune)
{
    struct nx_handle_table t;
    table_reset(&t);

    int dummy = 2;
    nx_handle_t h = NX_HANDLE_INVALID;
    ASSERT_EQ_U(nx_handle_alloc_with_slot(&t, NX_HANDLE_CHANNEL,
                                          NX_RIGHT_READ, &dummy,
                                          NULL, &h), NX_OK);
    size_t idx = (h & 0xFFu) - 1u;
    ASSERT_EQ_PTR(t.entries[idx].slot, NULL);
}

TEST(handle_set_slot_wires_on_valid_handle)
{
    struct nx_handle_table t;
    table_reset(&t);

    int dummy = 3;
    nx_handle_t h = NX_HANDLE_INVALID;
    nx_handle_alloc(&t, NX_HANDLE_FILE, NX_RIGHT_READ, &dummy, &h);

    struct nx_slot *fake_slot = (struct nx_slot *)(uintptr_t)0xDEADBEEFu;
    nx_handle_set_slot(&t, h, fake_slot);

    size_t idx = (h & 0xFFu) - 1u;
    ASSERT_EQ_PTR(t.entries[idx].slot, fake_slot);
}

TEST(handle_set_slot_noop_if_already_wired)
{
    struct nx_handle_table t;
    table_reset(&t);

    int dummy = 4;
    nx_handle_t h = NX_HANDLE_INVALID;
    struct nx_slot *slot_a = (struct nx_slot *)(uintptr_t)0x1000u;
    struct nx_slot *slot_b = (struct nx_slot *)(uintptr_t)0x2000u;
    nx_handle_alloc_with_slot(&t, NX_HANDLE_FILE, NX_RIGHT_READ, &dummy, slot_a, &h);
    nx_handle_set_slot(&t, h, slot_b);  /* second call must be a no-op */

    size_t idx = (h & 0xFFu) - 1u;
    ASSERT_EQ_PTR(t.entries[idx].slot, slot_a);  /* unchanged */
}

TEST(handle_close_clears_slot_field)
{
    struct nx_handle_table t;
    table_reset(&t);

    int dummy = 5;
    nx_handle_t h = NX_HANDLE_INVALID;
    struct nx_slot *fake_slot = (struct nx_slot *)(uintptr_t)0x3000u;
    nx_handle_alloc_with_slot(&t, NX_HANDLE_FILE, NX_RIGHT_READ, &dummy, fake_slot, &h);

    size_t idx = (h & 0xFFu) - 1u;
    ASSERT_EQ_PTR(t.entries[idx].slot, fake_slot);

    nx_handle_close(&t, h);
    ASSERT_EQ_PTR(t.entries[idx].slot, NULL);
}

TEST(handle_duplicate_propagates_slot)
{
    struct nx_handle_table t;
    table_reset(&t);

    int dummy = 6;
    nx_handle_t src = NX_HANDLE_INVALID;
    struct nx_slot *fake_slot = (struct nx_slot *)(uintptr_t)0x4000u;
    nx_handle_alloc_with_slot(&t, NX_HANDLE_FILE,
                              NX_RIGHT_READ | NX_RIGHT_WRITE,
                              &dummy, fake_slot, &src);

    nx_handle_t dup = NX_HANDLE_INVALID;
    ASSERT_EQ_U(nx_handle_duplicate(&t, src, NX_RIGHT_READ, &dup), NX_OK);

    size_t dup_idx = (dup & 0xFFu) - 1u;
    ASSERT_EQ_PTR(t.entries[dup_idx].slot, fake_slot);
}

TEST(handle_duplicate_null_slot_stays_null)
{
    struct nx_handle_table t;
    table_reset(&t);

    int dummy = 7;
    nx_handle_t src = NX_HANDLE_INVALID;
    nx_handle_alloc(&t, NX_HANDLE_CHANNEL, NX_RIGHT_READ, &dummy, &src);

    nx_handle_t dup = NX_HANDLE_INVALID;
    nx_handle_duplicate(&t, src, NX_RIGHT_READ, &dup);

    size_t dup_idx = (dup & 0xFFu) - 1u;
    ASSERT_EQ_PTR(t.entries[dup_idx].slot, NULL);
}

TEST(handle_invalidate_for_slot_clears_matching_entries)
{
    /* nx_process_create pre-installs 3 CONSOLE handles; capture the
     * baseline count so the assertions are independent of that. */
    struct nx_process *p = nx_process_create("inval_test");
    ASSERT(p != NULL);
    size_t base = nx_handle_table_count(&p->handles);

    int file_obj = 10, other_obj = 20;
    struct nx_slot *vfs_slot   = (struct nx_slot *)(uintptr_t)0x5000u;
    struct nx_slot *other_slot = (struct nx_slot *)(uintptr_t)0x6000u;

    nx_handle_t h_file, h_file2, h_other;
    nx_handle_alloc_with_slot(&p->handles, NX_HANDLE_FILE, NX_RIGHT_READ,
                              &file_obj, vfs_slot, &h_file);
    nx_handle_alloc_with_slot(&p->handles, NX_HANDLE_FILE, NX_RIGHT_READ,
                              &file_obj, vfs_slot, &h_file2);
    nx_handle_alloc_with_slot(&p->handles, NX_HANDLE_CHANNEL, NX_RIGHT_READ,
                              &other_obj, other_slot, &h_other);
    ASSERT_EQ_U(nx_handle_table_count(&p->handles), base + 3);

    nx_handle_table_invalidate_for_slot(vfs_slot);

    /* Both vfs-backed handles are gone. */
    ASSERT_EQ_U(nx_handle_lookup(&p->handles, h_file,  NULL, NULL, NULL), NX_ENOENT);
    ASSERT_EQ_U(nx_handle_lookup(&p->handles, h_file2, NULL, NULL, NULL), NX_ENOENT);
    /* The other-slot handle and all CONSOLE handles are unaffected. */
    ASSERT_EQ_U(nx_handle_lookup(&p->handles, h_other, NULL, NULL, NULL), NX_OK);
    ASSERT_EQ_U(nx_handle_table_count(&p->handles), base + 1);

    nx_process_destroy(p);
}

TEST(handle_invalidate_for_slot_ignores_null_slot_entries)
{
    struct nx_process *p = nx_process_create("inval_immune_test");
    ASSERT(p != NULL);
    size_t base = nx_handle_table_count(&p->handles);

    int obj = 30;
    struct nx_slot *vfs_slot = (struct nx_slot *)(uintptr_t)0x7000u;

    nx_handle_t h_wired, h_immune;
    nx_handle_alloc_with_slot(&p->handles, NX_HANDLE_FILE, NX_RIGHT_READ,
                              &obj, vfs_slot, &h_wired);
    nx_handle_alloc(&p->handles, NX_HANDLE_CHANNEL, NX_RIGHT_READ,
                    &obj, &h_immune);  /* NULL slot — immune */

    nx_handle_table_invalidate_for_slot(vfs_slot);

    ASSERT_EQ_U(nx_handle_lookup(&p->handles, h_wired,  NULL, NULL, NULL), NX_ENOENT);
    ASSERT_EQ_U(nx_handle_lookup(&p->handles, h_immune, NULL, NULL, NULL), NX_OK);
    ASSERT_EQ_U(nx_handle_table_count(&p->handles), base + 1);

    nx_process_destroy(p);
}

TEST(handle_interleaved_alloc_close_sustains_capacity)
{
    /* Fill, drain in random-ish order, refill.  Exposes bugs where
     * close doesn't return the slot to the free pool, or alloc leaks
     * a slot on error. */
    struct nx_handle_table t;
    table_reset(&t);

    int dummy = 0;
    nx_handle_t handles[NX_HANDLE_TABLE_CAPACITY];

    for (size_t i = 0; i < NX_HANDLE_TABLE_CAPACITY; i++)
        nx_handle_alloc(&t, NX_HANDLE_CHANNEL, NX_RIGHT_READ, &dummy, &handles[i]);

    /* Close every other one. */
    for (size_t i = 0; i < NX_HANDLE_TABLE_CAPACITY; i += 2)
        nx_handle_close(&t, handles[i]);
    ASSERT_EQ_U(nx_handle_table_count(&t), NX_HANDLE_TABLE_CAPACITY / 2);

    /* Refill — every slot should be reusable. */
    for (size_t i = 0; i < NX_HANDLE_TABLE_CAPACITY / 2; i++) {
        nx_handle_t h;
        ASSERT_EQ_U(nx_handle_alloc(&t, NX_HANDLE_VMO, NX_RIGHT_MAP,
                                    &dummy, &h), NX_OK);
    }
    ASSERT_EQ_U(nx_handle_table_count(&t), NX_HANDLE_TABLE_CAPACITY);
}
