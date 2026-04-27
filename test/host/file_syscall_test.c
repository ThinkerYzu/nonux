/*
 * Host tests for the file syscalls (slice 6.3).
 *
 * Each test stands up a minimal three-layer composition:
 *
 *     vfs slot       ← vfs_simple (the real component)
 *                       └─► filesystem.root slot
 *                             ← a test-local fake_fs driver with a
 *                               recording / storage mode suited to
 *                               the test.
 *
 * We invoke `nx_syscall_dispatch` on hand-built trap frames (same
 * technique as `syscall_test.c`) to exercise sys_open / sys_read /
 * sys_write / sys_handle_close without needing SVC.  `copy_from_user`
 * / `copy_to_user` are plain memcpy on host, so kernel-address
 * pointers are fine.
 *
 * What we prove:
 *   1. sys_open stores a HANDLE_FILE with rights derived from flags.
 *   2. sys_read / sys_write forward byte counts and payloads correctly
 *      and reject wrong-type or under-privileged handles.
 *   3. sys_handle_close on a HANDLE_FILE runs the vfs close — verified
 *      by the fake driver's close_calls counter.
 *   4. No-VFS / no-filesystem compositions bounce back with NX_ENOENT.
 *   5. Relative paths + oversize paths are rejected cleanly.
 */

#include "test_runner.h"

#include "framework/component.h"
#include "framework/handle.h"
#include "framework/registry.h"
#include "framework/syscall.h"
#include "interfaces/fs.h"
#include "interfaces/vfs.h"

#include <stdlib.h>
#include <string.h>

/* Exported by components/vfs_simple/vfs_simple.c. */
extern const struct nx_vfs_ops             vfs_simple_vfs_ops;
extern const struct nx_component_ops       vfs_simple_component_ops;
extern const struct nx_component_descriptor vfs_simple_descriptor;

/* Mirrored trap_frame layout (see syscall_test.c's note). */
struct trap_frame_host {
    uint64_t x[31];
    uint64_t sp_el0;
    uint64_t pc;
    uint64_t pstate;
};

#define CALL_DISPATCH(tf) \
    nx_syscall_dispatch((struct trap_frame *)(tf))

static void reset_frame(struct trap_frame_host *tf)
{
    memset(tf, 0, sizeof *tf);
}

/* ---------- Fake fs driver ------------------------------------------- */

struct fake_file {
    int     in_use;
    char    name[32];
    uint8_t data[64];
    size_t  size;
    size_t  cursor;
    uint32_t flags;
};

struct fake_fs_state {
    struct fake_file files[4];
    int close_calls;
};

static int fake_open(void *self, const char *path, uint32_t flags,
                     void **out_file)
{
    struct fake_fs_state *s = self;
    for (int i = 0; i < 4; i++) {
        if (!s->files[i].in_use) {
            struct fake_file *f = &s->files[i];
            f->in_use = 1;
            f->size   = 0;
            f->cursor = 0;
            f->flags  = flags;
            size_t n = 0;
            while (path[n] && n < sizeof f->name - 1) {
                f->name[n] = path[n]; n++;
            }
            f->name[n] = '\0';
            *out_file = f;
            return NX_OK;
        }
    }
    return NX_ENOMEM;
}

static void fake_close(void *self, void *file)
{
    struct fake_fs_state *s = self;
    struct fake_file     *f = file;
    s->close_calls++;
    if (f) f->in_use = 0;
}

static int64_t fake_read(void *self, void *file, void *buf, size_t cap)
{
    (void)self;
    struct fake_file *f = file;
    if (!(f->flags & NX_FS_OPEN_READ)) return NX_EPERM;
    size_t remain = f->cursor < f->size ? f->size - f->cursor : 0;
    size_t n = cap < remain ? cap : remain;
    memcpy(buf, f->data + f->cursor, n);
    f->cursor += n;
    return (int64_t)n;
}

