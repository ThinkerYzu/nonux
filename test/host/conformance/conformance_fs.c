/*
 * Filesystem-driver conformance suite — slice 9b.1 update.
 *
 * Slice 9b.1 changes the fs API: `open` now returns a uint32_t open-ID
 * (0 = failure, non-zero = valid ID) instead of writing to `void **out_file`.
 * All per-open ops (close, retain, read, write, seek) take `uint32_t id`
 * instead of `void *file`.  The conformance suite is updated accordingly.
 */

#include "conformance_fs.h"

#include "framework/registry.h"
#include "test/host/test_runner.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---------- helpers --------------------------------------------------- */

static uint32_t open_new_rw(const struct nx_fs_fixture *f, void *self,
                            const char *path)
{
    return f->ops->open(self, path,
                        NX_FS_OPEN_READ | NX_FS_OPEN_WRITE |
                        NX_FS_OPEN_CREATE);
}

/* --- case 1: open CREATE on a fresh path succeeds --------------------- */

void nx_conformance_fs_open_create_on_fresh_path_succeeds(
    const struct nx_fs_fixture *f)
{
    ASSERT_NOT_NULL(f);
    ASSERT_NOT_NULL(f->ops);
    ASSERT_NOT_NULL(f->ops->open);
    ASSERT_NOT_NULL(f->ops->close);

    void *self = f->create();
    ASSERT_NOT_NULL(self);

    uint32_t id = f->ops->open(self, "/a",
                               NX_FS_OPEN_READ | NX_FS_OPEN_WRITE |
                               NX_FS_OPEN_CREATE);
    ASSERT(id != 0);

    f->ops->close(self, id);
    f->destroy(self);
}

/* --- case 2: open w/o CREATE on a missing path returns failure --------- */

void nx_conformance_fs_open_without_create_on_missing_path_returns_enoent(
    const struct nx_fs_fixture *f)
{
    void *self = f->create();
    ASSERT_NOT_NULL(self);

    /* Slice 9b.1: failure is signaled by returning 0. */
    uint32_t id = f->ops->open(self, "/missing", NX_FS_OPEN_READ);
    ASSERT_EQ_U(id, 0);

    f->destroy(self);
}

/* --- case 3: a fresh (just-created) file reads 0 bytes ---------------- */

void nx_conformance_fs_fresh_file_reads_zero_bytes(
    const struct nx_fs_fixture *f)
{
    void *self = f->create();
    ASSERT_NOT_NULL(self);

    uint32_t id = open_new_rw(f, self, "/a");
    ASSERT(id != 0);

    char buf[16];
    int64_t n = f->ops->read(self, id, buf, sizeof buf);
    ASSERT_EQ_U(n, 0);

    f->ops->close(self, id);
    f->destroy(self);
}

/* --- case 4: write then reopen-read round-trips the bytes ------------- */

void nx_conformance_fs_write_then_read_after_reopen_roundtrips(
    const struct nx_fs_fixture *f)
{
    void *self = f->create();
    ASSERT_NOT_NULL(self);

    static const char payload[] = "hello, nonux";
    size_t n = sizeof payload - 1;

    uint32_t w = open_new_rw(f, self, "/greeting");
    ASSERT(w != 0);
    int64_t wrote = f->ops->write(self, w, payload, n);
    ASSERT_EQ_U((uint64_t)wrote, n);
    f->ops->close(self, w);

    uint32_t r = f->ops->open(self, "/greeting", NX_FS_OPEN_READ);
    ASSERT(r != 0);

    char buf[32];
    memset(buf, 0xAA, sizeof buf);
    int64_t got = f->ops->read(self, r, buf, sizeof buf);
    ASSERT_EQ_U((uint64_t)got, n);
    ASSERT(memcmp(buf, payload, n) == 0);

    int64_t tail = f->ops->read(self, r, buf, sizeof buf);
    ASSERT_EQ_U(tail, 0);

    f->ops->close(self, r);
    f->destroy(self);
}

