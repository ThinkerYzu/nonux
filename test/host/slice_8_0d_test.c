/*
 * Host-side tests for slice 8.0d — nx_fs_* sync-dispatcher wrappers.
 *
 * Tests the nx_fs_* wrappers (framework/fs_call.c) directly against
 * a fake filesystem driver registered in a slot.  In the host build,
 * wrappers call slot->active->descriptor->iface_ops directly, so
 * these tests pin arg encoding, return-value extraction, and null-slot
 * robustness.
 *
 * Kernel-side end-to-end testing (vfs_simple → ramfs/procfs through
 * the sync handle_msg path) is covered by the existing interactive
 * kernel tests (ls /, echo|cat, mkdir_tmp, etc.) which exercise the
 * full vfs_simple → ramfs path with the new nx_fs_* wrappers.
 *
 * Fixture: single slot "filesystem.root" ← fake_fs driver.
 * No vfs layer — wrappers tested directly against the driver slot.
 */

#include "test_runner.h"

#include "framework/component.h"
#include "framework/fs_call.h"
#include "framework/registry.h"
#include "interfaces/fs.h"
#include "interfaces/fs_types.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ================================================================== */
/* Minimal fake filesystem driver                                       */
/* ================================================================== */

#define FAKE8D_DATA "hello_8_0d"
#define FAKE8D_DATA_LEN 10

struct fake8d_file {
    int     in_use;
    char    name[32];
    uint8_t data[64];
    size_t  size;
    size_t  cursor;
    int     retain_count;
    int     close_calls;
};

struct fake8d_state {
    struct fake8d_file files[4];
    int mkdir_calls;
    int last_mkdir_rc;
};