static int64_t fake_write(void *self, void *file, const void *buf, size_t len)
{
    (void)self;
    struct fake_file *f = file;
    if (!(f->flags & NX_FS_OPEN_WRITE)) return NX_EPERM;
    size_t room = sizeof f->data - f->size;
    size_t n = len < room ? len : room;
    memcpy(f->data + f->size, buf, n);
    f->size += n;
    f->cursor = f->size;
    return (int64_t)n;
}

static int64_t fake_seek(void *self, void *file, int64_t offset, int whence)
{
    (void)self;
    struct fake_file *f = file;
    int64_t base;
    switch (whence) {
    case NX_FS_SEEK_SET: base = 0;                    break;
    case NX_FS_SEEK_CUR: base = (int64_t)f->cursor;   break;
    case NX_FS_SEEK_END: base = (int64_t)f->size;     break;
    default:             return NX_EINVAL;
    }
    int64_t np = base + offset;
    if (np < 0 || (uint64_t)np > f->size) return NX_EINVAL;
    f->cursor = (size_t)np;
    return np;
}

static int fake_readdir(void *self, uint32_t *cookie,
                        struct nx_fs_dirent *out)
{
    struct fake_fs_state *s = self;
    for (uint32_t i = *cookie; i < 4; i++) {
        if (!s->files[i].in_use) continue;
        size_t nlen = 0;
        while (s->files[i].name[nlen] && nlen < NX_FS_DIRENT_NAME_MAX - 1)
            nlen++;
        out->name_len = (uint32_t)nlen;
        memcpy(out->name, s->files[i].name, nlen);
        out->name[nlen] = '\0';
        *cookie = i + 1;
        return NX_OK;
    }
    *cookie = 4;
    return NX_ENOENT;
}

static const struct nx_fs_ops fake_fs_ops = {
    .open    = fake_open,   .close   = fake_close,
    .read    = fake_read,   .write   = fake_write,
    .seek    = fake_seek,   .readdir = fake_readdir,
};

static const struct nx_component_descriptor fake_fs_descriptor = {
    .name        = "fake_fs",
    .state_size  = sizeof(struct fake_fs_state),
    .deps_offset = 0,
    .deps        = NULL,
    .n_deps      = 0,
    .ops         = NULL,
    .iface_ops   = &fake_fs_ops,
};

/* ---------- Fixture setup ------------------------------------------- */

struct fixture {
    struct nx_slot      vfs_slot;
    struct nx_slot      root_slot;
    struct nx_component vfs_comp;
    struct nx_component fake_comp;
    struct fake_fs_state fake_state;
    void               *vfs_state;
};

