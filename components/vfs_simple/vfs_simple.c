/*
 * vfs_simple — single-mount VFS layer (slice 6.2).
 *
 * First real component bound to the `vfs` slot.  Implements the
 * `interfaces/vfs.h` contract that slice 6.3's `NX_SYS_OPEN / _READ /
 * _WRITE` syscalls dispatch through.  For v1 there is exactly one
 * mount — "/" — served by whatever component implements the
 * `filesystem.root` slot (ramfs in this slice; future tmpfs / ext2
 * swap in by editing kernel.json without touching vfs_simple).
 *
 * Slot-resolve discipline (DESIGN §Slot-Based Indirection).  Every
 * op resolves the target filesystem slot by calling `nx_slot_lookup`
 * at *call* time — never at init/enable — and forwards through the
 * `nx_fs_*` blocking-call wrappers (slice 8.0d), which in the kernel
 * build call the slot's handle_msg directly (sync dispatcher-to-
 * dispatcher).  This means:
 *
 *   - No init-order dependency on the bound driver.  The slot table
 *     is populated in bootstrap step 2 (slot binding), which runs
 *     before step 3 (lifecycle init/enable); by the time vfs_simple's
 *     first syscall lands in slice 6.3, both components are ACTIVE.
 *   - Future hot-swap of the root filesystem lands as a one-line
 *     `nx_slot_swap` — no stale pointers inside vfs_simple to
 *     invalidate.
 *   - The fact that vfs_simple has no required dep in its manifest is
 *     intentional: DI is for compile-time-checkable slot wiring (v0
 *     gen-config currently doesn't emit deps headers for in-tree
 *     components).  The slot itself is the late-binding primitive.
 *
 * Per-open state is the driver's opaque per-open pointer.  vfs_simple
 * stores no additional wrapper — the file handle round-trips straight
 * back to the driver on close/read/write.  When mount points and
 * path-stripping land (Phase 8+), vfs_simple will need a wrapper to
 * remember which mount a given open belongs to.
 */

#include "framework/component.h"
#include "framework/registry.h"
#include "framework/vfs_dispatch.h"
#include "framework/fs_call.h"
#include "interfaces/vfs.h"
#include "interfaces/fs.h"

#include <stddef.h>
#include <stdint.h>

#if !__STDC_HOSTED__
#include "core/lib/lib.h"
#endif

/*
 * Slice 9b.1 — per-open ID table.
 *
 * vfs_simple maintains its own internal open table (slice 9b.1).  The
 * `void *file` pointer API is replaced by a uint32_t open-ID returned
 * to callers.  vfs_simple maps ID → {mount_slot_name, driver_id} and
 * on each op re-resolves the slot by name (preserving the slot-as-
 * late-binding primitive: see DESIGN §Slot-Based Indirection).
 *
 * Slice 7.7b.2 per-open multi-mount support is preserved: the mount
 * slot name is still recorded alongside the driver ID.
 *
 * IDs are 1-based (0 = invalid/error).  The pool size matches the
 * previous wrapper pool.
 */
#define VFS_SIMPLE_MAX_OPEN  108u

struct vfs_simple_open {
    int          in_use;
    int          refs;
    const char  *mount;       /* slot name; string literal, never freed */
    uint32_t     driver_id;   /* ID returned by the underlying fs driver */
};

static struct vfs_simple_open g_vfs_simple_opens[VFS_SIMPLE_MAX_OPEN];

static struct vfs_simple_open *vfs_simple_alloc_wrapper(void)
{
    for (unsigned i = 0; i < VFS_SIMPLE_MAX_OPEN; i++) {
        if (!g_vfs_simple_opens[i].in_use) {
            g_vfs_simple_opens[i].in_use = 1;
            return &g_vfs_simple_opens[i];
        }
    }
    return NULL;
}

