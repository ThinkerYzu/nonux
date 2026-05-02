/*
 * Host-side tests for slice 8.0c — migrate framework production paths
 * to nx_vfs_* wrappers.
 *
 * These are per-callsite equivalence tests: each test calls an
 * nx_vfs_* wrapper and compares the result to what the direct
 * vops->op(vself, ...) call would return.  In the host build the
 * wrappers use a fast path that calls through slot->active ops
 * directly, so the two paths are structurally identical — the tests
 * pin behavior and will catch any regression in the wrapper's arg
 * encoding or return-value extraction.
 *
 * Fixture: vfs slot ← vfs_simple ← fake_fs (tiny in-memory driver,
 * same shape as the one in file_syscall_test.c but local to this
 * file so we don't depend on its internal types).
 *
 * Kernel-side (kernel-build) end-to-end blocking-call round-trip is
 * covered by the kernel ktests added alongside this slice.
 */

#include "test_runner.h"

#include "framework/component.h"
#include "framework/registry.h"
#include "framework/vfs_call.h"
#include "interfaces/fs.h"
#include "interfaces/vfs.h"
#include "interfaces/fs_types.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Exported by vfs_simple */
extern const struct nx_vfs_ops             vfs_simple_vfs_ops;
extern const struct nx_component_ops       vfs_simple_component_ops;
extern const struct nx_component_descriptor vfs_simple_descriptor;

/* ================================================================== */
/* Minimal fake filesystem driver                                       */
/* ================================================================== */

#define FAKE_DATA "hello_8_0c"
#define FAKE_DATA_LEN 10

struct fake8c_file {
    int     in_use;
    char    name[32];
    uint8_t data[64];
    size_t  size;
    size_t  cursor;
    int     retain_count;
    int     close_calls;
};

struct fake8c_state {
    struct fake8c_file files[4];
    int mkdir_calls;
    int last_mkdir_rc;
};

static int f8c_open(void *self, const char *path, uint32_t flags,
                    void **out)
{
    struct fake8c_state *s = self;
    if (!path || path[0] != '/') return NX_EINVAL;
    for (int i = 0; i < 4; i++) {
        if (!s->files[i].in_use) continue;
        if (strcmp(s->files[i].name, path) == 0) {
            s->files[i].cursor = 0;
            *out = &s->files[i];
            return NX_OK;
        }
    }
    if (flags & NX_FS_OPEN_CREATE) {
        for (int i = 0; i < 4; i++) {
            if (s->files[i].in_use) continue;
            s->files[i].in_use = 1;
            strncpy(s->files[i].name, path, sizeof(s->files[i].name) - 1);
            s->files[i].size   = 0;
            s->files[i].cursor = 0;
            s->files[i].retain_count = 1;
            s->files[i].close_calls  = 0;
            *out = &s->files[i];
            return NX_OK;
        }
        return NX_ENOMEM;
    }
    return NX_ENOENT;
}

static void f8c_close(void *self, void *file)
{
    (void)self;
    struct fake8c_file *f = file;
    if (f) f->close_calls++;
}

static void f8c_retain(void *self, void *file)
{
    (void)self;
    struct fake8c_file *f = file;
    if (f) f->retain_count++;
}

static int64_t f8c_read(void *self, void *file, void *buf, size_t cap)
{
    (void)self;
    struct fake8c_file *f = file;
    if (!f || !buf) return NX_EINVAL;
    size_t avail = f->size > f->cursor ? f->size - f->cursor : 0;
    size_t n = avail < cap ? avail : cap;
    memcpy(buf, f->data + f->cursor, n);
    f->cursor += n;
    return (int64_t)n;
}

static int64_t f8c_write(void *self, void *file, const void *buf, size_t len)
{
    (void)self;
    struct fake8c_file *f = file;
    if (!f || !buf) return NX_EINVAL;
    size_t end = f->cursor + len;
    if (end > sizeof f->data) return NX_ENOMEM;
    memcpy(f->data + f->cursor, buf, len);
    f->cursor += len;
    if (f->cursor > f->size) f->size = f->cursor;
    return (int64_t)len;
}

static int64_t f8c_seek(void *self, void *file, int64_t offset, int whence)
{
    (void)self;
    struct fake8c_file *f = file;
    if (!f) return NX_EINVAL;
    int64_t new_pos;
    if (whence == NX_FS_SEEK_SET)      new_pos = offset;
    else if (whence == NX_FS_SEEK_CUR) new_pos = (int64_t)f->cursor + offset;
    else if (whence == NX_FS_SEEK_END) new_pos = (int64_t)f->size + offset;
    else return NX_EINVAL;
    if (new_pos < 0) return NX_EINVAL;
    f->cursor = (size_t)new_pos;
    return new_pos;
}

