/*
 * Host tests for components/vfs_simple/ (slice 6.2).
 *
 * vfs_simple is a pure dispatch layer — every call resolves the
 * `filesystem.root` slot and forwards to the driver's `nx_fs_ops`.
 * The conformance suite that proves fs-driver correctness lives in
 * conformance_fs; running it against vfs_simple as well is the
 * moral equivalent of running the scheduler conformance against
 * both sched_rr and a trivial in-test dispatcher — redundant because
 * a correct dispatch layer + a conformant driver is by construction
 * conformant, but the kernel ktest (`ktest_vfs`) still exercises the
 * end-to-end round-trip through both layers to catch regressions.
 *
 * What this file proves instead:
 *   1. Lifecycle plumbing — init/enable/disable/destroy counters tick
 *      as expected.
 *   2. Late binding — vfs_simple resolves the slot fresh on every
 *      call, so a slot that becomes unbound after a successful open
 *      fails subsequent ops with NX_ENOENT.
 *   3. Pass-through correctness — a fake fs driver registered in-test
 *      (via the same public slot API the real bootstrap uses) sees
 *      the exact bytes / flags / status codes the harness expects.
 *   4. Input validation that happens *above* the driver — relative
 *      paths rejected with NX_EINVAL before touching the driver.
 */

#include "test_runner.h"

#include "framework/component.h"
#include "framework/registry.h"
#include "interfaces/fs.h"
#include "interfaces/vfs.h"

#include <stdlib.h>
#include <string.h>

/* Exported by components/vfs_simple/vfs_simple.c. */
extern const struct nx_vfs_ops             vfs_simple_vfs_ops;
extern const struct nx_component_ops       vfs_simple_component_ops;
extern const struct nx_component_descriptor vfs_simple_descriptor;

/* ---------- Fake fs driver ------------------------------------------ */

/*
 * A minimal test-local driver that records the calls vfs_simple made.
 * Not a component — we inject it directly via a hand-built descriptor
 * so the test is a pure-function check of vfs_simple's dispatch logic
 * (no bootstrap, no topo order, no IPC).
 */
struct fake_fs_state {
    int     open_calls;
    int     close_calls;
    int     read_calls;
    int     write_calls;

    /* Last-call capture for assertions. */
    char    last_path[32];
    uint32_t last_flags;

    /* A toy one-file backing store.  Read returns bytes from `data`;
     * write appends to it.  Good enough to prove round-trips land on
     * the driver side. */
    uint8_t data[64];
    size_t  size;
    int     file_sentinel;  /* pointer to this field is the opaque "file" */
};

static int fake_open(void *self, const char *path, uint32_t flags,
                     void **out_file)
{
    struct fake_fs_state *s = self;
    s->open_calls++;
    size_t n = 0;
    while (path[n] && n < sizeof s->last_path - 1) {
        s->last_path[n] = path[n]; n++;
    }
    s->last_path[n] = '\0';
    s->last_flags = flags;

    *out_file = &s->file_sentinel;
    return NX_OK;
}

static void fake_close(void *self, void *file)
{
    struct fake_fs_state *s = self;
    s->close_calls++;
    (void)file;
}

static int64_t fake_read(void *self, void *file, void *buf, size_t cap)
{
    struct fake_fs_state *s = self;
    s->read_calls++;
    (void)file;
    size_t n = s->size < cap ? s->size : cap;
    memcpy(buf, s->data, n);
    return (int64_t)n;
}

static int64_t fake_write(void *self, void *file, const void *buf, size_t len)
{
    struct fake_fs_state *s = self;
    s->write_calls++;
    (void)file;
    size_t room = sizeof s->data - s->size;
    size_t n = len < room ? len : room;
    memcpy(s->data + s->size, buf, n);
    s->size += n;
    return (int64_t)n;
}

static const struct nx_fs_ops fake_fs_ops = {
    .open  = fake_open,
    .close = fake_close,
    .read  = fake_read,
    .write = fake_write,
};

static const struct nx_component_descriptor fake_fs_descriptor = {
    .name        = "fake_fs",
    .state_size  = sizeof(struct fake_fs_state),
    .deps_offset = 0,
    .deps        = NULL,
    .n_deps      = 0,
    .ops         = NULL,           /* not exercised */
    .iface_ops   = &fake_fs_ops,   /* what vfs_simple reads */
};