/* --- case 5: read past EOF returns 0 ---------------------------------- */

void nx_conformance_fs_read_past_eof_returns_zero(
    const struct nx_fs_fixture *f)
{
    void *self = f->create();
    ASSERT_NOT_NULL(self);

    uint32_t id = open_new_rw(f, self, "/a");
    ASSERT(id != 0);

    static const char payload[] = "abc";
    int64_t wrote = f->ops->write(self, id, payload, 3);
    ASSERT_EQ_U((uint64_t)wrote, 3);
    f->ops->close(self, id);

    uint32_t r = f->ops->open(self, "/a", NX_FS_OPEN_READ);
    ASSERT(r != 0);

    char buf[8];
    ASSERT_EQ_U((uint64_t)f->ops->read(self, r, buf, sizeof buf), 3);
    ASSERT_EQ_U(f->ops->read(self, r, buf, sizeof buf), 0);
    ASSERT_EQ_U(f->ops->read(self, r, buf, sizeof buf), 0);

    f->ops->close(self, r);
    f->destroy(self);
}

/* --- case 6: two concurrent opens have independent cursors ------------ */

void nx_conformance_fs_two_opens_have_independent_cursors(
    const struct nx_fs_fixture *f)
{
    void *self = f->create();
    ASSERT_NOT_NULL(self);

    uint32_t w = open_new_rw(f, self, "/a");
    ASSERT(w != 0);
    ASSERT_EQ_U((uint64_t)f->ops->write(self, w, "0123456789", 10), 10);
    f->ops->close(self, w);

    uint32_t r1 = f->ops->open(self, "/a", NX_FS_OPEN_READ);
    uint32_t r2 = f->ops->open(self, "/a", NX_FS_OPEN_READ);
    ASSERT(r1 != 0);
    ASSERT(r2 != 0);

    char a[4];
    char b[4];
    ASSERT_EQ_U((uint64_t)f->ops->read(self, r1, a, 4), 4);
    ASSERT(memcmp(a, "0123", 4) == 0);

    ASSERT_EQ_U((uint64_t)f->ops->read(self, r2, b, 4), 4);
    ASSERT(memcmp(b, "0123", 4) == 0);

    ASSERT_EQ_U((uint64_t)f->ops->read(self, r1, a, 4), 4);
    ASSERT(memcmp(a, "4567", 4) == 0);

    f->ops->close(self, r1);
    f->ops->close(self, r2);
    f->destroy(self);
}

/* --- case 7: write on a READ-only open returns EPERM ------------------ */

void nx_conformance_fs_write_without_write_right_returns_eperm(
    const struct nx_fs_fixture *f)
{
    void *self = f->create();
    ASSERT_NOT_NULL(self);

    uint32_t w = open_new_rw(f, self, "/a");
    ASSERT(w != 0);
    ASSERT_EQ_U((uint64_t)f->ops->write(self, w, "xy", 2), 2);
    f->ops->close(self, w);

    uint32_t r = f->ops->open(self, "/a", NX_FS_OPEN_READ);
    ASSERT(r != 0);
    ASSERT_EQ_U((uint64_t)f->ops->write(self, r, "z", 1), (uint64_t)NX_EPERM);

    f->ops->close(self, r);
    f->destroy(self);
}

/* --- case 8: readdir on an empty filesystem returns ENOENT ----------- */

void nx_conformance_fs_readdir_on_empty_fs_returns_enoent(
    const struct nx_fs_fixture *f)
{
    ASSERT_NOT_NULL(f->ops->readdir);

    void *self = f->create();
    ASSERT_NOT_NULL(self);

    /* Drain "." and ".." if the driver yields them (POSIX-compliant drivers
     * like ramfs do; simpler stubs may go straight to ENOENT). */
    uint32_t cookie = 0;
    struct nx_fs_dirent ent;
    int rc = NX_OK;
    for (int n = 0; n < 4; n++) {
        rc = f->ops->readdir(self, "/", &cookie, &ent);
        if (rc != NX_OK) break;
        /* Only dot entries are valid on an otherwise-empty filesystem. */
        ASSERT(ent.name_len >= 1 && ent.name[0] == '.');
    }
    ASSERT_EQ_U(rc, NX_ENOENT);

    f->destroy(self);
}