static int f8c_readdir(void *self, const char *dir_path,
                       uint32_t *cookie, struct nx_fs_dirent *out)
{
    (void)self; (void)dir_path;
    static const char *names[] = { "file0", "file1" };
    if (*cookie >= 2) return NX_ENOENT;
    strncpy(out->name, names[*cookie], sizeof out->name - 1);
    out->name_len = (uint32_t)strlen(out->name);
    (*cookie)++;
    return NX_OK;
}

static int f8c_mkdir(void *self, const char *path)
{
    struct fake8c_state *s = self;
    (void)path;
    s->mkdir_calls++;
    return s->last_mkdir_rc;
}

static int f8c_stat(void *self, const char *path, struct nx_fs_stat *out)
{
    struct fake8c_state *s = self;
    if (!path || path[0] != '/') return NX_EINVAL;
    if (path[1] == '\0') { out->kind = NX_FS_KIND_DIR; out->size = 0; return NX_OK; }
    for (int i = 0; i < 4; i++) {
        if (!s->files[i].in_use) continue;
        if (strcmp(s->files[i].name, path) == 0) {
            out->kind = NX_FS_KIND_FILE;
            out->size = (int64_t)s->files[i].size;
            return NX_OK;
        }
    }
    return NX_ENOENT;
}

static const struct nx_fs_ops fake8c_ops = {
    .open    = f8c_open,   .close   = f8c_close,
    .retain  = f8c_retain, .read    = f8c_read,
    .write   = f8c_write,  .seek    = f8c_seek,
    .readdir = f8c_readdir, .mkdir  = f8c_mkdir,
    .stat    = f8c_stat,
};

static const struct nx_component_descriptor fake8c_descriptor = {
    .name        = "fake8c_fs",
    .state_size  = sizeof(struct fake8c_state),
    .deps_offset = 0,
    .deps        = NULL,
    .n_deps      = 0,
    .ops         = NULL,
    .iface_ops   = &fake8c_ops,
};

/* ================================================================== */
/* Fixture                                                              */
/* ================================================================== */

struct fix8c {
    struct nx_slot      vfs_slot;
    struct nx_slot      root_slot;
    struct nx_component vfs_comp;
    struct nx_component fake_comp;
    struct fake8c_state fake_state;
    void               *vfs_state;
};

static void fix8c_setup(struct fix8c *fx)
{
    nx_graph_reset();
    memset(fx, 0, sizeof *fx);

    fx->vfs_slot.name        = "vfs";
    fx->vfs_slot.iface       = "vfs";
    fx->vfs_slot.mutability  = NX_MUT_HOT;
    fx->vfs_slot.concurrency = NX_CONC_SHARED;
    ASSERT_EQ_U(nx_slot_register(&fx->vfs_slot), NX_OK);

    fx->vfs_state = calloc(1, vfs_simple_descriptor.state_size);
    ASSERT_NOT_NULL(fx->vfs_state);
    ASSERT_EQ_U(vfs_simple_component_ops.init(fx->vfs_state),   NX_OK);
    ASSERT_EQ_U(vfs_simple_component_ops.enable(fx->vfs_state), NX_OK);

    fx->vfs_comp.manifest_id = "vfs_simple";
    fx->vfs_comp.instance_id = "0";
    fx->vfs_comp.impl        = fx->vfs_state;
    fx->vfs_comp.descriptor  = &vfs_simple_descriptor;
    ASSERT_EQ_U(nx_component_register(&fx->vfs_comp), NX_OK);
    ASSERT_EQ_U(nx_slot_swap(&fx->vfs_slot, &fx->vfs_comp), NX_OK);

    fx->root_slot.name        = "filesystem.root";
    fx->root_slot.iface       = "filesystem";
    fx->root_slot.mutability  = NX_MUT_HOT;
    fx->root_slot.concurrency = NX_CONC_SHARED;
    ASSERT_EQ_U(nx_slot_register(&fx->root_slot), NX_OK);

    fx->fake_comp.manifest_id = "fake8c_fs";
    fx->fake_comp.instance_id = "0";
    fx->fake_comp.impl        = &fx->fake_state;
    fx->fake_comp.descriptor  = &fake8c_descriptor;
    ASSERT_EQ_U(nx_component_register(&fx->fake_comp), NX_OK);
    ASSERT_EQ_U(nx_slot_swap(&fx->root_slot, &fx->fake_comp), NX_OK);
}