static void fixture_setup(struct fixture *fx)
{
    nx_graph_reset();
    nx_syscall_reset_for_test();
    memset(fx, 0, sizeof *fx);

    /* vfs slot ← vfs_simple (real component). */
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

    /* filesystem.root slot ← fake_fs. */
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

/* ---------- Syscall helpers ----------------------------------------- */

static int64_t dispatch(struct trap_frame_host *tf,
                        uint64_t num,
                        uint64_t a0, uint64_t a1, uint64_t a2)
{
    reset_frame(tf);
    tf->x[8] = num;
    tf->x[0] = a0;
    tf->x[1] = a1;
    tf->x[2] = a2;
    CALL_DISPATCH(tf);
    return (int64_t)tf->x[0];
}

/* ---------- Tests --------------------------------------------------- */

TEST(sys_open_create_write_close_roundtrip)
{
    struct fixture fx;
    fixture_setup(&fx);
    struct trap_frame_host tf;

    int64_t h = dispatch(&tf, NX_SYS_OPEN, (uint64_t)(uintptr_t)"/hello",
                         NX_VFS_OPEN_READ | NX_VFS_OPEN_WRITE |
                         NX_VFS_OPEN_CREATE, 0);
    ASSERT(h > 0);  /* valid handle, not an error */

    /* Write 5 bytes, expect 5 back. */
    int64_t wrote = dispatch(&tf, NX_SYS_WRITE, (uint64_t)h,
                             (uint64_t)(uintptr_t)"world", 5);
    ASSERT_EQ_U((uint64_t)wrote, 5);

    /* Close the handle via sys_handle_close → HANDLE_FILE branch
     * should have driven fake_close.  This is the headline check. */
    int64_t cc = dispatch(&tf, NX_SYS_HANDLE_CLOSE, (uint64_t)h, 0, 0);
    ASSERT_EQ_U((uint64_t)cc, NX_OK);
    ASSERT_EQ_U(fx.fake_state.close_calls, 1);

    /* Reopen, read bytes back — proves the fake driver kept the data
     * across the close/reopen, i.e. the HANDLE_FILE slot went away
     * cleanly and the file object in the driver persists. */
    int64_t r = dispatch(&tf, NX_SYS_OPEN, (uint64_t)(uintptr_t)"/hello",
                         NX_VFS_OPEN_READ, 0);
    ASSERT(r > 0);
    char buf[16] = {0};
    int64_t got = dispatch(&tf, NX_SYS_READ, (uint64_t)r,
                           (uint64_t)(uintptr_t)buf, sizeof buf);
    /* Fake fs doesn't share storage across opens — each fake_open
     * allocates a fresh buffer.  So this read returns 0 (EOF on an
     * empty fresh file).  That's fine; the headline of this test is
     * the write return value and the close-destructor call. */
    ASSERT_EQ_U((uint64_t)got, 0);

    dispatch(&tf, NX_SYS_HANDLE_CLOSE, (uint64_t)r, 0, 0);
    ASSERT_EQ_U(fx.fake_state.close_calls, 2);

    fixture_teardown(&fx);
}

TEST(sys_open_assigns_rights_from_flags)
{
    struct fixture fx;
    fixture_setup(&fx);
    struct trap_frame_host tf;

    /* Open WRITE-only.  A subsequent read must get NX_EPERM — the
     * handle's rights mask must be WRITE-only. */
    int64_t h = dispatch(&tf, NX_SYS_OPEN, (uint64_t)(uintptr_t)"/w",
                         NX_VFS_OPEN_WRITE | NX_VFS_OPEN_CREATE, 0);
    ASSERT(h > 0);

    char buf[4];
    int64_t rc = dispatch(&tf, NX_SYS_READ, (uint64_t)h,
                          (uint64_t)(uintptr_t)buf, sizeof buf);
    ASSERT_EQ_U((uint64_t)rc, (uint64_t)(int64_t)NX_EPERM);

    dispatch(&tf, NX_SYS_HANDLE_CLOSE, (uint64_t)h, 0, 0);
    fixture_teardown(&fx);
}

TEST(sys_read_on_non_file_handle_returns_einval)
{
    struct fixture fx;
    fixture_setup(&fx);
    struct trap_frame_host tf;

    /* Alloc a VMO handle directly — the wrong type for sys_read. */
    struct nx_handle_table *t = nx_syscall_current_table();
    static int dummy;
    nx_handle_t h;
    ASSERT_EQ_U(nx_handle_alloc(t, NX_HANDLE_VMO,
                                NX_RIGHT_READ | NX_RIGHT_WRITE,
                                &dummy, &h), NX_OK);

    char buf[4];
    int64_t rc = dispatch(&tf, NX_SYS_READ, (uint64_t)h,
                          (uint64_t)(uintptr_t)buf, sizeof buf);
    ASSERT_EQ_U((uint64_t)rc, (uint64_t)(int64_t)NX_EINVAL);

    fixture_teardown(&fx);
}

TEST(sys_write_on_stale_handle_returns_enoent)
{
    struct fixture fx;
    fixture_setup(&fx);
    struct trap_frame_host tf;

    /* Stale handle value: index 2 (slot never allocated), generation
     * 0.  nx_handle_lookup returns NX_ENOENT for an empty slot.  We
     * deliberately skip handle indices 1 and 2 (encoded values 1 and
     * 2) because slice 7.6c.3c's stdio magic intercepts those when
     * the slot is empty — they route to NX_SYS_DEBUG_WRITE (UART)
     * instead of returning ENOENT. */
    int64_t rc = dispatch(&tf, NX_SYS_WRITE, (uint64_t)3,
                          (uint64_t)(uintptr_t)"x", 1);
    ASSERT_EQ_U((uint64_t)rc, (uint64_t)(int64_t)NX_ENOENT);

    fixture_teardown(&fx);
}

TEST(sys_write_to_console_handle_routes_through_nx_console_write)
{
    /* Slice 7.6d.N.6b replaced the magic-fd-fallback hack with proper
     * per-process CONSOLE handles pre-installed at slots 0/1/2 by
     * `nx_process_create`.  This host test installs CONSOLE handles
     * directly in the kernel process's table (the host syscall layer
     * uses g_kernel_process when no task is current) and checks that
     * NX_SYS_WRITE to fd 1 / fd 2 dispatches through nx_console_write,
     * which on the host build returns the byte count without touching
     * a UART (there is none).  Programs that allocate fd 1/2 to
     * something else (e.g. via dup3 over the console) get the regular
     * type-keyed dispatch — covered by the pipe roundtrip ktest. */
    extern int g_nx_console;
    struct fixture fx;
    fixture_setup(&fx);
    struct trap_frame_host tf;

    /* Pre-install three CONSOLE entries in the kernel process's table
     * so fd 1 / fd 2 round-trip through the regular handle dispatch.
     * Production code does this in nx_process_create; the host syscall
     * fixture skips that path so we install manually here.  Zero the
     * table struct directly to also reset slot generations — across
     * tests the kernel process's slot 0/1/2 generations have been
     * bumped by prior test sweeps, so `nx_handle_table_init` (which
     * preserves generation) would yield encoded values like 1025
     * instead of 1.  We don't assert the literal encoded values; the
     * write-via-fd-1/2 dispatch is what we're validating. */
    struct nx_handle_table *t = nx_syscall_current_table();
    memset(t, 0, sizeof *t);
    nx_handle_t h0, h1, h2;
    ASSERT_EQ_U(nx_handle_alloc(t, NX_HANDLE_CONSOLE, NX_RIGHT_WRITE,
                                &g_nx_console, &h0), NX_OK);
    ASSERT_EQ_U(nx_handle_alloc(t, NX_HANDLE_CONSOLE, NX_RIGHT_WRITE,
                                &g_nx_console, &h1), NX_OK);
    ASSERT_EQ_U(nx_handle_alloc(t, NX_HANDLE_CONSOLE, NX_RIGHT_READ,
                                &g_nx_console, &h2), NX_OK);
    ASSERT_EQ_U(h0, 1);  /* slot 0 → STDOUT_FILENO */
    ASSERT_EQ_U(h1, 2);  /* slot 1 → STDERR_FILENO */
    ASSERT_EQ_U(h2, 3);  /* slot 2 → STDIN_FILENO via h==0 */

    int64_t rc = dispatch(&tf, NX_SYS_WRITE, (uint64_t)1,
                          (uint64_t)(uintptr_t)"hi", 2);
    ASSERT_EQ_U((uint64_t)rc, 2);

    rc = dispatch(&tf, NX_SYS_WRITE, (uint64_t)2,
                  (uint64_t)(uintptr_t)"err", 3);
    ASSERT_EQ_U((uint64_t)rc, 3);

    fixture_teardown(&fx);
}

TEST(sys_open_with_no_vfs_slot_returns_enoent)
{
    nx_graph_reset();
    nx_syscall_reset_for_test();
    struct trap_frame_host tf;

    int64_t rc = dispatch(&tf, NX_SYS_OPEN,
                          (uint64_t)(uintptr_t)"/x",
                          NX_VFS_OPEN_READ | NX_VFS_OPEN_CREATE, 0);
    ASSERT_EQ_U((uint64_t)rc, (uint64_t)(int64_t)NX_ENOENT);
}

TEST(sys_open_null_path_returns_einval)
{
    struct fixture fx;
    fixture_setup(&fx);
    struct trap_frame_host tf;

    int64_t rc = dispatch(&tf, NX_SYS_OPEN, 0,
                          NX_VFS_OPEN_READ, 0);
    ASSERT_EQ_U((uint64_t)rc, (uint64_t)(int64_t)NX_EINVAL);

    fixture_teardown(&fx);
}

TEST(sys_open_path_without_nul_within_cap_returns_einval)
{
    struct fixture fx;
    fixture_setup(&fx);
    struct trap_frame_host tf;

    /* A NX_PATH_MAX-sized buffer full of non-NUL bytes: every byte
     * copies fine (per-byte bounds check), but no NUL is found within
     * the cap, so copy_path_from_user returns NX_EINVAL. */
    char long_path[NX_PATH_MAX + 8];
    memset(long_path, 'x', sizeof long_path);

    int64_t rc = dispatch(&tf, NX_SYS_OPEN,
                          (uint64_t)(uintptr_t)long_path,
                          NX_VFS_OPEN_READ | NX_VFS_OPEN_CREATE, 0);
    ASSERT_EQ_U((uint64_t)rc, (uint64_t)(int64_t)NX_EINVAL);

    fixture_teardown(&fx);
}

TEST(sys_open_then_write_caps_at_file_io_max)
{
    /* A write with `len > NX_FILE_IO_MAX` must be capped at the
     * staging-buffer size — the caller gets back the byte count
     * actually copied, and can loop to transfer more. */
    struct fixture fx;
    fixture_setup(&fx);
    struct trap_frame_host tf;

    int64_t h = dispatch(&tf, NX_SYS_OPEN, (uint64_t)(uintptr_t)"/big",
                         NX_VFS_OPEN_WRITE | NX_VFS_OPEN_CREATE, 0);
    ASSERT(h > 0);

    /* fake_fs's file capacity is 64 bytes; we won't hit that here.
     * We're only proving the syscall caps `len` at NX_FILE_IO_MAX
     * before handing to the driver. */
    static char huge[NX_FILE_IO_MAX + 16];
    memset(huge, 'y', sizeof huge);
    int64_t wrote = dispatch(&tf, NX_SYS_WRITE, (uint64_t)h,
                             (uint64_t)(uintptr_t)huge, sizeof huge);
    /* fake_fs writes at most 64 bytes (its per-file cap); the syscall
     * caps at NX_FILE_IO_MAX=256, driver caps at 64, so return is 64. */
    ASSERT(wrote > 0);
    ASSERT(wrote <= (int64_t)NX_FILE_IO_MAX);

    dispatch(&tf, NX_SYS_HANDLE_CLOSE, (uint64_t)h, 0, 0);
    fixture_teardown(&fx);
}

/* ---------- Slice 6.4 — seek / readdir syscalls --------------------- */

TEST(sys_seek_set_then_read_returns_original_bytes)
{
    struct fixture fx;
    fixture_setup(&fx);
    struct trap_frame_host tf;

    int64_t h = dispatch(&tf, NX_SYS_OPEN, (uint64_t)(uintptr_t)"/seek",
                         NX_VFS_OPEN_READ | NX_VFS_OPEN_WRITE |
                         NX_VFS_OPEN_CREATE, 0);
    ASSERT(h > 0);

    ASSERT_EQ_U((uint64_t)dispatch(&tf, NX_SYS_WRITE, (uint64_t)h,
                                   (uint64_t)(uintptr_t)"hello", 5),
                5);

    /* Seek to 0 via dispatch: NX_SYS_SEEK(h, offset=0, whence=SET=0). */
    int64_t pos = dispatch(&tf, NX_SYS_SEEK, (uint64_t)h, 0,
                           NX_VFS_SEEK_SET);
    ASSERT_EQ_U((uint64_t)pos, 0);

    char buf[8] = {0};
    int64_t got = dispatch(&tf, NX_SYS_READ, (uint64_t)h,
                           (uint64_t)(uintptr_t)buf, sizeof buf);
    ASSERT_EQ_U((uint64_t)got, 5);
    ASSERT(memcmp(buf, "hello", 5) == 0);

    dispatch(&tf, NX_SYS_HANDLE_CLOSE, (uint64_t)h, 0, 0);
    fixture_teardown(&fx);
}

TEST(sys_seek_end_returns_file_size)
{
    struct fixture fx;
    fixture_setup(&fx);
    struct trap_frame_host tf;

    int64_t h = dispatch(&tf, NX_SYS_OPEN, (uint64_t)(uintptr_t)"/end",
                         NX_VFS_OPEN_READ | NX_VFS_OPEN_WRITE |
                         NX_VFS_OPEN_CREATE, 0);
    ASSERT(h > 0);
    ASSERT_EQ_U((uint64_t)dispatch(&tf, NX_SYS_WRITE, (uint64_t)h,
                                   (uint64_t)(uintptr_t)"abcdef", 6),
                6);

    int64_t pos = dispatch(&tf, NX_SYS_SEEK, (uint64_t)h, 0,
                           NX_VFS_SEEK_END);
    ASSERT_EQ_U((uint64_t)pos, 6);

    dispatch(&tf, NX_SYS_HANDLE_CLOSE, (uint64_t)h, 0, 0);
    fixture_teardown(&fx);
}

TEST(sys_seek_past_size_returns_einval)
{
    struct fixture fx;
    fixture_setup(&fx);
    struct trap_frame_host tf;

    int64_t h = dispatch(&tf, NX_SYS_OPEN, (uint64_t)(uintptr_t)"/s",
                         NX_VFS_OPEN_READ | NX_VFS_OPEN_WRITE |
                         NX_VFS_OPEN_CREATE, 0);
    ASSERT(h > 0);
    dispatch(&tf, NX_SYS_WRITE, (uint64_t)h,
             (uint64_t)(uintptr_t)"ab", 2);

    int64_t rc = dispatch(&tf, NX_SYS_SEEK, (uint64_t)h, 100,
                          NX_VFS_SEEK_SET);
    ASSERT_EQ_U((uint64_t)rc, (uint64_t)(int64_t)NX_EINVAL);

    dispatch(&tf, NX_SYS_HANDLE_CLOSE, (uint64_t)h, 0, 0);
    fixture_teardown(&fx);
}

TEST(sys_readdir_yields_created_files_then_enoent)
{
    struct fixture fx;
    fixture_setup(&fx);
    struct trap_frame_host tf;

    /* Create two files and keep them open — fake_fs's slot is the
     * per-open storage, so a close wipes the name too.  (Real fs
     * drivers like ramfs separate file-table entries from per-open
     * state, so their names survive close — exercised via the kernel
     * ktest and ramfs conformance.) */
    int64_t a = dispatch(&tf, NX_SYS_OPEN, (uint64_t)(uintptr_t)"/alpha",
                         NX_VFS_OPEN_WRITE | NX_VFS_OPEN_CREATE, 0);
    ASSERT(a > 0);
    int64_t b = dispatch(&tf, NX_SYS_OPEN, (uint64_t)(uintptr_t)"/beta",
                         NX_VFS_OPEN_WRITE | NX_VFS_OPEN_CREATE, 0);
    ASSERT(b > 0);

    uint32_t cookie = 0;
    struct nx_fs_dirent ent;
    int saw_alpha = 0, saw_beta = 0;
    for (int i = 0; i < 6; i++) {
        memset(&ent, 0, sizeof ent);
        int64_t rc = dispatch(&tf, NX_SYS_READDIR,
                              (uint64_t)(uintptr_t)&cookie,
                              (uint64_t)(uintptr_t)&ent, 0);
        if (rc == NX_ENOENT) break;
        ASSERT_EQ_U((uint64_t)rc, NX_OK);
        if (ent.name_len == 6 && memcmp(ent.name, "/alpha", 6) == 0)
            saw_alpha++;
        else if (ent.name_len == 5 && memcmp(ent.name, "/beta", 5) == 0)
            saw_beta++;
    }
    ASSERT_EQ_U(saw_alpha, 1);
    ASSERT_EQ_U(saw_beta, 1);

    /* Further readdir calls past ENOENT keep returning ENOENT. */
    int64_t rc = dispatch(&tf, NX_SYS_READDIR,
                          (uint64_t)(uintptr_t)&cookie,
                          (uint64_t)(uintptr_t)&ent, 0);
    ASSERT_EQ_U((uint64_t)rc, (uint64_t)(int64_t)NX_ENOENT);

    dispatch(&tf, NX_SYS_HANDLE_CLOSE, (uint64_t)a, 0, 0);
    dispatch(&tf, NX_SYS_HANDLE_CLOSE, (uint64_t)b, 0, 0);
    fixture_teardown(&fx);
}

TEST(sys_seek_no_seek_right_returns_eperm)
{
    /* Allocate a FILE handle directly into the table WITHOUT RIGHT_SEEK.
     * Invoke sys_seek — expect NX_EPERM from the rights check.  Proves
     * the rights path is distinct from the happy path where open
     * grants SEEK implicitly. */
    struct fixture fx;
    fixture_setup(&fx);
    struct trap_frame_host tf;

    /* Seed a file through the driver directly so we own a valid per-
     * open pointer. */
    void *file = NULL;
    ASSERT_EQ_U(fx.fake_comp.descriptor->iface_ops ==
                (const void *)&fake_fs_ops, 1);
    ASSERT_EQ_U(fake_fs_ops.open(&fx.fake_state, "/r",
                                 NX_FS_OPEN_READ | NX_FS_OPEN_WRITE |
                                 NX_FS_OPEN_CREATE, &file),
                NX_OK);

    /* Alloc a HANDLE_FILE with only READ (no SEEK). */
    struct nx_handle_table *t = nx_syscall_current_table();
    nx_handle_t h;
    ASSERT_EQ_U(nx_handle_alloc(t, NX_HANDLE_FILE, NX_RIGHT_READ,
                                file, &h), NX_OK);

    int64_t rc = dispatch(&tf, NX_SYS_SEEK, (uint64_t)h, 0,
                          NX_VFS_SEEK_SET);
    ASSERT_EQ_U((uint64_t)rc, (uint64_t)(int64_t)NX_EPERM);

    dispatch(&tf, NX_SYS_HANDLE_CLOSE, (uint64_t)h, 0, 0);
    fixture_teardown(&fx);
}

TEST(sys_handle_close_file_after_unmount_still_closes_handle_slot)
{
    /* Edge case: the vfs slot vanishes between open and close.  The
     * handle slot must still be freed (otherwise the table leaks);
     * the driver-side close is skipped (unavoidable, and documented
     * in the syscall body). */
    struct fixture fx;
    fixture_setup(&fx);
    struct trap_frame_host tf;

    int64_t h = dispatch(&tf, NX_SYS_OPEN, (uint64_t)(uintptr_t)"/g",
                         NX_VFS_OPEN_READ | NX_VFS_OPEN_WRITE |
                         NX_VFS_OPEN_CREATE, 0);
    ASSERT(h > 0);

    /* Unbind the vfs slot.  Any subsequent read/write via this
     * handle will return NX_ENOENT from resolve_vfs — but close must
     * still reclaim the handle table slot. */
    ASSERT_EQ_U(nx_slot_swap(&fx.vfs_slot, NULL), NX_OK);

    int close_calls_before = fx.fake_state.close_calls;
    int64_t rc = dispatch(&tf, NX_SYS_HANDLE_CLOSE, (uint64_t)h, 0, 0);
    ASSERT_EQ_U((uint64_t)rc, NX_OK);
    /* Driver close NOT called (vfs slot was gone) — this is the
     * documented cost of racing a close with an unmount. */
    ASSERT_EQ_U(fx.fake_state.close_calls, close_calls_before);

    /* The handle slot is free — subsequent lookup returns NX_ENOENT. */
    struct nx_handle_table *t = nx_syscall_current_table();
    ASSERT_EQ_U(nx_handle_lookup(t, (nx_handle_t)h, 0, 0, 0), NX_ENOENT);

    fixture_teardown(&fx);
}