/* --- case 9: readdir yields every created file, then ENOENT ----------- */

void nx_conformance_fs_readdir_yields_created_files_then_enoent(
    const struct nx_fs_fixture *f)
{
    void *self = f->create();
    ASSERT_NOT_NULL(self);

    static const char *const paths[] = { "/a", "/b", "/c" };
    for (int i = 0; i < 3; i++) {
        uint32_t h = open_new_rw(f, self, paths[i]);
        ASSERT(h != 0);
        f->ops->close(self, h);
    }

    int seen[3] = { 0, 0, 0 };
    uint32_t cookie = 0;
    /* +2 slots for "." and ".." that POSIX-compliant drivers prepend. */
    for (int iter = 0; iter < 12; iter++) {
        struct nx_fs_dirent ent;
        int rc = f->ops->readdir(self, "/", &cookie, &ent);
        if (rc == NX_ENOENT) break;
        ASSERT_EQ_U(rc, NX_OK);

        /* Skip "." and ".." — POSIX-compliant drivers yield them first. */
        if (ent.name_len >= 1 && ent.name[0] == '.') continue;

        int matched = 0;
        for (int i = 0; i < 3; i++) {
            if (ent.name_len == 1 && ent.name[0] == paths[i][1]) {
                ASSERT(seen[i] == 0);
                seen[i] = 1;
                matched = 1;
                break;
            }
        }
        ASSERT(matched);
    }
    for (int i = 0; i < 3; i++) ASSERT(seen[i]);

    struct nx_fs_dirent tail;
    ASSERT_EQ_U(f->ops->readdir(self, "/", &cookie, &tail), NX_ENOENT);

    f->destroy(self);
}

/* --- case 13 (slice 7.7b.1): mkdir creates dir visible in readdir ---- */

void nx_conformance_fs_mkdir_creates_dir_visible_in_readdir(
    const struct nx_fs_fixture *f)
{
    ASSERT_NOT_NULL(f->ops->mkdir);
    ASSERT_NOT_NULL(f->ops->readdir);

    void *self = f->create();
    ASSERT_NOT_NULL(self);

    ASSERT_EQ_U(f->ops->mkdir(self, "/d"), NX_OK);
    ASSERT_EQ_U(f->ops->mkdir(self, "/d"), NX_EEXIST);

    int saw = 0;
    uint32_t cookie = 0;
    for (int iter = 0; iter < 10; iter++) {
        struct nx_fs_dirent ent;
        int rc = f->ops->readdir(self, "/", &cookie, &ent);
        if (rc == NX_ENOENT) break;
        ASSERT_EQ_U(rc, NX_OK);
        if (ent.name_len == 1 && ent.name[0] == 'd') {
            ASSERT(saw == 0);
            saw = 1;
        }
    }
    ASSERT(saw == 1);

    f->destroy(self);
}

/* --- case 14 (slice 7.7b.1): stat reports kind for files and dirs ---- */

