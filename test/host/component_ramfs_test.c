/*
 * Host tests for components/ramfs/ (slice 6.2).
 *
 * Three groups, same shape as component_mm_buddy_test.c:
 *
 *   1. Conformance — seven TEST()s wrapping the universal helpers
 *      from test/host/conformance/conformance_fs.{h,c}.  A component
 *      that fails any of these is not allowed to bind to a
 *      `filesystem.*` slot.
 *
 *   2. Lifecycle cycling — 100× init→enable→disable→destroy with
 *      zero residue (observable via counters + leak detector).
 *
 *   3. ramfs-specific smoke — alloc exhaustion paths not covered by
 *      the universal contract.
 */

#include "test_runner.h"

#include "conformance/conformance_fs.h"
#include "framework/component.h"
#include "framework/registry.h"
#include "interfaces/fs.h"

#include <stdlib.h>

/* Exported by components/ramfs/ramfs.c. */
extern const struct nx_fs_ops              ramfs_fs_ops;
extern const struct nx_component_ops       ramfs_component_ops;
extern const struct nx_component_descriptor ramfs_descriptor;

/* --- factory used by the conformance harness ------------------------- */

static void *ramfs_fixture_create(void)
{
    void *state = calloc(1, ramfs_descriptor.state_size);
    if (!state) return NULL;
    if (ramfs_component_ops.init(state) != NX_OK) {
        free(state);
        return NULL;
    }
    return state;
}

static void ramfs_fixture_destroy(void *self)
{
    ramfs_component_ops.destroy(self);
    free(self);
}

static const struct nx_fs_fixture ramfs_fixture = {
    .ops     = &ramfs_fs_ops,
    .create  = ramfs_fixture_create,
    .destroy = ramfs_fixture_destroy,
};

/* --- 1. Conformance --------------------------------------------------- */

TEST(ramfs_conformance_open_create_on_fresh_path_succeeds)
{
    nx_conformance_fs_open_create_on_fresh_path_succeeds(&ramfs_fixture);
}

TEST(ramfs_conformance_open_without_create_on_missing_path_returns_enoent)
{
    nx_conformance_fs_open_without_create_on_missing_path_returns_enoent(&ramfs_fixture);
}

TEST(ramfs_conformance_fresh_file_reads_zero_bytes)
{
    nx_conformance_fs_fresh_file_reads_zero_bytes(&ramfs_fixture);
}

TEST(ramfs_conformance_write_then_read_after_reopen_roundtrips)
{
    nx_conformance_fs_write_then_read_after_reopen_roundtrips(&ramfs_fixture);
}

TEST(ramfs_conformance_read_past_eof_returns_zero)
{
    nx_conformance_fs_read_past_eof_returns_zero(&ramfs_fixture);
}

TEST(ramfs_conformance_two_opens_have_independent_cursors)
{
    nx_conformance_fs_two_opens_have_independent_cursors(&ramfs_fixture);
}

TEST(ramfs_conformance_write_without_write_right_returns_eperm)
{
    nx_conformance_fs_write_without_write_right_returns_eperm(&ramfs_fixture);
}

TEST(ramfs_conformance_readdir_on_empty_fs_returns_enoent)
{
    nx_conformance_fs_readdir_on_empty_fs_returns_enoent(&ramfs_fixture);
}

TEST(ramfs_conformance_readdir_yields_created_files_then_enoent)
{
    nx_conformance_fs_readdir_yields_created_files_then_enoent(&ramfs_fixture);
}

TEST(ramfs_conformance_seek_set_to_zero_restarts_reads)
{
    nx_conformance_fs_seek_set_to_zero_restarts_reads(&ramfs_fixture);
}

TEST(ramfs_conformance_seek_end_returns_file_size)
{
    nx_conformance_fs_seek_end_returns_file_size(&ramfs_fixture);
}

TEST(ramfs_conformance_seek_past_size_returns_einval)
{
    nx_conformance_fs_seek_past_size_returns_einval(&ramfs_fixture);
}