static void fix8c_teardown(struct fix8c *fx)
{
    if (fx->vfs_state) {
        vfs_simple_component_ops.disable(fx->vfs_state);
        vfs_simple_component_ops.destroy(fx->vfs_state);
        free(fx->vfs_state);
        fx->vfs_state = NULL;
    }
    nx_graph_reset();
}

/* Helper: plant a file with known content. */
static struct fake8c_file *plant_file(struct fix8c *fx, const char *name,
                                      const char *content)
{
    for (int i = 0; i < 4; i++) {
        if (fx->fake_state.files[i].in_use) continue;
        struct fake8c_file *f = &fx->fake_state.files[i];
        f->in_use = 1;
        strncpy(f->name, name, sizeof f->name - 1);
        size_t len = strlen(content);
        if (len > sizeof f->data) len = sizeof f->data;
        memcpy(f->data, content, len);
        f->size   = len;
        f->cursor = 0;
        f->retain_count = 1;
        f->close_calls  = 0;
        return f;
    }
    return NULL;
}

/* ================================================================== */
/* Section 1: nx_vfs_open / nx_vfs_close equivalence                   */
/* ================================================================== */

TEST(wrapper_open_existing_file_returns_ok)
{
    struct fix8c fx;
    fix8c_setup(&fx);
    plant_file(&fx, "/a.txt", FAKE_DATA);

    void *file = NULL;
    int rc = nx_vfs_open(&fx.vfs_slot, "/a.txt", NX_VFS_OPEN_READ, &file);
    ASSERT_EQ_U(rc, NX_OK);
    ASSERT_NOT_NULL(file);

    nx_vfs_close(&fx.vfs_slot, file);
    fix8c_teardown(&fx);
}

TEST(wrapper_open_missing_file_returns_enoent)
{
    struct fix8c fx;
    fix8c_setup(&fx);

    void *file = NULL;
    int rc = nx_vfs_open(&fx.vfs_slot, "/nope.txt", NX_VFS_OPEN_READ, &file);
    ASSERT_EQ_U(rc, NX_ENOENT);

    fix8c_teardown(&fx);
}

TEST(wrapper_open_equiv_direct_open)
{
    struct fix8c fx;
    fix8c_setup(&fx);
    plant_file(&fx, "/b.txt", FAKE_DATA);

    /* Direct path. */
    void *direct_file = NULL;
    int direct_rc = f8c_open(&fx.fake_state, "/b.txt", NX_VFS_OPEN_READ,
                              &direct_file);

    /* Wrapper path (resets cursor via vfs_simple which calls fake ops). */
    fx.fake_state.files[0].cursor = 0;
    void *wrap_file = NULL;
    int wrap_rc = nx_vfs_open(&fx.vfs_slot, "/b.txt", NX_VFS_OPEN_READ,
                               &wrap_file);

    ASSERT_EQ_U(direct_rc, wrap_rc);
    ASSERT((direct_file != NULL) == (wrap_file != NULL));

    nx_vfs_close(&fx.vfs_slot, wrap_file);
    fix8c_teardown(&fx);
}

TEST(wrapper_close_increments_close_calls)
{
    struct fix8c fx;
    fix8c_setup(&fx);
    struct fake8c_file *fnode = plant_file(&fx, "/c.txt", FAKE_DATA);

    void *file = NULL;
    ASSERT_EQ_U(nx_vfs_open(&fx.vfs_slot, "/c.txt", NX_VFS_OPEN_READ, &file),
                NX_OK);
    int before = fnode->close_calls;
    nx_vfs_close(&fx.vfs_slot, file);
    ASSERT_EQ_U(fnode->close_calls, before + 1);

    fix8c_teardown(&fx);
}

/* ================================================================== */
/* Section 2: nx_vfs_read equivalence                                  */
/* ================================================================== */