/* ---------- Fixture setup ------------------------------------------- */

/*
 * vfs_simple_state is private to vfs_simple.c; tests allocate it by
 * the descriptor-published size and treat it as an opaque `void *`.
 * Same technique the slice-5.2 mm_buddy tests use.
 */
struct fixture {
    struct nx_slot       root_slot;
    struct nx_component  fake_comp;
    struct fake_fs_state fake_state;
    void                *vfs_state;
};

static void fixture_setup(struct fixture *fx)
{
    nx_graph_reset();
    memset(fx, 0, sizeof *fx);

    fx->root_slot.name        = "filesystem.root";
    fx->root_slot.iface       = "filesystem";
    fx->root_slot.mutability  = NX_MUT_HOT;
    fx->root_slot.concurrency = NX_CONC_SHARED;
    ASSERT_EQ_U(nx_slot_register(&fx->root_slot), NX_OK);

    fx->fake_comp.manifest_id = "fake_fs";
    fx->fake_comp.instance_id = "0";
    fx->fake_comp.impl        = &fx->fake_state;
    fx->fake_comp.descriptor  = &fake_fs_descriptor;
    ASSERT_EQ_U(nx_component_register(&fx->fake_comp), NX_OK);
    ASSERT_EQ_U(nx_slot_swap(&fx->root_slot, &fx->fake_comp), NX_OK);

    fx->vfs_state = calloc(1, vfs_simple_descriptor.state_size);
    ASSERT_NOT_NULL(fx->vfs_state);
    ASSERT_EQ_U(vfs_simple_component_ops.init(fx->vfs_state),   NX_OK);
    ASSERT_EQ_U(vfs_simple_component_ops.enable(fx->vfs_state), NX_OK);
}

static void fixture_teardown(struct fixture *fx)
{
    if (fx->vfs_state) {
        vfs_simple_component_ops.disable(fx->vfs_state);
        vfs_simple_component_ops.destroy(fx->vfs_state);
        free(fx->vfs_state);
        fx->vfs_state = NULL;
    }
}

/* ---------- Tests --------------------------------------------------- */

TEST(vfs_simple_lifecycle_counters_tick)
{
    nx_graph_reset();
    void *s = calloc(1, vfs_simple_descriptor.state_size);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ_U(vfs_simple_component_ops.init(s),    NX_OK);
    ASSERT_EQ_U(vfs_simple_component_ops.enable(s),  NX_OK);
    ASSERT_EQ_U(vfs_simple_component_ops.disable(s), NX_OK);
    vfs_simple_component_ops.destroy(s);
    /* vfs_simple_state is private to vfs_simple.c; the counters live
     * at offsets 0..3 as `unsigned` — see its source.  We treat the
     * state as `unsigned[4]` to inspect them without exposing the
     * struct.  If the component reorders fields this test fails
     * loudly, which is the point. */
    const unsigned *ctrs = s;
    ASSERT_EQ_U(ctrs[0], 1);  /* init */
    ASSERT_EQ_U(ctrs[1], 1);  /* enable */
    ASSERT_EQ_U(ctrs[2], 1);  /* disable */
    ASSERT_EQ_U(ctrs[3], 1);  /* destroy */
    free(s);
}

TEST(vfs_simple_open_forwards_path_and_flags_to_driver)
{
    struct fixture fx;
    fixture_setup(&fx);

    void *file = NULL;
    int rc = vfs_simple_vfs_ops.open(fx.vfs_state, "/hello",
                                     NX_VFS_OPEN_READ | NX_VFS_OPEN_WRITE |
                                     NX_VFS_OPEN_CREATE, &file);
    ASSERT_EQ_U(rc, NX_OK);
    ASSERT_EQ_U(fx.fake_state.open_calls, 1);
    ASSERT(strcmp(fx.fake_state.last_path, "/hello") == 0);
    ASSERT_EQ_U(fx.fake_state.last_flags,
                NX_VFS_OPEN_READ | NX_VFS_OPEN_WRITE | NX_VFS_OPEN_CREATE);

    vfs_simple_vfs_ops.close(fx.vfs_state, file);
    ASSERT_EQ_U(fx.fake_state.close_calls, 1);
    fixture_teardown(&fx);
}