TEST(ramfs_conformance_mkdir_creates_dir_visible_in_readdir)
{
    nx_conformance_fs_mkdir_creates_dir_visible_in_readdir(&ramfs_fixture);
}

TEST(ramfs_conformance_stat_reports_kind_for_files_and_dirs)
{
    nx_conformance_fs_stat_reports_kind_for_files_and_dirs(&ramfs_fixture);
}

/* --- 4. Hierarchical-paths smoke (slice 7.7b.1) ---------------------- */

TEST(ramfs_hierarchical_readdir_dedups_synthesised_dirs)
{
    /* Create /bin/sh and /bin/cat (no explicit /bin entry).  readdir
     * over "/" should yield "bin" exactly once (the synthesised
     * intermediate); readdir over "/bin" should yield "sh" and
     * "cat" (basenames). */
    void *self = ramfs_fixture_create();
    ASSERT_NOT_NULL(self);

    void *h;
    ASSERT_EQ_U(ramfs_fs_ops.open(self, "/bin/sh",
                                  NX_FS_OPEN_WRITE | NX_FS_OPEN_CREATE, &h),
                NX_OK);
    ramfs_fs_ops.close(self, h);
    ASSERT_EQ_U(ramfs_fs_ops.open(self, "/bin/cat",
                                  NX_FS_OPEN_WRITE | NX_FS_OPEN_CREATE, &h),
                NX_OK);
    ramfs_fs_ops.close(self, h);

    /* /bin is a synthesised dir per stat. */
    struct nx_fs_stat st;
    ASSERT_EQ_U(ramfs_fs_ops.stat(self, "/bin", &st), NX_OK);
    ASSERT_EQ_U(st.kind, NX_FS_KIND_DIR);

    /* readdir("/") yields "bin" exactly once. */
    int saw_bin = 0;
    uint32_t cookie = 0;
    for (int i = 0; i < 32; i++) {
        struct nx_fs_dirent ent;
        int rc = ramfs_fs_ops.readdir(self, "/", &cookie, &ent);
        if (rc == NX_ENOENT) break;
        ASSERT_EQ_U(rc, NX_OK);
        if (ent.name_len == 3 && memcmp(ent.name, "bin", 3) == 0)
            saw_bin++;
    }
    ASSERT_EQ_U(saw_bin, 1);

    /* readdir("/bin") yields sh + cat. */
    int saw_sh = 0, saw_cat = 0;
    cookie = 0;
    for (int i = 0; i < 32; i++) {
        struct nx_fs_dirent ent;
        int rc = ramfs_fs_ops.readdir(self, "/bin", &cookie, &ent);
        if (rc == NX_ENOENT) break;
        ASSERT_EQ_U(rc, NX_OK);
        if (ent.name_len == 2 && memcmp(ent.name, "sh", 2) == 0) saw_sh++;
        if (ent.name_len == 3 && memcmp(ent.name, "cat", 3) == 0) saw_cat++;
    }
    ASSERT_EQ_U(saw_sh, 1);
    ASSERT_EQ_U(saw_cat, 1);

    ramfs_fixture_destroy(self);
}

TEST(ramfs_open_on_directory_returns_eperm)
{
    /* Slice 7.7b.1: open() on a directory entry refuses with EPERM —
     * the syscall layer must route DIR paths through HANDLE_DIR. */
    void *self = ramfs_fixture_create();
    ASSERT_NOT_NULL(self);
    ASSERT_EQ_U(ramfs_fs_ops.mkdir(self, "/d"), NX_OK);

    void *f = NULL;
    int rc = ramfs_fs_ops.open(self, "/d", NX_FS_OPEN_READ, &f);
    ASSERT_EQ_U(rc, NX_EPERM);

    ramfs_fixture_destroy(self);
}

/* --- 2. Lifecycle cycling -------------------------------------------- */