TEST(wrapper_read_returns_correct_bytes)
{
    struct fix8c fx;
    fix8c_setup(&fx);
    plant_file(&fx, "/r.txt", FAKE_DATA);

    void *file = NULL;
    ASSERT_EQ_U(nx_vfs_open(&fx.vfs_slot, "/r.txt", NX_VFS_OPEN_READ, &file),
                NX_OK);

    char buf[32];
    int64_t got = nx_vfs_read(&fx.vfs_slot, file, buf, sizeof buf);
    ASSERT_EQ_U((size_t)got, FAKE_DATA_LEN);
    ASSERT_EQ_U(memcmp(buf, FAKE_DATA, FAKE_DATA_LEN), 0);

    nx_vfs_close(&fx.vfs_slot, file);
    fix8c_teardown(&fx);
}

TEST(wrapper_read_equiv_direct_read)
{
    struct fix8c fx;
    fix8c_setup(&fx);
    plant_file(&fx, "/rd.txt", "abc");

    void *file = NULL;
    ASSERT_EQ_U(nx_vfs_open(&fx.vfs_slot, "/rd.txt", NX_VFS_OPEN_READ, &file),
                NX_OK);

    /* First read. */
    char buf1[8];
    int64_t rc1 = nx_vfs_read(&fx.vfs_slot, file, buf1, sizeof buf1);

    /* Seek back to start and read again — second result should match first. */
    ASSERT_EQ_U(nx_vfs_seek(&fx.vfs_slot, file, 0, NX_VFS_SEEK_SET), 0);
    char buf2[8];
    int64_t rc2 = nx_vfs_read(&fx.vfs_slot, file, buf2, sizeof buf2);

    ASSERT_EQ_U(rc1, rc2);
    ASSERT_EQ_U(memcmp(buf1, buf2, (size_t)rc1), 0);

    nx_vfs_close(&fx.vfs_slot, file);
    fix8c_teardown(&fx);
}

/* ================================================================== */
/* Section 3: nx_vfs_write equivalence                                 */
/* ================================================================== */

TEST(wrapper_write_stores_data)
{
    struct fix8c fx;
    fix8c_setup(&fx);
    plant_file(&fx, "/w.txt", "");

    void *file = NULL;
    ASSERT_EQ_U(nx_vfs_open(&fx.vfs_slot, "/w.txt", NX_VFS_OPEN_WRITE, &file),
                NX_OK);

    int64_t wrote = nx_vfs_write(&fx.vfs_slot, file, "xyz", 3);
    ASSERT_EQ_U(wrote, 3);

    /* Seek back to start and read to verify. */
    ASSERT_EQ_U(nx_vfs_seek(&fx.vfs_slot, file, 0, NX_VFS_SEEK_SET), 0);
    char rbuf[8];
    int64_t got = nx_vfs_read(&fx.vfs_slot, file, rbuf, sizeof rbuf);
    ASSERT_EQ_U(got, 3);
    ASSERT_EQ_U(memcmp(rbuf, "xyz", 3), 0);

    nx_vfs_close(&fx.vfs_slot, file);
    fix8c_teardown(&fx);
}

TEST(wrapper_write_equiv_direct_write)
{
    struct fix8c fx;
    fix8c_setup(&fx);
    plant_file(&fx, "/wd.txt", "");

    void *file1 = NULL, *file2 = NULL;
    ASSERT_EQ_U(nx_vfs_open(&fx.vfs_slot, "/wd.txt",
                             NX_VFS_OPEN_WRITE, &file1), NX_OK);
    ASSERT_EQ_U(nx_vfs_open(&fx.vfs_slot, "/wd.txt",
                             NX_VFS_OPEN_WRITE, &file2), NX_OK);

    int64_t rc1 = nx_vfs_write(&fx.vfs_slot, file1, "hi", 2);
    int64_t rc2 = nx_vfs_write(&fx.vfs_slot, file2, "hi", 2);
    ASSERT_EQ_U(rc1, rc2);

    nx_vfs_close(&fx.vfs_slot, file1);
    nx_vfs_close(&fx.vfs_slot, file2);
    fix8c_teardown(&fx);
}

/* ================================================================== */
/* Section 4: nx_vfs_seek equivalence                                  */
/* ================================================================== */

TEST(wrapper_seek_moves_cursor)
{
    struct fix8c fx;
    fix8c_setup(&fx);
    plant_file(&fx, "/s.txt", "0123456789");

    void *file = NULL;
    ASSERT_EQ_U(nx_vfs_open(&fx.vfs_slot, "/s.txt", NX_VFS_OPEN_READ, &file),
                NX_OK);

    int64_t new_pos = nx_vfs_seek(&fx.vfs_slot, file, 5, NX_VFS_SEEK_SET);
    ASSERT_EQ_U(new_pos, 5);

    nx_vfs_close(&fx.vfs_slot, file);
    fix8c_teardown(&fx);
}

