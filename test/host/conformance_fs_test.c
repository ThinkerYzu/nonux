/*
 * Host test that exercises the fs-driver conformance harness (slice 6.1).
 *
 * Two purposes:
 *   1. Smoke-test the harness itself by running every universal case
 *      against a trivially-correct in-file `fs_stub` fixture.  If the
 *      harness is buggy it fails here rather than later against the
 *      first real driver (ramfs in slice 6.2).
 *   2. Serve as a worked example — `components/ramfs/` will wrap the
 *      same harness helpers one-TEST-per-case in slice 6.2.
 *
 * `fs_stub` is the world's dumbest correct fs driver: a fixed table of
 * named byte buffers with a per-open cursor.  Not wired into any
 * component and not linked into the kernel build — pure test-only code.
 */

#include "test_runner.h"
#include "conformance/conformance_fs.h"

#include "interfaces/fs.h"
#include "framework/registry.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---------- fs_stub: in-file fixture driver --------------------------- */

#define STUB_MAX_FILES  8
#define STUB_NAME_MAX   32
#define STUB_FILE_CAP   256

struct stub_file {
    int      in_use;
    char     name[STUB_NAME_MAX];
    uint8_t  data[STUB_FILE_CAP];
    size_t   size;
};

struct stub_open {
    struct stub_file *file;
    uint32_t          flags;
    size_t            cursor;
};

struct fs_stub {
    struct stub_file files[STUB_MAX_FILES];
};

static struct stub_file *stub_find(struct fs_stub *s, const char *path)
{
    for (int i = 0; i < STUB_MAX_FILES; i++)
        if (s->files[i].in_use &&
            strncmp(s->files[i].name, path, STUB_NAME_MAX - 1) == 0)
            return &s->files[i];
    return NULL;
}

static struct stub_file *stub_create(struct fs_stub *s, const char *path)
{
    size_t plen = strlen(path);
    if (plen == 0 || plen >= STUB_NAME_MAX) return NULL;
    for (int i = 0; i < STUB_MAX_FILES; i++) {
        if (!s->files[i].in_use) {
            struct stub_file *f = &s->files[i];
            f->in_use = 1;
            memcpy(f->name, path, plen + 1);  /* include NUL */
            f->size = 0;
            return f;
        }
    }
    return NULL;
}

static int stub_open_op(void *self, const char *path, uint32_t flags,
                        void **out_file)
{
    const uint32_t known = NX_FS_OPEN_READ | NX_FS_OPEN_WRITE |
                           NX_FS_OPEN_CREATE;
    if (!self || !path || !out_file) return NX_EINVAL;
    if (path[0] == '\0') return NX_EINVAL;
    if (flags & ~known) return NX_EINVAL;

    struct fs_stub *s = self;
    struct stub_file *file = stub_find(s, path);
    if (!file) {
        if (!(flags & NX_FS_OPEN_CREATE)) return NX_ENOENT;
        file = stub_create(s, path);
        if (!file) return NX_ENOMEM;
    }

    struct stub_open *op = calloc(1, sizeof *op);
    if (!op) return NX_ENOMEM;
    op->file   = file;
    op->flags  = flags;
    op->cursor = 0;
    *out_file = op;
    return NX_OK;
}

static void stub_close_op(void *self, void *file)
{
    (void)self;
    if (!file) return;
    free(file);
}

static int64_t stub_read_op(void *self, void *file, void *buf, size_t cap)
{
    (void)self;
    if (!file) return NX_EINVAL;
    if (cap == 0) return 0;
    if (!buf) return NX_EINVAL;

    struct stub_open *op = file;
    if (!(op->flags & NX_FS_OPEN_READ)) return NX_EPERM;

    size_t remain = (op->cursor < op->file->size)
                    ? op->file->size - op->cursor : 0;
    size_t n = remain < cap ? remain : cap;
    if (n > 0) memcpy(buf, op->file->data + op->cursor, n);
    op->cursor += n;
    return (int64_t)n;
}

static int64_t stub_write_op(void *self, void *file, const void *buf, size_t len)
{
    (void)self;
    if (!file) return NX_EINVAL;
    if (len == 0) return 0;
    if (!buf) return NX_EINVAL;

    struct stub_open *op = file;
    if (!(op->flags & NX_FS_OPEN_WRITE)) return NX_EPERM;

    size_t room = (op->cursor < STUB_FILE_CAP)
                  ? STUB_FILE_CAP - op->cursor : 0;
    if (room == 0) return NX_ENOMEM;
    size_t n = len < room ? len : room;
    memcpy(op->file->data + op->cursor, buf, n);
    op->cursor += n;
    if (op->cursor > op->file->size) op->file->size = op->cursor;
    return (int64_t)n;
}

static int64_t stub_seek_op(void *self, void *file, int64_t offset, int whence)
{
    (void)self;
    if (!file) return NX_EINVAL;
    struct stub_open *op = file;

    int64_t new_pos;
    switch (whence) {
    case NX_FS_SEEK_SET: new_pos = offset;                       break;
    case NX_FS_SEEK_CUR: new_pos = (int64_t)op->cursor + offset; break;
    case NX_FS_SEEK_END: new_pos = (int64_t)op->file->size + offset; break;
    default:             return NX_EINVAL;
    }
    if (new_pos < 0 || (uint64_t)new_pos > op->file->size) return NX_EINVAL;
    op->cursor = (size_t)new_pos;
    return new_pos;
}