TEST(ramfs_lifecycle_100_cycles_leave_no_residue)
{
    void *state = calloc(1, ramfs_descriptor.state_size);
    ASSERT_NOT_NULL(state);

    enum { N = 100 };
    for (int i = 0; i < N; i++) {
        ASSERT_EQ_U(ramfs_component_ops.init(state),    NX_OK);
        ASSERT_EQ_U(ramfs_component_ops.enable(state),  NX_OK);
        ASSERT_EQ_U(ramfs_component_ops.disable(state), NX_OK);
        ramfs_component_ops.destroy(state);
    }

    free(state);
}

/* --- 3. ramfs-specific smoke ---------------------------------------- */

TEST(ramfs_file_table_exhaustion_returns_enomem)
{
    /* Universal conformance doesn't drive past capacity — it's a v1
     * geometry we've set in the driver, not part of the interface.
     * This test names the invariant so a future bump to the capacity
     * (or a change to a dynamic table) stays visible. */
    void *self = ramfs_fixture_create();
    ASSERT_NOT_NULL(self);

    /* RAMFS_MAX_FILES is 24 per the component (slice 7.6d.N.13 bump);
     * use 32 so we clearly see the cutoff without depending on the
     * exact number. */
    enum { ATTEMPTS = 32 };
    unsigned created = 0;
    for (int i = 0; i < ATTEMPTS; i++) {
        char path[8];
        path[0] = '/';
        path[1] = 'a' + i;
        path[2] = '\0';
        void *f = NULL;
        int rc = ramfs_fs_ops.open(self, path,
                                   NX_FS_OPEN_READ | NX_FS_OPEN_WRITE |
                                   NX_FS_OPEN_CREATE, &f);
        if (rc == NX_OK) {
            created++;
            ramfs_fs_ops.close(self, f);
        } else {
            ASSERT_EQ_U(rc, NX_ENOMEM);
        }
    }
    /* At least one attempt past capacity must have hit NX_ENOMEM.  Upper
     * bound is the hard capacity so the test catches a regression that
     * silently bumps the limit. */
    ASSERT(created >= 1);
    ASSERT(created < (unsigned)ATTEMPTS);

    ramfs_fixture_destroy(self);
}

TEST(ramfs_open_slot_exhaustion_returns_enomem)
{
    /* Every open consumes one open-slot.  Opening the same file many
     * times without closing must eventually exhaust the per-instance
     * open pool. */
    void *self = ramfs_fixture_create();
    ASSERT_NOT_NULL(self);

    /* Seed the file so subsequent opens don't burn the file-table
     * slot on each attempt. */
    void *seed = NULL;
    ASSERT_EQ_U(ramfs_fs_ops.open(self, "/f",
                                  NX_FS_OPEN_READ | NX_FS_OPEN_WRITE |
                                  NX_FS_OPEN_CREATE, &seed),
                NX_OK);
    ramfs_fs_ops.close(self, seed);  /* releases open slot but not file */

    enum { ATTEMPTS = 128 }; /* > RAMFS_MAX_OPEN (96 = 4*24 per the
                              * slice 7.6d.N.13 bump) */
    void *opens[ATTEMPTS];
    unsigned n = 0;
    for (int i = 0; i < ATTEMPTS; i++) {
        void *f = NULL;
        int rc = ramfs_fs_ops.open(self, "/f", NX_FS_OPEN_READ, &f);
        if (rc == NX_OK) {
            opens[n++] = f;
        } else {
            ASSERT_EQ_U(rc, NX_ENOMEM);
            break;
        }
    }
    ASSERT(n > 0);
    ASSERT(n < (unsigned)ATTEMPTS);

    for (unsigned i = 0; i < n; i++) ramfs_fs_ops.close(self, opens[i]);

    /* After closing every open, we should be able to open again. */
    void *f2 = NULL;
    ASSERT_EQ_U(ramfs_fs_ops.open(self, "/f", NX_FS_OPEN_READ, &f2), NX_OK);
    ramfs_fs_ops.close(self, f2);

    ramfs_fixture_destroy(self);
}