TEST(vfs_simple_write_then_read_forwards_byte_counts)
{
    struct fixture fx;
    fixture_setup(&fx);

    void *f = NULL;
    ASSERT_EQ_U(vfs_simple_vfs_ops.open(fx.vfs_state, "/x",
                                        NX_VFS_OPEN_READ | NX_VFS_OPEN_WRITE |
                                        NX_VFS_OPEN_CREATE, &f),
                NX_OK);

    const char *payload = "world";
    int64_t wrote = vfs_simple_vfs_ops.write(fx.vfs_state, f, payload, 5);
    ASSERT_EQ_U((uint64_t)wrote, 5);
    ASSERT_EQ_U(fx.fake_state.write_calls, 1);

    char buf[8] = {0};
    int64_t got = vfs_simple_vfs_ops.read(fx.vfs_state, f, buf, sizeof buf);
    ASSERT_EQ_U((uint64_t)got, 5);
    ASSERT_EQ_U(fx.fake_state.read_calls, 1);
    ASSERT(memcmp(buf, payload, 5) == 0);

    vfs_simple_vfs_ops.close(fx.vfs_state, f);
    fixture_teardown(&fx);
}

TEST(vfs_simple_relative_path_rejected_before_driver_call)
{
    struct fixture fx;
    fixture_setup(&fx);

    void *f = (void *)0xdead;
    int rc = vfs_simple_vfs_ops.open(fx.vfs_state, "hello",
                                     NX_VFS_OPEN_READ | NX_VFS_OPEN_CREATE,
                                     &f);
    ASSERT_EQ_U(rc, NX_EINVAL);
    ASSERT_EQ_U(fx.fake_state.open_calls, 0);
    fixture_teardown(&fx);
}

TEST(vfs_simple_null_path_returns_einval)
{
    struct fixture fx;
    fixture_setup(&fx);

    void *f = NULL;
    ASSERT_EQ_U(vfs_simple_vfs_ops.open(fx.vfs_state, NULL,
                                        NX_VFS_OPEN_READ, &f),
                NX_EINVAL);
    ASSERT_EQ_U(fx.fake_state.open_calls, 0);
    fixture_teardown(&fx);
}

TEST(vfs_simple_without_mounted_fs_returns_enoent)
{
    /* Register vfs_simple + its slot but NO filesystem.root.  Opens
     * must bounce back with NX_ENOENT — the slot-resolve step fails
     * before any driver is touched. */
    nx_graph_reset();
    void *vstate = calloc(1, vfs_simple_descriptor.state_size);
    ASSERT_NOT_NULL(vstate);
    ASSERT_EQ_U(vfs_simple_component_ops.init(vstate), NX_OK);

    void *f = NULL;
    ASSERT_EQ_U(vfs_simple_vfs_ops.open(vstate, "/x",
                                        NX_VFS_OPEN_READ | NX_VFS_OPEN_CREATE,
                                        &f),
                NX_ENOENT);
    vfs_simple_component_ops.destroy(vstate);
    free(vstate);
}

TEST(vfs_simple_resolves_slot_fresh_on_each_call)
{
    /* Prove the late-binding property: unbind the slot mid-run, and
     * the next op fails even though an earlier op on the same handle
     * succeeded.  This is the primitive future hot-swap depends on. */
    struct fixture fx;
    fixture_setup(&fx);

    void *f = NULL;
    ASSERT_EQ_U(vfs_simple_vfs_ops.open(fx.vfs_state, "/a",
                                        NX_VFS_OPEN_READ | NX_VFS_OPEN_WRITE |
                                        NX_VFS_OPEN_CREATE, &f),
                NX_OK);
    ASSERT_EQ_U((uint64_t)vfs_simple_vfs_ops.write(fx.vfs_state, f, "z", 1),
                1);

    /* Now clear the slot binding.  Subsequent reads/writes must
     * observe NX_ENOENT from vfs_simple's resolve step. */
    ASSERT_EQ_U(nx_slot_swap(&fx.root_slot, NULL), NX_OK);

    char buf[4];
    ASSERT_EQ_U((uint64_t)vfs_simple_vfs_ops.read(fx.vfs_state, f, buf, 4),
                (uint64_t)NX_ENOENT);
    fixture_teardown(&fx);
}