static void vfs_simple_free_wrapper(struct vfs_simple_open *w)
{
    w->in_use    = 0;
    w->refs      = 0;
    w->mount     = NULL;
    w->driver_id = 0;
}

/* Convert between the public uint32_t ID and the internal entry. */
static uint32_t vfs_simple_wrap_to_id(struct vfs_simple_open *w)
{
    return (uint32_t)(w - g_vfs_simple_opens) + 1;
}

static struct vfs_simple_open *vfs_simple_id_to_wrap(uint32_t id)
{
    if (id == 0 || id > VFS_SIMPLE_MAX_OPEN) return NULL;
    struct vfs_simple_open *w = &g_vfs_simple_opens[id - 1];
    return w->in_use ? w : NULL;
}

struct vfs_simple_state {
    /* Lifecycle counters for test introspection — same shape as every
     * other component so ktest can prove each verb fired. */
    unsigned init_called;
    unsigned enable_called;
    unsigned disable_called;
    unsigned destroy_called;
};

/* ---------- Mount table ----------------------------------------------- */

/*
 * Slice 7.7b.2 — mount table.  Returns the slot name responsible for
 * `path`.  v1 has exactly one non-root mount (`/proc` → procfs); the
 * dispatch is hard-coded rather than table-driven because (a) one
 * entry doesn't justify a configuration loop, and (b) the mount path
 * "/proc" is encoded into the procfs driver too (it recognises the
 * full path including the prefix), so introducing a table now would
 * just push the coupling around.  A second non-root mount is the
 * trigger for promoting this to a real mount-table struct + a
 * gen-config-driven binding list.
 *
 * Match rule: `path` is procfs's iff it equals "/proc" exactly, or
 * begins with "/proc/".  Everything else (including "/procX...") goes
 * to root.  The `slash_or_end` test prevents "/procfoo" from
 * accidentally routing to procfs.
 */
static const char *mount_for_path(const char *path)
{
    /* Match "/proc" */
    if (path[0] == '/' && path[1] == 'p' && path[2] == 'r' &&
        path[3] == 'o' && path[4] == 'c') {
        char tail = path[5];
        if (tail == '\0' || tail == '/') return "filesystem.proc";
    }
    return "filesystem.root";
}

/* ---------- nx_vfs_ops — forward to the mounted driver --------------- */

static uint32_t vfs_simple_open(void *self, const char *path, uint32_t flags)
{
    (void)self;
    if (!path) return 0;
    if (path[0] != '/') return 0;

    const char *mount = mount_for_path(path);
    struct vfs_simple_open *w = vfs_simple_alloc_wrapper();
    if (!w) return 0;

    uint32_t driver_id = nx_fs_open(nx_slot_lookup(mount), path, flags);
    if (driver_id == 0) { vfs_simple_free_wrapper(w); return 0; }

    w->refs      = 1;
    w->mount     = mount;
    w->driver_id = driver_id;
    return vfs_simple_wrap_to_id(w);
}

static void vfs_simple_close(void *self, uint32_t id)
{
    (void)self;
    if (id == 0) return;
    struct vfs_simple_open *w = vfs_simple_id_to_wrap(id);
    if (!w) return;
    /* Slot may have been swapped to NULL between open and close — in
     * which case nx_fs_close is a no-op (disp_resolve / HOST_RESOLVE_FS
     * returns early).  Drop the wrapper anyway so the pool isn't leaked. */
    nx_fs_close(nx_slot_lookup(w->mount), w->driver_id);
    if (w->refs > 0 && --w->refs > 0) return;
    vfs_simple_free_wrapper(w);
}

static void vfs_simple_retain(void *self, uint32_t id)
{
    (void)self;
    if (id == 0) return;
    struct vfs_simple_open *w = vfs_simple_id_to_wrap(id);
    if (!w) return;
    struct nx_slot *fs_slot = nx_slot_lookup(w->mount);
    if (!fs_slot || !fs_slot->active) return;
    nx_fs_retain(fs_slot, w->driver_id);
    w->refs++;
}

