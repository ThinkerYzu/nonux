/*
 * Slice 9b.1 host tests — component-owned object tables.
 *
 * Exercises the new ID-based open/read/write/seek/close API exposed by
 * ramfs (via fs IDL), vfs_simple (via vfs IDL), and verifies that the
 * char_device IDL now includes a `read` op.
 */

#include "test_runner.h"

#include "conformance/conformance_fs.h"
#include "framework/component.h"
#include "framework/registry.h"
#include "interfaces/fs.h"
#include "interfaces/vfs.h"
#include "interfaces/char_device.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* Exported by components/ramfs/ramfs.c. */
extern const struct nx_fs_ops              ramfs_fs_ops;
extern const struct nx_component_ops       ramfs_component_ops;
extern const struct nx_component_descriptor ramfs_descriptor;

/* Exported by components/vfs_simple/vfs_simple.c. */
extern const struct nx_vfs_ops             vfs_simple_vfs_ops;
extern const struct nx_component_ops       vfs_simple_component_ops;
extern const struct nx_component_descriptor vfs_simple_descriptor;

/* ---------- Ramfs helpers -------------------------------------------- */

static void *ramfs_9b_create(void)
{
    void *s = calloc(1, ramfs_descriptor.state_size);
    if (!s) return NULL;
    ramfs_component_ops.init(s);
    ramfs_component_ops.enable(s);
    return s;
}

static void ramfs_9b_destroy(void *s)
{
    ramfs_component_ops.disable(s);
    ramfs_component_ops.destroy(s);
    free(s);
}

/* ---------- Tests: ramfs ID table ------------------------------------ */

/* 1: open returns non-zero id; missing path returns zero */
TEST(ramfs_9b_open_returns_nonzero_id)
{
    void *s = ramfs_9b_create();
    ASSERT_NOT_NULL(s);

    uint32_t id = ramfs_fs_ops.open(s, "/a",
                                    NX_FS_OPEN_READ | NX_FS_OPEN_WRITE |
                                    NX_FS_OPEN_CREATE);
    ASSERT(id != 0);
    ASSERT(id >= 1);
    ramfs_fs_ops.close(s, id);

    /* Missing path without CREATE returns 0. */
    uint32_t bad = ramfs_fs_ops.open(s, "/nope", NX_FS_OPEN_READ);
    ASSERT_EQ_U(bad, 0);

    ramfs_9b_destroy(s);
}

/* 2: write then read round-trip via IDs */
TEST(ramfs_9b_id_write_read_roundtrip)
{
    void *s = ramfs_9b_create();
    ASSERT_NOT_NULL(s);

    uint32_t w = ramfs_fs_ops.open(s, "/msg",
                                   NX_FS_OPEN_READ | NX_FS_OPEN_WRITE |
                                   NX_FS_OPEN_CREATE);
    ASSERT(w != 0);

    static const char payload[] = "hello-9b";
    int64_t wrote = ramfs_fs_ops.write(s, w, payload, sizeof payload - 1);
    ASSERT_EQ_U((uint64_t)wrote, sizeof payload - 1);
    ramfs_fs_ops.close(s, w);

    uint32_t r = ramfs_fs_ops.open(s, "/msg", NX_FS_OPEN_READ);
    ASSERT(r != 0);

    char buf[32] = {0};
    int64_t got = ramfs_fs_ops.read(s, r, buf, sizeof buf);
    ASSERT_EQ_U((uint64_t)got, sizeof payload - 1);
    ASSERT(memcmp(buf, payload, sizeof payload - 1) == 0);

    ramfs_fs_ops.close(s, r);
    ramfs_9b_destroy(s);
}

/* 3: stale id after close returns error */
TEST(ramfs_9b_stale_id_after_close_returns_error)
{
    void *s = ramfs_9b_create();
    ASSERT_NOT_NULL(s);

    uint32_t id = ramfs_fs_ops.open(s, "/x",
                                    NX_FS_OPEN_READ | NX_FS_OPEN_WRITE |
                                    NX_FS_OPEN_CREATE);
    ASSERT(id != 0);
    ramfs_fs_ops.close(s, id);

    char buf[4];
    int64_t rc = ramfs_fs_ops.read(s, id, buf, sizeof buf);
    ASSERT((int64_t)rc < 0);  /* stale id: invalid */

    ramfs_9b_destroy(s);
}