static int stub_readdir_op(void *self, uint32_t *cookie,
                           struct nx_fs_dirent *out)
{
    if (!self || !cookie || !out) return NX_EINVAL;
    struct fs_stub *s = self;
    for (unsigned i = *cookie; i < STUB_MAX_FILES; i++) {
        if (s->files[i].in_use) {
            size_t nlen = 0;
            while (s->files[i].name[nlen] != '\0' &&
                   nlen < NX_FS_DIRENT_NAME_MAX - 1) nlen++;
            out->name_len = (uint32_t)nlen;
            memcpy(out->name, s->files[i].name, nlen);
            out->name[nlen] = '\0';
            *cookie = i + 1;
            return NX_OK;
        }
    }
    *cookie = STUB_MAX_FILES;
    return NX_ENOENT;
}

static const struct nx_fs_ops fs_stub_ops = {
    .open    = stub_open_op,
    .close   = stub_close_op,
    .read    = stub_read_op,
    .write   = stub_write_op,
    .seek    = stub_seek_op,
    .readdir = stub_readdir_op,
};

static void *fs_stub_create(void)
{
    return calloc(1, sizeof(struct fs_stub));
}

static void fs_stub_destroy(void *self)
{
    free(self);
}

static const struct nx_fs_fixture fs_stub_fixture = {
    .ops     = &fs_stub_ops,
    .create  = fs_stub_create,
    .destroy = fs_stub_destroy,
};

/* ---------- conformance TEST wrappers -------------------------------- */

TEST(fs_stub_conformance_open_create_on_fresh_path_succeeds)
{
    nx_conformance_fs_open_create_on_fresh_path_succeeds(&fs_stub_fixture);
}

TEST(fs_stub_conformance_open_without_create_on_missing_path_returns_enoent)
{
    nx_conformance_fs_open_without_create_on_missing_path_returns_enoent(&fs_stub_fixture);
}

TEST(fs_stub_conformance_fresh_file_reads_zero_bytes)
{
    nx_conformance_fs_fresh_file_reads_zero_bytes(&fs_stub_fixture);
}

TEST(fs_stub_conformance_write_then_read_after_reopen_roundtrips)
{
    nx_conformance_fs_write_then_read_after_reopen_roundtrips(&fs_stub_fixture);
}

TEST(fs_stub_conformance_read_past_eof_returns_zero)
{
    nx_conformance_fs_read_past_eof_returns_zero(&fs_stub_fixture);
}

TEST(fs_stub_conformance_two_opens_have_independent_cursors)
{
    nx_conformance_fs_two_opens_have_independent_cursors(&fs_stub_fixture);
}

TEST(fs_stub_conformance_write_without_write_right_returns_eperm)
{
    nx_conformance_fs_write_without_write_right_returns_eperm(&fs_stub_fixture);
}

TEST(fs_stub_conformance_readdir_on_empty_fs_returns_enoent)
{
    nx_conformance_fs_readdir_on_empty_fs_returns_enoent(&fs_stub_fixture);
}

TEST(fs_stub_conformance_readdir_yields_created_files_then_enoent)
{
    nx_conformance_fs_readdir_yields_created_files_then_enoent(&fs_stub_fixture);
}

TEST(fs_stub_conformance_seek_set_to_zero_restarts_reads)
{
    nx_conformance_fs_seek_set_to_zero_restarts_reads(&fs_stub_fixture);
}

TEST(fs_stub_conformance_seek_end_returns_file_size)
{
    nx_conformance_fs_seek_end_returns_file_size(&fs_stub_fixture);
}

TEST(fs_stub_conformance_seek_past_size_returns_einval)
{
    nx_conformance_fs_seek_past_size_returns_einval(&fs_stub_fixture);
}

/* ---------- local fs_stub unit tests (fixture sanity checks) --------- */

TEST(fs_stub_internal_unknown_flag_bit_rejected)
{
    void *self = fs_stub_create();
    ASSERT_NOT_NULL(self);
    void *f = NULL;
    /* Bit 31 is outside the known READ/WRITE/CREATE set. */
    ASSERT_EQ_U(fs_stub_ops.open(self, "/a", (1U << 31), &f), NX_EINVAL);
    fs_stub_destroy(self);
}

TEST(fs_stub_internal_write_fills_capacity_then_enomem)
{
    void *self = fs_stub_create();
    ASSERT_NOT_NULL(self);

    void *f = NULL;
    ASSERT_EQ_U(fs_stub_ops.open(self, "/big",
                                 NX_FS_OPEN_WRITE | NX_FS_OPEN_CREATE, &f),
                NX_OK);

    /* Fill to capacity. */
    static uint8_t junk[STUB_FILE_CAP];
    memset(junk, 'x', sizeof junk);
    ASSERT_EQ_U((uint64_t)fs_stub_ops.write(self, f, junk, sizeof junk),
                (uint64_t)STUB_FILE_CAP);

    /* Next byte has nowhere to go → NX_ENOMEM. */
    ASSERT_EQ_U((uint64_t)fs_stub_ops.write(self, f, "y", 1),
                (uint64_t)NX_ENOMEM);

    fs_stub_ops.close(self, f);
    fs_stub_destroy(self);
}
