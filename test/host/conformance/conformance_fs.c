/*
 * Filesystem-driver conformance suite — implementation (slice 6.1).
 *
 * See conformance_fs.h for the usage contract.  Each helper exercises
 * one invariant of `struct nx_fs_ops`; callers wrap them in TEST()s.
 */

#include "conformance_fs.h"

#include "framework/registry.h"
#include "test/host/test_runner.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---------- helpers --------------------------------------------------- */

static void *open_new_rw(const struct nx_fs_fixture *f, void *self,
                         const char *path)
{
    void *file = NULL;
    int rc = f->ops->open(self, path,
                          NX_FS_OPEN_READ | NX_FS_OPEN_WRITE |
                          NX_FS_OPEN_CREATE,
                          &file);
    /* Caller asserts on the result; we return file (or NULL on rc!=OK). */
    if (rc != NX_OK) return NULL;
    return file;
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

    void *file = NULL;
    int rc = f->ops->open(self, "/a",
                          NX_FS_OPEN_READ | NX_FS_OPEN_WRITE |
                          NX_FS_OPEN_CREATE,
                          &file);
    ASSERT_EQ_U(rc, NX_OK);
    ASSERT_NOT_NULL(file);

    f->ops->close(self, file);
    f->destroy(self);
}

/* --- case 2: open w/o CREATE on a missing path returns ENOENT --------- */

void nx_conformance_fs_open_without_create_on_missing_path_returns_enoent(
    const struct nx_fs_fixture *f)
{
    void *self = f->create();
    ASSERT_NOT_NULL(self);

    void *file = (void *)0xdeadbeef;  /* sentinel — must not be touched */
    int rc = f->ops->open(self, "/missing",
                          NX_FS_OPEN_READ, &file);
    ASSERT_EQ_U(rc, NX_ENOENT);
    /* Drivers are not required to clobber out_file on failure, so we
     * don't assert its value — only that the status is correct. */
    (void)file;

    f->destroy(self);
}

/* --- case 3: a fresh (just-created) file reads 0 bytes ---------------- */

void nx_conformance_fs_fresh_file_reads_zero_bytes(
    const struct nx_fs_fixture *f)
{
    void *self = f->create();
    ASSERT_NOT_NULL(self);

    void *file = open_new_rw(f, self, "/a");
    ASSERT_NOT_NULL(file);

    char buf[16];
    int64_t n = f->ops->read(self, file, buf, sizeof buf);
    ASSERT_EQ_U(n, 0);

    f->ops->close(self, file);
    f->destroy(self);
}

/* --- case 4: write then reopen-read round-trips the bytes ------------- */

void nx_conformance_fs_write_then_read_after_reopen_roundtrips(
    const struct nx_fs_fixture *f)
{
    void *self = f->create();
    ASSERT_NOT_NULL(self);

    static const char payload[] = "hello, nonux";
    size_t n = sizeof payload - 1;  /* drop the NUL */

    void *w = open_new_rw(f, self, "/greeting");
    ASSERT_NOT_NULL(w);
    int64_t wrote = f->ops->write(self, w, payload, n);
    ASSERT_EQ_U((uint64_t)wrote, n);
    f->ops->close(self, w);

    /* Reopen READ-only so we also prove an existing file doesn't need
     * CREATE.  A driver that stripes bytes on close has to have the
     * write visible here. */
    void *r = NULL;
    int rc = f->ops->open(self, "/greeting", NX_FS_OPEN_READ, &r);
    ASSERT_EQ_U(rc, NX_OK);
    ASSERT_NOT_NULL(r);

    char buf[32];
    memset(buf, 0xAA, sizeof buf);
    int64_t got = f->ops->read(self, r, buf, sizeof buf);
    ASSERT_EQ_U((uint64_t)got, n);
    ASSERT(memcmp(buf, payload, n) == 0);

    /* One more read should now see EOF. */
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

    void *file = open_new_rw(f, self, "/a");
    ASSERT_NOT_NULL(file);

    static const char payload[] = "abc";
    int64_t wrote = f->ops->write(self, file, payload, 3);
    ASSERT_EQ_U((uint64_t)wrote, 3);

    f->ops->close(self, file);

    void *r = NULL;
    ASSERT_EQ_U(f->ops->open(self, "/a", NX_FS_OPEN_READ, &r), NX_OK);

    char buf[8];
    ASSERT_EQ_U((uint64_t)f->ops->read(self, r, buf, sizeof buf), 3);
    /* cursor now at EOF — every subsequent read is 0. */
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

    /* Seed the file. */
    void *w = open_new_rw(f, self, "/a");
    ASSERT_NOT_NULL(w);
    ASSERT_EQ_U((uint64_t)f->ops->write(self, w, "0123456789", 10), 10);
    f->ops->close(self, w);

    void *r1 = NULL;
    void *r2 = NULL;
    ASSERT_EQ_U(f->ops->open(self, "/a", NX_FS_OPEN_READ, &r1), NX_OK);
    ASSERT_EQ_U(f->ops->open(self, "/a", NX_FS_OPEN_READ, &r2), NX_OK);

    /* Advance r1 by 4.  r2's cursor must not move — the next read on
     * r2 still starts at byte 0. */
    char a[4];
    char b[4];
    ASSERT_EQ_U((uint64_t)f->ops->read(self, r1, a, 4), 4);
    ASSERT(memcmp(a, "0123", 4) == 0);

    ASSERT_EQ_U((uint64_t)f->ops->read(self, r2, b, 4), 4);
    ASSERT(memcmp(b, "0123", 4) == 0);

    /* And r1's next read resumes from 4, not 0. */
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

    /* Seed an existing file so the READ-only open isn't ambiguous with
     * the ENOENT path. */
    void *w = open_new_rw(f, self, "/a");
    ASSERT_NOT_NULL(w);
    ASSERT_EQ_U((uint64_t)f->ops->write(self, w, "xy", 2), 2);
    f->ops->close(self, w);

    void *r = NULL;
    ASSERT_EQ_U(f->ops->open(self, "/a", NX_FS_OPEN_READ, &r), NX_OK);
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

    uint32_t cookie = 0;
    struct nx_fs_dirent ent;
    int rc = f->ops->readdir(self, &cookie, &ent);
    ASSERT_EQ_U(rc, NX_ENOENT);

    f->destroy(self);
}