/* 4: two opens on the same file have independent cursors */
TEST(ramfs_9b_two_ids_independent_cursors)
{
    void *s = ramfs_9b_create();
    ASSERT_NOT_NULL(s);

    uint32_t w = ramfs_fs_ops.open(s, "/c",
                                   NX_FS_OPEN_READ | NX_FS_OPEN_WRITE |
                                   NX_FS_OPEN_CREATE);
    ASSERT(w != 0);
    ramfs_fs_ops.write(s, w, "0123456789", 10);
    ramfs_fs_ops.close(s, w);

    uint32_t r1 = ramfs_fs_ops.open(s, "/c", NX_FS_OPEN_READ);
    uint32_t r2 = ramfs_fs_ops.open(s, "/c", NX_FS_OPEN_READ);
    ASSERT(r1 != 0);
    ASSERT(r2 != 0);
    ASSERT(r1 != r2);  /* independent ID slots */

    char a[4], b[4];
    ramfs_fs_ops.read(s, r1, a, 4);     /* r1 cursor → 4 */
    ramfs_fs_ops.read(s, r2, b, 4);     /* r2 cursor still at 0 */
    ASSERT(memcmp(a, "0123", 4) == 0);
    ASSERT(memcmp(b, "0123", 4) == 0);

    ramfs_fs_ops.close(s, r1);
    ramfs_fs_ops.close(s, r2);
    ramfs_9b_destroy(s);
}

/* 5: seek via id repositions cursor */
TEST(ramfs_9b_seek_via_id_works)
{
    void *s = ramfs_9b_create();
    ASSERT_NOT_NULL(s);

    uint32_t id = ramfs_fs_ops.open(s, "/s",
                                    NX_FS_OPEN_READ | NX_FS_OPEN_WRITE |
                                    NX_FS_OPEN_CREATE);
    ASSERT(id != 0);
    ramfs_fs_ops.write(s, id, "abcdef", 6);

    int64_t pos = ramfs_fs_ops.seek(s, id, 0, NX_FS_SEEK_SET);
    ASSERT_EQ_U((uint64_t)pos, 0);

    char buf[4] = {0};
    ramfs_fs_ops.read(s, id, buf, 3);
    ASSERT(memcmp(buf, "abc", 3) == 0);

    ramfs_fs_ops.close(s, id);
    ramfs_9b_destroy(s);
}

/* 6: table-full returns 0 */
TEST(ramfs_9b_table_full_returns_zero)
{
    void *s = ramfs_9b_create();
    ASSERT_NOT_NULL(s);

    /* Open the same file many times until the open-slot pool fills. */
    /* First create the file. */
    uint32_t seed = ramfs_fs_ops.open(s, "/f",
                                      NX_FS_OPEN_READ | NX_FS_OPEN_WRITE |
                                      NX_FS_OPEN_CREATE);
    ASSERT(seed != 0);
    ramfs_fs_ops.close(s, seed);

    enum { ATTEMPTS = 256 };  /* > RAMFS_MAX_OPEN (128 = 4*32) */
    uint32_t ids[ATTEMPTS];
    unsigned n = 0;
    for (int i = 0; i < ATTEMPTS; i++) {
        uint32_t id = ramfs_fs_ops.open(s, "/f", NX_FS_OPEN_READ);
        if (id == 0) break;
        ids[n++] = id;
    }
    ASSERT(n > 0);
    ASSERT(n < (unsigned)ATTEMPTS);

    for (unsigned i = 0; i < n; i++) ramfs_fs_ops.close(s, ids[i]);
    ramfs_9b_destroy(s);
}

/* ---------- Tests: vfs_simple ID forwarding -------------------------- */

struct vfs9b_fixture {
    struct nx_slot       root_slot;
    struct nx_component  ramfs_comp;
    void                *ramfs_state;
    void                *vfs_state;
};

static void vfs9b_setup(struct vfs9b_fixture *fx)
{
    nx_graph_reset();
    memset(fx, 0, sizeof *fx);

    fx->root_slot.name        = "filesystem.root";
    fx->root_slot.iface       = "filesystem";
    fx->root_slot.mutability  = NX_MUT_HOT;
    fx->root_slot.concurrency = NX_CONC_SHARED;
    nx_slot_register(&fx->root_slot);

    fx->ramfs_state = calloc(1, ramfs_descriptor.state_size);
    ramfs_component_ops.init(fx->ramfs_state);
    ramfs_component_ops.enable(fx->ramfs_state);

    fx->ramfs_comp.manifest_id = "ramfs";
    fx->ramfs_comp.instance_id = "0";
    fx->ramfs_comp.impl        = fx->ramfs_state;
    fx->ramfs_comp.descriptor  = &ramfs_descriptor;
    nx_component_register(&fx->ramfs_comp);
    nx_slot_swap(&fx->root_slot, &fx->ramfs_comp);

    fx->vfs_state = calloc(1, vfs_simple_descriptor.state_size);
    vfs_simple_component_ops.init(fx->vfs_state);
    vfs_simple_component_ops.enable(fx->vfs_state);
}