static int64_t vfs_simple_read(void *self, uint32_t id, void *buf, size_t cap)
{
    (void)self;
    struct vfs_simple_open *w = vfs_simple_id_to_wrap(id);
    if (!w) return NX_EINVAL;
    return nx_fs_read(nx_slot_lookup(w->mount), w->driver_id, buf, cap);
}

static int64_t vfs_simple_write(void *self, uint32_t id, const void *buf,
                                size_t len)
{
    (void)self;
    struct vfs_simple_open *w = vfs_simple_id_to_wrap(id);
    if (!w) return NX_EINVAL;
    return nx_fs_write(nx_slot_lookup(w->mount), w->driver_id, buf, len);
}

static int64_t vfs_simple_seek(void *self, uint32_t id,
                               int64_t offset, int whence)
{
    (void)self;
    struct vfs_simple_open *w = vfs_simple_id_to_wrap(id);
    if (!w) return NX_EINVAL;
    return nx_fs_seek(nx_slot_lookup(w->mount), w->driver_id, offset, whence);
}

static int vfs_simple_readdir(void *self, const char *dir_path,
                              uint32_t *cookie, struct nx_fs_dirent *out)
{
    (void)self;
    if (!dir_path || !cookie || !out) return NX_EINVAL;
    if (dir_path[0] != '/') return NX_EINVAL;
    return nx_fs_readdir(nx_slot_lookup(mount_for_path(dir_path)),
                         dir_path, cookie, out);
}

static int vfs_simple_mkdir(void *self, const char *path)
{
    (void)self;
    if (!path) return NX_EINVAL;
    if (path[0] != '/') return NX_EINVAL;
    return nx_fs_mkdir(nx_slot_lookup(mount_for_path(path)), path);
}

static int vfs_simple_stat(void *self, const char *path,
                           struct nx_fs_stat *out)
{
    (void)self;
    if (!path || !out) return NX_EINVAL;
    if (path[0] != '/') return NX_EINVAL;
    return nx_fs_stat(nx_slot_lookup(mount_for_path(path)), path, out);
}

const struct nx_vfs_ops vfs_simple_vfs_ops = {
    .open    = vfs_simple_open,
    .close   = vfs_simple_close,
    .retain  = vfs_simple_retain,
    .read    = vfs_simple_read,
    .write   = vfs_simple_write,
    .seek    = vfs_simple_seek,
    .readdir = vfs_simple_readdir,
    .mkdir   = vfs_simple_mkdir,
    .stat    = vfs_simple_stat,
};

/* ---------- Component lifecycle -------------------------------------- */

static int vfs_simple_init(void *self)
{
    struct vfs_simple_state *s = self;
    s->init_called++;
    return NX_OK;
}

static int vfs_simple_enable(void *self)
{
    struct vfs_simple_state *s = self;
    s->enable_called++;
    return NX_OK;
}

static int vfs_simple_disable(void *self)
{
    struct vfs_simple_state *s = self;
    s->disable_called++;
    return NX_OK;
}

static void vfs_simple_destroy(void *self)
{
    struct vfs_simple_state *s = self;
    s->destroy_called++;
}

static int vfs_simple_handle_msg(void *self, struct nx_ipc_message *msg)
{
    return nx_vfs_dispatch(self, &vfs_simple_vfs_ops, msg);
}

const struct nx_component_ops vfs_simple_component_ops = {
    .init       = vfs_simple_init,
    .enable     = vfs_simple_enable,
    .disable    = vfs_simple_disable,
    .destroy    = vfs_simple_destroy,
    .handle_msg = vfs_simple_handle_msg,
};

NX_COMPONENT_REGISTER_NO_DEPS_IFACE(vfs_simple,
                                    struct vfs_simple_state,
                                    &vfs_simple_component_ops,
                                    &vfs_simple_vfs_ops);