void nx_conformance_fs_stat_reports_kind_for_files_and_dirs(
    const struct nx_fs_fixture *f)
{
    ASSERT_NOT_NULL(f->ops->stat);
    ASSERT_NOT_NULL(f->ops->mkdir);

    void *self = f->create();
    ASSERT_NOT_NULL(self);

    struct nx_fs_stat st;
    ASSERT_EQ_U(f->ops->stat(self, "/", &st), NX_OK);
    ASSERT_EQ_U(st.kind, NX_FS_KIND_DIR);

    ASSERT_EQ_U(f->ops->stat(self, "/nope", &st), NX_ENOENT);

    uint32_t h = open_new_rw(f, self, "/file");
    ASSERT(h != 0);
    ASSERT_EQ_U((uint64_t)f->ops->write(self, h, "hi", 2), 2);
    f->ops->close(self, h);
    ASSERT_EQ_U(f->ops->stat(self, "/file", &st), NX_OK);
    ASSERT_EQ_U(st.kind, NX_FS_KIND_FILE);
    ASSERT_EQ_U((uint64_t)st.size, 2);

    ASSERT_EQ_U(f->ops->mkdir(self, "/dir"), NX_OK);
    ASSERT_EQ_U(f->ops->stat(self, "/dir", &st), NX_OK);
    ASSERT_EQ_U(st.kind, NX_FS_KIND_DIR);

    f->destroy(self);
}

/* --- case 10: seek_set to zero restarts reads ------------------------ */

void nx_conformance_fs_seek_set_to_zero_restarts_reads(
    const struct nx_fs_fixture *f)
{
    ASSERT_NOT_NULL(f->ops->seek);

    void *self = f->create();
    ASSERT_NOT_NULL(self);

    uint32_t h = open_new_rw(f, self, "/a");
    ASSERT(h != 0);
    ASSERT_EQ_U((uint64_t)f->ops->write(self, h, "hello", 5), 5);
    char tail[4];
    ASSERT_EQ_U(f->ops->read(self, h, tail, sizeof tail), 0);

    int64_t pos = f->ops->seek(self, h, 0, NX_FS_SEEK_SET);
    ASSERT_EQ_U((uint64_t)pos, 0);

    char buf[8] = {0};
    int64_t got = f->ops->read(self, h, buf, sizeof buf);
    ASSERT_EQ_U((uint64_t)got, 5);
    ASSERT(buf[0] == 'h' && buf[1] == 'e' && buf[2] == 'l' &&
           buf[3] == 'l' && buf[4] == 'o');

    f->ops->close(self, h);
    f->destroy(self);
}

/* --- case 11: seek_end with offset 0 returns file size --------------- */

void nx_conformance_fs_seek_end_returns_file_size(
    const struct nx_fs_fixture *f)
{
    void *self = f->create();
    ASSERT_NOT_NULL(self);

    uint32_t h = open_new_rw(f, self, "/a");
    ASSERT(h != 0);
    ASSERT_EQ_U((uint64_t)f->ops->write(self, h, "abcdefghij", 10), 10);

    int64_t pos = f->ops->seek(self, h, 0, NX_FS_SEEK_END);
    ASSERT_EQ_U((uint64_t)pos, 10);

    char buf[4];
    ASSERT_EQ_U(f->ops->read(self, h, buf, sizeof buf), 0);

    f->ops->close(self, h);
    f->destroy(self);
}

/* --- case 12: seek past size returns EINVAL -------------------------- */

void nx_conformance_fs_seek_past_size_returns_einval(
    const struct nx_fs_fixture *f)
{
    void *self = f->create();
    ASSERT_NOT_NULL(self);

    uint32_t h = open_new_rw(f, self, "/a");
    ASSERT(h != 0);
    ASSERT_EQ_U((uint64_t)f->ops->write(self, h, "abc", 3), 3);

    int64_t rc = f->ops->seek(self, h, 100, NX_FS_SEEK_SET);
    ASSERT_EQ_U((uint64_t)rc, (uint64_t)NX_EINVAL);

    int64_t pos = f->ops->seek(self, h, 0, NX_FS_SEEK_CUR);
    ASSERT_EQ_U((uint64_t)pos, 3);

    rc = f->ops->seek(self, h, 1, NX_FS_SEEK_END);
    ASSERT_EQ_U((uint64_t)rc, (uint64_t)NX_EINVAL);

    rc = f->ops->seek(self, h, 0, 99);
    ASSERT_EQ_U((uint64_t)rc, (uint64_t)NX_EINVAL);

    f->ops->close(self, h);
    f->destroy(self);
}