static void vfs9b_teardown(struct vfs9b_fixture *fx)
{
    if (fx->vfs_state) {
        vfs_simple_component_ops.disable(fx->vfs_state);
        vfs_simple_component_ops.destroy(fx->vfs_state);
        free(fx->vfs_state);
    }
    if (fx->ramfs_state) {
        ramfs_component_ops.disable(fx->ramfs_state);
        ramfs_component_ops.destroy(fx->ramfs_state);
        free(fx->ramfs_state);
    }
}

/* 7: vfs_simple open returns a VFS-local id (non-zero) */
TEST(vfs_simple_9b_open_returns_local_id)
{
    struct vfs9b_fixture fx;
    vfs9b_setup(&fx);

    uint32_t id = vfs_simple_vfs_ops.open(fx.vfs_state, "/a",
                                           NX_VFS_OPEN_READ | NX_VFS_OPEN_WRITE |
                                           NX_VFS_OPEN_CREATE);
    ASSERT(id != 0);
    vfs_simple_vfs_ops.close(fx.vfs_state, id);
    vfs9b_teardown(&fx);
}

/* 8: vfs_simple read/write round-trip via ID */
TEST(vfs_simple_9b_read_write_roundtrip_via_id)
{
    struct vfs9b_fixture fx;
    vfs9b_setup(&fx);

    uint32_t w = vfs_simple_vfs_ops.open(fx.vfs_state, "/hello",
                                          NX_VFS_OPEN_READ | NX_VFS_OPEN_WRITE |
                                          NX_VFS_OPEN_CREATE);
    ASSERT(w != 0);
    int64_t wrote = vfs_simple_vfs_ops.write(fx.vfs_state, w, "9b-vfs", 6);
    ASSERT_EQ_U((uint64_t)wrote, 6);
    vfs_simple_vfs_ops.close(fx.vfs_state, w);

    uint32_t r = vfs_simple_vfs_ops.open(fx.vfs_state, "/hello",
                                          NX_VFS_OPEN_READ);
    ASSERT(r != 0);
    char buf[16] = {0};
    int64_t got = vfs_simple_vfs_ops.read(fx.vfs_state, r, buf, sizeof buf);
    ASSERT_EQ_U((uint64_t)got, 6);
    ASSERT(memcmp(buf, "9b-vfs", 6) == 0);
    vfs_simple_vfs_ops.close(fx.vfs_state, r);
    vfs9b_teardown(&fx);
}

/* ---------- Tests: char_device read op present in IDL ---------------- */

/* 9: char_device IDL now has a read op in the ops struct.
 * We verify this by implementing a minimal stub and confirming the
 * struct field exists at compile-time. */
static int64_t dummy_char_read(void *self, uint32_t id, void *buf, size_t cap)
{
    (void)self; (void)id; (void)buf; (void)cap;
    return 0;
}

TEST(char_device_9b_read_op_in_struct)
{
    /* Compile-time check: nx_char_device_ops.read field exists (slice 9b.1). */
    struct nx_char_device_ops ops = {
        .read = dummy_char_read,
    };
    ASSERT_NOT_NULL(ops.read);
    ASSERT_EQ_U((uint64_t)ops.read(NULL, 0, NULL, 0), 0);
}

/* 10: char_device read takes (self, id, buf, cap) — id is u32 (singleton
 * ignores it; multi-device would select instance). */
static uint32_t g_capture_id;
static int      g_capture_called;

static int64_t capture_read_fn(void *self, uint32_t id, void *buf, size_t cap)
{
    (void)self; (void)buf; (void)cap;
    g_capture_id = id;
    g_capture_called = 1;
    return 0;
}

TEST(char_device_9b_read_signature_is_id_based)
{
    struct nx_char_device_ops ops = { .read = capture_read_fn };
    g_capture_called = 0; g_capture_id = 0xDEAD;
    ops.read(NULL, 42, NULL, 0);
    ASSERT(g_capture_called);
    ASSERT_EQ_U(g_capture_id, 42);
}