static int fd8_open(void *self, const char *path, uint32_t flags,
                    void **out)
{
    struct fake8d_state *s = self;
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

static void fd8_close(void *self, void *file)
{
    (void)self;
    struct fake8d_file *f = file;
    if (f) f->close_calls++;
}

static void fd8_retain(void *self, void *file)
{
    (void)self;
    struct fake8d_file *f = file;
    if (f) f->retain_count++;
}

static int64_t fd8_read(void *self, void *file, void *buf, size_t cap)
{
    (void)self;
    struct fake8d_file *f = file;
    if (!f || !buf) return NX_EINVAL;
    size_t avail = f->size > f->cursor ? f->size - f->cursor : 0;
    size_t n = avail < cap ? avail : cap;
    memcpy(buf, f->data + f->cursor, n);
    f->cursor += n;
    return (int64_t)n;
}

static int64_t fd8_write(void *self, void *file, const void *buf, size_t len)
{
    (void)self;
    struct fake8d_file *f = file;
    if (!f || !buf) return NX_EINVAL;
    size_t end = f->cursor + len;
    if (end > sizeof f->data) return NX_ENOMEM;
    memcpy(f->data + f->cursor, buf, len);
    f->cursor += len;
    if (f->cursor > f->size) f->size = f->cursor;
    return (int64_t)len;
}

static int64_t fd8_seek(void *self, void *file, int64_t offset, int whence)
{
    (void)self;
    struct fake8d_file *f = file;
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

static int fd8_readdir(void *self, const char *dir_path,
                       uint32_t *cookie, struct nx_fs_dirent *out)
{
    (void)self; (void)dir_path;
    static const char *names[] = { "alpha", "beta" };
    if (*cookie >= 2) return NX_ENOENT;
    strncpy(out->name, names[*cookie], sizeof out->name - 1);
    out->name_len = (uint32_t)strlen(out->name);
    (*cookie)++;
    return NX_OK;
}

static int fd8_mkdir(void *self, const char *path)
{
    struct fake8d_state *s = self;
    (void)path;
    s->mkdir_calls++;
    return s->last_mkdir_rc;
}

static int fd8_stat(void *self, const char *path, struct nx_fs_stat *out)
{
    struct fake8d_state *s = self;
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

static const struct nx_fs_ops fake8d_ops = {
    .open    = fd8_open,    .close   = fd8_close,
    .retain  = fd8_retain,  .read    = fd8_read,
    .write   = fd8_write,   .seek    = fd8_seek,
    .readdir = fd8_readdir, .mkdir   = fd8_mkdir,
    .stat    = fd8_stat,
};

static const struct nx_component_descriptor fake8d_descriptor = {
    .name        = "fake8d_fs",
    .state_size  = sizeof(struct fake8d_state),
    .deps_offset = 0,
    .deps        = NULL,
    .n_deps      = 0,
    .ops         = NULL,
    .iface_ops   = &fake8d_ops,
};

/* ================================================================== */
/* Fixture                                                              */
/* ================================================================== */

struct fix8d {
    struct nx_slot      root_slot;
    struct nx_component fake_comp;
    struct fake8d_state fake_state;
};

static void fix8d_setup(struct fix8d *fx)
{
    nx_graph_reset();
    memset(fx, 0, sizeof *fx);

    fx->root_slot.name        = "filesystem.root";
    fx->root_slot.iface       = "filesystem";
    fx->root_slot.mutability  = NX_MUT_HOT;
    fx->root_slot.concurrency = NX_CONC_SHARED;
    ASSERT_EQ_U(nx_slot_register(&fx->root_slot), NX_OK);

    fx->fake_comp.manifest_id = "fake8d_fs";
    fx->fake_comp.instance_id = "0";
    fx->fake_comp.impl        = &fx->fake_state;
    fx->fake_comp.descriptor  = &fake8d_descriptor;
    ASSERT_EQ_U(nx_component_register(&fx->fake_comp), NX_OK);
    ASSERT_EQ_U(nx_slot_swap(&fx->root_slot, &fx->fake_comp), NX_OK);
}

static void fix8d_teardown(struct fix8d *fx)
{
    nx_graph_reset();
    (void)fx;
}

/* Plant a file with known content in the fake driver. */
static struct fake8d_file *plant_file(struct fix8d *fx, const char *name,
                                      const char *content)
{
    for (int i = 0; i < 4; i++) {
        if (fx->fake_state.files[i].in_use) continue;
        struct fake8d_file *f = &fx->fake_state.files[i];
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
/* Section 1: nx_fs_open / nx_fs_close                                 */
/* ================================================================== */

TEST(fs_wrapper_open_existing_file_returns_ok)
{
    struct fix8d fx;
    fix8d_setup(&fx);
    plant_file(&fx, "/a.txt", FAKE8D_DATA);

    void *file = NULL;
    int rc = nx_fs_open(&fx.root_slot, "/a.txt", NX_FS_OPEN_READ, &file);
    ASSERT_EQ_U(rc, NX_OK);
    ASSERT_NOT_NULL(file);
    nx_fs_close(&fx.root_slot, file);

    fix8d_teardown(&fx);
}

TEST(fs_wrapper_open_missing_file_returns_enoent)
{
    struct fix8d fx;
    fix8d_setup(&fx);

    void *file = NULL;
    int rc = nx_fs_open(&fx.root_slot, "/no.txt", NX_FS_OPEN_READ, &file);
    ASSERT_EQ_U(rc, NX_ENOENT);

    fix8d_teardown(&fx);
}

TEST(fs_wrapper_open_equiv_direct_open)
{
    struct fix8d fx;
    fix8d_setup(&fx);
    plant_file(&fx, "/eq.txt", FAKE8D_DATA);

    void *direct_file = NULL;
    int direct_rc = fd8_open(&fx.fake_state, "/eq.txt", NX_FS_OPEN_READ,
                             &direct_file);
    if (direct_file) fd8_close(&fx.fake_state, direct_file);

    void *wrap_file = NULL;
    int wrap_rc = nx_fs_open(&fx.root_slot, "/eq.txt", NX_FS_OPEN_READ,
                             &wrap_file);
    if (wrap_file) nx_fs_close(&fx.root_slot, wrap_file);

    ASSERT_EQ_U(direct_rc, wrap_rc);

    fix8d_teardown(&fx);
}

TEST(fs_wrapper_close_increments_close_calls)
{
    struct fix8d fx;
    fix8d_setup(&fx);
    struct fake8d_file *fnode = plant_file(&fx, "/cl.txt", FAKE8D_DATA);

    void *file = NULL;
    ASSERT_EQ_U(nx_fs_open(&fx.root_slot, "/cl.txt", NX_FS_OPEN_READ, &file),
                NX_OK);
    int before = fnode->close_calls;
    nx_fs_close(&fx.root_slot, file);
    ASSERT_EQ_U(fnode->close_calls, before + 1);

    fix8d_teardown(&fx);
}

/* ================================================================== */
/* Section 2: nx_fs_read equivalence                                   */
/* ================================================================== */

TEST(fs_wrapper_read_returns_correct_bytes)
{
    struct fix8d fx;
    fix8d_setup(&fx);
    plant_file(&fx, "/rd.txt", FAKE8D_DATA);

    void *file = NULL;
    ASSERT_EQ_U(nx_fs_open(&fx.root_slot, "/rd.txt", NX_FS_OPEN_READ, &file),
                NX_OK);

    char buf[16] = {0};
    int64_t n = nx_fs_read(&fx.root_slot, file, buf, sizeof buf);
    ASSERT_EQ_U((unsigned)n, FAKE8D_DATA_LEN);
    ASSERT_EQ_U(memcmp(buf, FAKE8D_DATA, FAKE8D_DATA_LEN), 0);

    nx_fs_close(&fx.root_slot, file);
    fix8d_teardown(&fx);
}

TEST(fs_wrapper_read_equiv_direct_read)
{
    struct fix8d fx;
    fix8d_setup(&fx);
    plant_file(&fx, "/rdeq.txt", FAKE8D_DATA);

    /* Direct open+read+close first; fd8_open resets cursor to 0 each open. */
    void *df = NULL;
    fd8_open(&fx.fake_state, "/rdeq.txt", NX_FS_OPEN_READ, &df);
    char dbuf[16] = {0};
    int64_t drc = fd8_read(&fx.fake_state, df, dbuf, sizeof dbuf);
    fd8_close(&fx.fake_state, df);

    /* Wrapper open+read+close; nx_fs_open calls fd8_open which resets cursor. */
    void *wf = NULL;
    nx_fs_open(&fx.root_slot, "/rdeq.txt", NX_FS_OPEN_READ, &wf);
    char wbuf[16] = {0};
    int64_t wrc = nx_fs_read(&fx.root_slot, wf, wbuf, sizeof wbuf);
    nx_fs_close(&fx.root_slot, wf);

    ASSERT_EQ_U((unsigned long long)drc, (unsigned long long)wrc);
    ASSERT_EQ_U(memcmp(dbuf, wbuf, (size_t)drc), 0);

    fix8d_teardown(&fx);
}

/* ================================================================== */
/* Section 3: nx_fs_write equivalence                                  */
/* ================================================================== */

TEST(fs_wrapper_write_stores_data)
{
    struct fix8d fx;
    fix8d_setup(&fx);

    void *file = NULL;
    ASSERT_EQ_U(nx_fs_open(&fx.root_slot, "/wr.txt",
                           NX_FS_OPEN_WRITE | NX_FS_OPEN_CREATE, &file),
                NX_OK);
    int64_t n = nx_fs_write(&fx.root_slot, file, "abc", 3);
    ASSERT_EQ_U((unsigned)n, 3);
    nx_fs_close(&fx.root_slot, file);

    fix8d_teardown(&fx);
}

TEST(fs_wrapper_write_equiv_direct_write)
{
    struct fix8d fx;
    fix8d_setup(&fx);

    void *df = NULL, *wf = NULL;
    fd8_open(&fx.fake_state, "/weq.txt", NX_FS_OPEN_WRITE | NX_FS_OPEN_CREATE,
             &df);
    nx_fs_open(&fx.root_slot, "/weq2.txt",
               NX_FS_OPEN_WRITE | NX_FS_OPEN_CREATE, &wf);

    int64_t drc = fd8_write(&fx.fake_state, df, "xy", 2);
    int64_t wrc = nx_fs_write(&fx.root_slot, wf, "xy", 2);

    ASSERT_EQ_U((unsigned long long)drc, (unsigned long long)wrc);

    fd8_close(&fx.fake_state, df);
    nx_fs_close(&fx.root_slot, wf);
    fix8d_teardown(&fx);
}

/* ================================================================== */
/* Section 4: nx_fs_seek equivalence                                   */
/* ================================================================== */

TEST(fs_wrapper_seek_moves_cursor)
{
    struct fix8d fx;
    fix8d_setup(&fx);
    plant_file(&fx, "/sk.txt", "abcdef");

    void *file = NULL;
    ASSERT_EQ_U(nx_fs_open(&fx.root_slot, "/sk.txt", NX_FS_OPEN_READ, &file),
                NX_OK);
    int64_t pos = nx_fs_seek(&fx.root_slot, file, 3, NX_FS_SEEK_SET);
    ASSERT_EQ_U((unsigned long long)pos, 3ULL);

    char c = 0;
    nx_fs_read(&fx.root_slot, file, &c, 1);
    ASSERT_EQ_U(c, 'd');

    nx_fs_close(&fx.root_slot, file);
    fix8d_teardown(&fx);
}

TEST(fs_wrapper_seek_equiv_direct_seek)
{
    struct fix8d fx;
    fix8d_setup(&fx);
    plant_file(&fx, "/skeq.txt", "0123456789");

    void *df = NULL, *wf = NULL;
    fd8_open(&fx.fake_state, "/skeq.txt", NX_FS_OPEN_READ, &df);
    nx_fs_open(&fx.root_slot, "/skeq.txt", NX_FS_OPEN_READ, &wf);

    int64_t drc = fd8_seek(&fx.fake_state, df, 5, NX_FS_SEEK_SET);
    int64_t wrc = nx_fs_seek(&fx.root_slot, wf, 5, NX_FS_SEEK_SET);

    ASSERT_EQ_U((unsigned long long)drc, (unsigned long long)wrc);

    fd8_close(&fx.fake_state, df);
    nx_fs_close(&fx.root_slot, wf);
    fix8d_teardown(&fx);
}

/* ================================================================== */
/* Section 5: nx_fs_stat equivalence                                   */
/* ================================================================== */

TEST(fs_wrapper_stat_file_returns_correct_kind_and_size)
{
    struct fix8d fx;
    fix8d_setup(&fx);
    plant_file(&fx, "/st.txt", "hello");

    struct nx_fs_stat st;
    memset(&st, 0, sizeof st);
    int rc = nx_fs_stat(&fx.root_slot, "/st.txt", &st);
    ASSERT_EQ_U(rc, NX_OK);
    ASSERT_EQ_U(st.kind, NX_FS_KIND_FILE);
    ASSERT_EQ_U((unsigned long long)st.size, 5ULL);

    fix8d_teardown(&fx);
}

TEST(fs_wrapper_stat_equiv_direct_stat)
{
    struct fix8d fx;
    fix8d_setup(&fx);
    plant_file(&fx, "/steq.txt", "xyz");

    struct nx_fs_stat dst, wst;
    memset(&dst, 0, sizeof dst);
    memset(&wst, 0, sizeof wst);

    int drc = fd8_stat(&fx.fake_state, "/steq.txt", &dst);
    int wrc = nx_fs_stat(&fx.root_slot, "/steq.txt", &wst);

    ASSERT_EQ_U(drc, wrc);
    ASSERT_EQ_U(dst.kind, wst.kind);
    ASSERT_EQ_U((unsigned long long)dst.size, (unsigned long long)wst.size);

    fix8d_teardown(&fx);
}

/* ================================================================== */
/* Section 6: nx_fs_readdir equivalence                                */
/* ================================================================== */

TEST(fs_wrapper_readdir_returns_first_entry)
{
    struct fix8d fx;
    fix8d_setup(&fx);

    uint32_t cookie = 0;
    struct nx_fs_dirent ent;
    memset(&ent, 0, sizeof ent);
    int rc = nx_fs_readdir(&fx.root_slot, "/", &cookie, &ent);
    ASSERT_EQ_U(rc, NX_OK);
    ASSERT_EQ_U(strcmp(ent.name, "alpha"), 0);
    ASSERT_EQ_U(cookie, 1);

    fix8d_teardown(&fx);
}

TEST(fs_wrapper_readdir_returns_enoent_at_end)
{
    struct fix8d fx;
    fix8d_setup(&fx);

    uint32_t cookie = 2;
    struct nx_fs_dirent ent;
    int rc = nx_fs_readdir(&fx.root_slot, "/", &cookie, &ent);
    ASSERT_EQ_U(rc, NX_ENOENT);

    fix8d_teardown(&fx);
}

TEST(fs_wrapper_readdir_equiv_direct_readdir)
{
    struct fix8d fx;
    fix8d_setup(&fx);

    uint32_t dc = 0, wc = 0;
    struct nx_fs_dirent de, we;
    memset(&de, 0, sizeof de);
    memset(&we, 0, sizeof we);

    int drc = fd8_readdir(&fx.fake_state, "/", &dc, &de);
    int wrc = nx_fs_readdir(&fx.root_slot, "/", &wc, &we);

    ASSERT_EQ_U(drc, wrc);
    ASSERT_EQ_U(dc, wc);
    ASSERT_EQ_U(strcmp(de.name, we.name), 0);

    fix8d_teardown(&fx);
}

/* ================================================================== */
/* Section 7: nx_fs_mkdir equivalence                                  */
/* ================================================================== */

TEST(fs_wrapper_mkdir_calls_op)
{
    struct fix8d fx;
    fix8d_setup(&fx);
    fx.fake_state.last_mkdir_rc = NX_OK;

    int rc = nx_fs_mkdir(&fx.root_slot, "/newdir");
    ASSERT_EQ_U(rc, NX_OK);
    ASSERT_EQ_U(fx.fake_state.mkdir_calls, 1);

    fix8d_teardown(&fx);
}

TEST(fs_wrapper_mkdir_equiv_direct_mkdir)
{
    struct fix8d fx;
    fix8d_setup(&fx);
    fx.fake_state.last_mkdir_rc = NX_EEXIST;

    int drc = fd8_mkdir(&fx.fake_state, "/x");
    fx.fake_state.mkdir_calls = 0;
    int wrc = nx_fs_mkdir(&fx.root_slot, "/x");

    ASSERT_EQ_U(drc, wrc);

    fix8d_teardown(&fx);
}

/* ================================================================== */
/* Section 8: nx_fs_retain equivalence                                 */
/* ================================================================== */

TEST(fs_wrapper_retain_bumps_driver_refcount)
{
    struct fix8d fx;
    fix8d_setup(&fx);
    struct fake8d_file *fnode = plant_file(&fx, "/ret.txt", FAKE8D_DATA);

    void *file = NULL;
    ASSERT_EQ_U(nx_fs_open(&fx.root_slot, "/ret.txt", NX_FS_OPEN_READ, &file),
                NX_OK);
    int before = fnode->retain_count;
    nx_fs_retain(&fx.root_slot, file);
    ASSERT_EQ_U(fnode->retain_count, before + 1);

    nx_fs_close(&fx.root_slot, file);
    fix8d_teardown(&fx);
}

TEST(fs_wrapper_retain_equiv_direct_retain)
{
    struct fix8d fx;
    fix8d_setup(&fx);
    struct fake8d_file *f1 = plant_file(&fx, "/r1.txt", FAKE8D_DATA);
    struct fake8d_file *f2 = plant_file(&fx, "/r2.txt", FAKE8D_DATA);
    (void)f2;

    void *wf = NULL;
    ASSERT_EQ_U(nx_fs_open(&fx.root_slot, "/r1.txt", NX_FS_OPEN_READ, &wf),
                NX_OK);

    int before = f1->retain_count;
    fd8_retain(&fx.fake_state, wf);       /* direct: file ptr is the fake node */
    int after_direct = f1->retain_count;
    nx_fs_retain(&fx.root_slot, wf);       /* wrapper */
    int after_wrap = f1->retain_count;

    ASSERT_EQ_U(after_direct - before, 1);
    ASSERT_EQ_U(after_wrap - after_direct, 1);

    nx_fs_close(&fx.root_slot, wf);
    fix8d_teardown(&fx);
}

/* ================================================================== */
/* Section 9: null-slot robustness                                     */
/* ================================================================== */

TEST(fs_wrapper_open_null_slot_returns_enoent)
{
    void *file = NULL;
    int rc = nx_fs_open(NULL, "/x.txt", NX_FS_OPEN_READ, &file);
    ASSERT_EQ_U(rc, NX_ENOENT);
}

TEST(fs_wrapper_read_null_slot_returns_error)
{
    char buf[8];
    int64_t rc = nx_fs_read(NULL, (void *)1, buf, sizeof buf);
    ASSERT(rc < 0);
}

TEST(fs_wrapper_stat_null_slot_returns_enoent)
{
    struct nx_fs_stat st;
    int rc = nx_fs_stat(NULL, "/x.txt", &st);
    ASSERT_EQ_U(rc, NX_ENOENT);
}

TEST(fs_wrapper_mkdir_null_slot_returns_error)
{
    int rc = nx_fs_mkdir(NULL, "/d");
    ASSERT(rc < 0);
}

TEST(fs_wrapper_readdir_null_slot_returns_error)
{
    uint32_t cookie = 0;
    struct nx_fs_dirent ent;
    int rc = nx_fs_readdir(NULL, "/", &cookie, &ent);
    ASSERT(rc < 0);
}