/* --- case 9: readdir yields every created file, then ENOENT ----------- */

void nx_conformance_fs_readdir_yields_created_files_then_enoent(
    const struct nx_fs_fixture *f)
{
    void *self = f->create();
    ASSERT_NOT_NULL(self);

    /* Create three files with distinct names. */
    static const char *const paths[] = { "/a", "/b", "/c" };
    for (int i = 0; i < 3; i++) {
        void *h = open_new_rw(f, self, paths[i]);
        ASSERT_NOT_NULL(h);
        f->ops->close(self, h);
    }

    /* Iterate with a fresh cookie.  Collect yielded names into `seen[]`
     * — order is driver-defined so we don't assert a specific sequence,
     * only that all three appear and nothing extra does. */
    int seen[3] = { 0, 0, 0 };
    uint32_t cookie = 0;
    for (int iter = 0; iter < 10; iter++) {  /* loose upper bound */
        struct nx_fs_dirent ent;
        int rc = f->ops->readdir(self, &cookie, &ent);
        if (rc == NX_ENOENT) break;
        ASSERT_EQ_U(rc, NX_OK);

        int matched = 0;
        for (int i = 0; i < 3; i++) {
            if (ent.name_len == 2 && ent.name[0] == '/' &&
                ent.name[1] == paths[i][1]) {
                ASSERT(seen[i] == 0);   /* each name once */
                seen[i] = 1;
                matched = 1;
                break;
            }
        }
        ASSERT(matched);                /* no surprise names */
    }
    /* All three names seen; post-ENOENT cookie is sticky (repeated
     * calls keep returning ENOENT). */
    for (int i = 0; i < 3; i++) ASSERT(seen[i]);

    struct nx_fs_dirent tail;
    ASSERT_EQ_U(f->ops->readdir(self, &cookie, &tail), NX_ENOENT);

    f->destroy(self);
}

/* --- case 10: seek_set to zero restarts reads ------------------------ */

void nx_conformance_fs_seek_set_to_zero_restarts_reads(
    const struct nx_fs_fixture *f)
{
    ASSERT_NOT_NULL(f->ops->seek);

    void *self = f->create();
    ASSERT_NOT_NULL(self);

    void *h = open_new_rw(f, self, "/a");
    ASSERT_NOT_NULL(h);
    ASSERT_EQ_U((uint64_t)f->ops->write(self, h, "hello", 5), 5);
    /* After the write, the cursor is at EOF (5) — a read here returns 0. */
    char tail[4];
    ASSERT_EQ_U(f->ops->read(self, h, tail, sizeof tail), 0);

    /* Seek back to the start; a read now sees the original bytes. */
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

    void *h = open_new_rw(f, self, "/a");
    ASSERT_NOT_NULL(h);
    ASSERT_EQ_U((uint64_t)f->ops->write(self, h, "abcdefghij", 10), 10);

    /* SEEK_END with offset 0 reports file size without extending. */
    int64_t pos = f->ops->seek(self, h, 0, NX_FS_SEEK_END);
    ASSERT_EQ_U((uint64_t)pos, 10);

    /* Cursor is at EOF — reads return 0. */
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

    void *h = open_new_rw(f, self, "/a");
    ASSERT_NOT_NULL(h);
    ASSERT_EQ_U((uint64_t)f->ops->write(self, h, "abc", 3), 3);

    /* SEEK_SET past size → NX_EINVAL.  Cursor stays at previous
     * position (EOF after the write), verified by a subsequent
     * SEEK_CUR which must report 3. */
    int64_t rc = f->ops->seek(self, h, 100, NX_FS_SEEK_SET);
    ASSERT_EQ_U((uint64_t)rc, (uint64_t)NX_EINVAL);

    int64_t pos = f->ops->seek(self, h, 0, NX_FS_SEEK_CUR);
    ASSERT_EQ_U((uint64_t)pos, 3);

    /* SEEK_END with positive offset also rejected. */
    rc = f->ops->seek(self, h, 1, NX_FS_SEEK_END);
    ASSERT_EQ_U((uint64_t)rc, (uint64_t)NX_EINVAL);

    /* Unknown whence likewise. */
    rc = f->ops->seek(self, h, 0, 99);
    ASSERT_EQ_U((uint64_t)rc, (uint64_t)NX_EINVAL);

    f->ops->close(self, h);
    f->destroy(self);
}