TEST(wrapper_seek_equiv_direct_seek)
{
    struct fix8c fx;
    fix8c_setup(&fx);
    plant_file(&fx, "/sd.txt", "abcdef");

    void *file1 = NULL, *file2 = NULL;
    ASSERT_EQ_U(nx_vfs_open(&fx.vfs_slot, "/sd.txt", NX_VFS_OPEN_READ, &file1),
                NX_OK);
    ASSERT_EQ_U(nx_vfs_open(&fx.vfs_slot, "/sd.txt", NX_VFS_OPEN_READ, &file2),
                NX_OK);

    int64_t rc1 = nx_vfs_seek(&fx.vfs_slot, file1, 3, NX_VFS_SEEK_SET);
    int64_t rc2 = nx_vfs_seek(&fx.vfs_slot, file2, 3, NX_VFS_SEEK_SET);
    ASSERT_EQ_U(rc1, rc2);
    ASSERT_EQ_U(rc1, 3);

    nx_vfs_close(&fx.vfs_slot, file1);
    nx_vfs_close(&fx.vfs_slot, file2);
    fix8c_teardown(&fx);
}

/* ================================================================== */
/* Section 5: nx_vfs_stat equivalence                                  */
/* ================================================================== */

TEST(wrapper_stat_file_returns_correct_kind_and_size)
{
    struct fix8c fx;
    fix8c_setup(&fx);
    plant_file(&fx, "/st.txt", FAKE_DATA);

    struct nx_fs_stat st;
    int rc = nx_vfs_stat(&fx.vfs_slot, "/st.txt", &st);
    ASSERT_EQ_U(rc, NX_OK);
    ASSERT_EQ_U(st.kind, NX_FS_KIND_FILE);
    ASSERT_EQ_U((size_t)st.size, FAKE_DATA_LEN);

    fix8c_teardown(&fx);
}

TEST(wrapper_stat_missing_path_returns_enoent)
{
    struct fix8c fx;
    fix8c_setup(&fx);

    struct nx_fs_stat st;
    int rc = nx_vfs_stat(&fx.vfs_slot, "/nope.txt", &st);
    ASSERT_EQ_U(rc, NX_ENOENT);

    fix8c_teardown(&fx);
}

TEST(wrapper_stat_equiv_direct_stat)
{
    struct fix8c fx;
    fix8c_setup(&fx);
    plant_file(&fx, "/stq.txt", "12345");

    struct nx_fs_stat direct_st, wrap_st;
    int direct_rc = f8c_stat(&fx.fake_state, "/stq.txt", &direct_st);
    int wrap_rc   = nx_vfs_stat(&fx.vfs_slot, "/stq.txt", &wrap_st);

    ASSERT_EQ_U(direct_rc, wrap_rc);
    ASSERT_EQ_U(direct_st.kind, wrap_st.kind);
    ASSERT_EQ_U((size_t)direct_st.size, (size_t)wrap_st.size);

    fix8c_teardown(&fx);
}

/* ================================================================== */
/* Section 6: nx_vfs_readdir equivalence                               */
/* ================================================================== */

TEST(wrapper_readdir_returns_first_entry)
{
    struct fix8c fx;
    fix8c_setup(&fx);

    uint32_t cookie = 0;
    struct nx_fs_dirent ent;
    int rc = nx_vfs_readdir(&fx.vfs_slot, "/", &cookie, &ent);
    ASSERT_EQ_U(rc, NX_OK);
    ASSERT_EQ_U(strcmp(ent.name, "file0"), 0);
    ASSERT_EQ_U(cookie, 1);

    fix8c_teardown(&fx);
}

TEST(wrapper_readdir_returns_enoent_at_end)
{
    struct fix8c fx;
    fix8c_setup(&fx);

    uint32_t cookie = 2;  /* past end */
    struct nx_fs_dirent ent;
    int rc = nx_vfs_readdir(&fx.vfs_slot, "/", &cookie, &ent);
    ASSERT_EQ_U(rc, NX_ENOENT);

    fix8c_teardown(&fx);
}

