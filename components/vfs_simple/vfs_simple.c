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
 * op resolves the root filesystem by calling `nx_slot_lookup
 * ("filesystem.root")` at *call* time — never at init/enable — and
 * dereferences `slot->active->descriptor->iface_ops` for the driver's
 * `struct nx_fs_ops`.  This means:
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
#include "interfaces/vfs.h"
#include "interfaces/fs.h"

#include <stddef.h>
#include <stdint.h>

#if !__STDC_HOSTED__
#include "core/lib/lib.h"
#endif

struct vfs_simple_state {
    /* Lifecycle counters for test introspection — same shape as every
     * other component so ktest can prove each verb fired. */
    unsigned init_called;
    unsigned enable_called;
    unsigned disable_called;
    unsigned destroy_called;
};

/* ---------- Slot resolution helper ------------------------------------ */

/*
 * Resolve the `filesystem.root` slot to its active driver's ops + self.
 * Returns NX_OK on success with `*out_ops` / `*out_self` populated,
 * NX_ENOENT if the slot is unregistered or has no active binding (i.e.
 * no filesystem mounted at /), NX_EINVAL on a missing iface_ops pointer
 * (a malformed filesystem component that doesn't export nx_fs_ops).
 */
static int resolve_root_fs(const struct nx_fs_ops **out_ops, void **out_self)
{
    struct nx_slot *slot = nx_slot_lookup("filesystem.root");
    if (!slot || !slot->active) return NX_ENOENT;
    if (!slot->active->descriptor || !slot->active->descriptor->iface_ops)
        return NX_EINVAL;

    *out_ops  = (const struct nx_fs_ops *)slot->active->descriptor->iface_ops;
    *out_self = slot->active->impl;
    return NX_OK;
}

/* ---------- nx_vfs_ops — forward to the mounted driver --------------- */

static int vfs_simple_open(void *self, const char *path, uint32_t flags,
                           void **out_file)
{
    (void)self;
    if (!path) return NX_EINVAL;
    /* v1 only recognises absolute paths; an empty or relative path is
     * rejected before touching the driver.  The leading '/' is passed
     * through unchanged — ramfs's namespace is flat (slice 6.2) so the
     * slash becomes part of the filename.  Slice 6.4's directory
     * support will strip mount prefixes here. */
    if (path[0] != '/') return NX_EINVAL;

    const struct nx_fs_ops *ops; void *fs_self;
    int rc = resolve_root_fs(&ops, &fs_self);
    if (rc != NX_OK) return rc;

    /* nx_vfs_*_READ/_WRITE/_CREATE match nx_fs_*_READ/_WRITE/_CREATE bit-
     * for-bit (both headers define them as (1<<0) .. (1<<2)).  No
     * translation needed; pass the mask through. */
    return ops->open(fs_self, path, flags, out_file);
}

static void vfs_simple_close(void *self, void *file)
{
    (void)self;
    if (!file) return;
    const struct nx_fs_ops *ops; void *fs_self;
    if (resolve_root_fs(&ops, &fs_self) != NX_OK) return;
    ops->close(fs_self, file);
}

static int64_t vfs_simple_read(void *self, void *file, void *buf, size_t cap)
{
    (void)self;
    const struct nx_fs_ops *ops; void *fs_self;
    int rc = resolve_root_fs(&ops, &fs_self);
    if (rc != NX_OK) return rc;
    return ops->read(fs_self, file, buf, cap);
}

static int64_t vfs_simple_write(void *self, void *file, const void *buf,
                                size_t len)
{
    (void)self;
    const struct nx_fs_ops *ops; void *fs_self;
    int rc = resolve_root_fs(&ops, &fs_self);
    if (rc != NX_OK) return rc;
    return ops->write(fs_self, file, buf, len);
}

static int64_t vfs_simple_seek(void *self, void *file,
                               int64_t offset, int whence)
{
    (void)self;
    const struct nx_fs_ops *ops; void *fs_self;
    int rc = resolve_root_fs(&ops, &fs_self);
    if (rc != NX_OK) return rc;
    return ops->seek(fs_self, file, offset, whence);
}

static int vfs_simple_readdir(void *self, uint32_t *cookie,
                              struct nx_fs_dirent *out)
{
    (void)self;
    if (!cookie || !out) return NX_EINVAL;
    const struct nx_fs_ops *ops; void *fs_self;
    int rc = resolve_root_fs(&ops, &fs_self);
    if (rc != NX_OK) return rc;
    return ops->readdir(fs_self, cookie, out);
}

const struct nx_vfs_ops vfs_simple_vfs_ops = {
    .open    = vfs_simple_open,
    .close   = vfs_simple_close,
    .read    = vfs_simple_read,
    .write   = vfs_simple_write,
    .seek    = vfs_simple_seek,
    .readdir = vfs_simple_readdir,
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

const struct nx_component_ops vfs_simple_component_ops = {
    .init    = vfs_simple_init,
    .enable  = vfs_simple_enable,
    .disable = vfs_simple_disable,
    .destroy = vfs_simple_destroy,
    /* No pause_hook: spawns_threads is false in the manifest.
     * No handle_msg: vfs_simple is driven through iface_ops by the
     * syscall layer (slice 6.3), not via the IPC router. */
};

NX_COMPONENT_REGISTER_NO_DEPS_IFACE(vfs_simple,
                                    struct vfs_simple_state,
                                    &vfs_simple_component_ops,
                                    &vfs_simple_vfs_ops);