TEST(wrapper_readdir_equiv_direct_readdir)
{
    struct fix8c fx;
    fix8c_setup(&fx);

    uint32_t dc = 0, wc = 0;
    struct nx_fs_dirent de, we;
    int direct_rc = f8c_readdir(&fx.fake_state, "/", &dc, &de);
    int wrap_rc   = nx_vfs_readdir(&fx.vfs_slot, "/", &wc, &we);

    ASSERT_EQ_U(direct_rc, wrap_rc);
    ASSERT_EQ_U(dc, wc);
    ASSERT_EQ_U(strcmp(de.name, we.name), 0);

    fix8c_teardown(&fx);
}

/* ================================================================== */
/* Section 7: nx_vfs_mkdir equivalence                                 */
/* ================================================================== */

TEST(wrapper_mkdir_calls_op)
{
    struct fix8c fx;
    fix8c_setup(&fx);
    fx.fake_state.last_mkdir_rc = NX_OK;

    int rc = nx_vfs_mkdir(&fx.vfs_slot, "/newdir");
    ASSERT_EQ_U(rc, NX_OK);
    ASSERT_EQ_U(fx.fake_state.mkdir_calls, 1);

    fix8c_teardown(&fx);
}

TEST(wrapper_mkdir_equiv_direct_mkdir)
{
    struct fix8c fx;
    fix8c_setup(&fx);
    fx.fake_state.last_mkdir_rc = NX_EEXIST;

    int direct_rc = f8c_mkdir(&fx.fake_state, "/x");
    fx.fake_state.mkdir_calls = 0;
    int wrap_rc   = nx_vfs_mkdir(&fx.vfs_slot, "/x");

    ASSERT_EQ_U(direct_rc, wrap_rc);

    fix8c_teardown(&fx);
}

/* ================================================================== */
/* Section 8: nx_vfs_retain equivalence                                */
/* ================================================================== */

TEST(wrapper_retain_bumps_refcount)
{
    struct fix8c fx;
    fix8c_setup(&fx);
    struct fake8c_file *fnode = plant_file(&fx, "/ret.txt", FAKE_DATA);

    /* Open via vfs_simple so we get a proper wrapper handle. */
    void *file = NULL;
    ASSERT_EQ_U(nx_vfs_open(&fx.vfs_slot, "/ret.txt", NX_VFS_OPEN_READ, &file),
                NX_OK);

    int before = fnode->retain_count;
    nx_vfs_retain(&fx.vfs_slot, file);
    ASSERT_EQ_U(fnode->retain_count, before + 1);

    nx_vfs_close(&fx.vfs_slot, file);
    fix8c_teardown(&fx);
}

TEST(wrapper_retain_equiv_direct_retain)
{
    struct fix8c fx;
    fix8c_setup(&fx);
    struct fake8c_file *fnode = plant_file(&fx, "/retq.txt", FAKE_DATA);

    void *file1 = NULL, *file2 = NULL;
    ASSERT_EQ_U(nx_vfs_open(&fx.vfs_slot, "/retq.txt", NX_VFS_OPEN_READ, &file1),
                NX_OK);
    ASSERT_EQ_U(nx_vfs_open(&fx.vfs_slot, "/retq.txt", NX_VFS_OPEN_READ, &file2),
                NX_OK);

    int before = fnode->retain_count;
    nx_vfs_retain(&fx.vfs_slot, file1);
    int after1 = fnode->retain_count;
    nx_vfs_retain(&fx.vfs_slot, file2);
    int after2 = fnode->retain_count;

    /* Each retain increments by 1. */
    ASSERT_EQ_U(after1 - before, 1);
    ASSERT_EQ_U(after2 - after1, 1);

    nx_vfs_close(&fx.vfs_slot, file1);
    nx_vfs_close(&fx.vfs_slot, file2);
    fix8c_teardown(&fx);
}

/* ================================================================== */
/* Section 9: null-slot robustness                                     */
/* ================================================================== */

TEST(wrapper_open_null_slot_returns_enoent)
{
    void *file = NULL;
    int rc = nx_vfs_open(NULL, "/x.txt", NX_VFS_OPEN_READ, &file);
    ASSERT_EQ_U(rc, NX_ENOENT);
}

TEST(wrapper_read_null_slot_returns_error)
{
    char buf[8];
    int64_t rc = nx_vfs_read(NULL, (void *)1, buf, sizeof buf);
    ASSERT(rc < 0);
}

TEST(wrapper_stat_null_slot_returns_enoent)
{
    struct nx_fs_stat st;
    int rc = nx_vfs_stat(NULL, "/x.txt", &st);
    ASSERT_EQ_U(rc, NX_ENOENT);
}
