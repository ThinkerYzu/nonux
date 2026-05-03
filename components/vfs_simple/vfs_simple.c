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
 * Slice 7.7b.2 — per-open wrapper.
 *
 * vfs_simple now serves multiple mounts (root + procfs).  The opaque
 * `void *file` we hand back from open() can't simply be the driver's
 * per-open pointer any more, because subsequent close/read/write/seek/
 * retain calls only have that pointer to work with — they wouldn't
 * know which mount's op table to dispatch into.  So we wrap the
 * driver per-open in a small `vfs_simple_open` that remembers the
 * mount slot name alongside the per-open, and hand the wrapper out
 * to callers.
 *
 * The mount is recorded by *slot name* (a string-literal pointer like
 * "filesystem.root") rather than by cached ops/self pointers.  This
 * preserves the slot-as-late-binding-primitive property
 * (DESIGN §Slot-Based Indirection): every op re-resolves the slot
 * via `nx_slot_lookup`, so an `nx_slot_swap(slot, NULL)` mid-run
 * causes subsequent reads to fail cleanly with NX_ENOENT (proven by
 * the slice-3.x slot_swap_clears_handle test).  Caching the ops
 * pointer would break that — exactly what slice 7.7b.2's first
 * iteration tripped on.
 *
 * Refcount discipline.  Wrapper.refs and driver per-open refs move in
 * lockstep so a wrapper is freed exactly when its driver per-open is.
 * `open` returns refs=1 on both sides.  `retain` calls driver retain
 * (must be non-NULL — every filesystem driver bound under vfs_simple
 * implements it as of slice 7.7b.2) and bumps wrapper.refs.  `close`
 * always calls driver close (which decrements driver refs and frees
 * its slot at zero) and decrements wrapper.refs; the wrapper is
 * returned to the pool when refs reaches zero.  Callers MUST pair
 * every successful open / retain with exactly one close.
 *
 * Pool sized for v1's ramfs RAMFS_MAX_OPEN (= 96) plus headroom for
 * procfs's PROCFS_MAX_OPEN (= 8) plus retain duplicates.  108 is
 * comfortably above RAMFS_MAX_OPEN so a worst-case all-files-open mix
 * doesn't trip ENOMEM at the wrapper layer.  Static .bss cost is
 * trivial (~3.5 KB).
 */
#define VFS_SIMPLE_MAX_OPEN  108u

struct vfs_simple_open {
    int          in_use;
    int          refs;
    const char  *mount;       /* slot name; string literal, never freed */
    void        *driver_file;
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
    w->in_use      = 0;
    w->refs        = 0;
    w->mount       = NULL;
    w->driver_file = NULL;
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

static int vfs_simple_open(void *self, const char *path, uint32_t flags,
                           void **out_file)
{
    (void)self;
    if (!path) return NX_EINVAL;
    /* v1 only recognises absolute paths; an empty or relative path is
     * rejected before touching the driver. */
    if (path[0] != '/') return NX_EINVAL;

    const char *mount = mount_for_path(path);
    struct vfs_simple_open *w = vfs_simple_alloc_wrapper();
    if (!w) return NX_ENOMEM;

    /* nx_vfs_*_READ/_WRITE/_CREATE match nx_fs_*_READ/_WRITE/_CREATE bit-
     * for-bit (both headers define them as (1<<0) .. (1<<2)).  No
     * translation needed; pass the mask through. */
    void *driver_file = NULL;
    int rc = nx_fs_open(nx_slot_lookup(mount), path, flags, &driver_file);
    if (rc != NX_OK) { vfs_simple_free_wrapper(w); return rc; }

    w->refs        = 1;
    w->mount       = mount;
    w->driver_file = driver_file;
    *out_file      = w;
    return NX_OK;
}

static void vfs_simple_close(void *self, void *file)
{
    (void)self;
    if (!file) return;
    struct vfs_simple_open *w = file;
    /* Slot may have been swapped to NULL between open and close — in
     * which case nx_fs_close is a no-op (disp_resolve / HOST_RESOLVE_FS
     * returns early).  Drop the wrapper anyway so vfs_simple's pool
     * isn't permanently leaked.  Mirrors the existing "close after
     * unmount still closes the handle slot" host test. */
    nx_fs_close(nx_slot_lookup(w->mount), w->driver_file);
    if (w->refs > 0 && --w->refs > 0) return;
    vfs_simple_free_wrapper(w);
}

static void vfs_simple_retain(void *self, void *file)
{
    (void)self;
    if (!file) return;
    struct vfs_simple_open *w = file;
    struct nx_slot *fs_slot = nx_slot_lookup(w->mount);
    /* Guard: only bump wrapper.refs if the slot is still active so that
     * wrapper.refs stays in sync with the driver's per-open refcount.
     * Reading slot->active is legal here — vfs_simple runs on the
     * dispatcher thread (R8).  All bound drivers implement retain as of
     * slice 7.7b.2, so no ops->retain NULL check is needed. */
    if (!fs_slot || !fs_slot->active) return;
    nx_fs_retain(fs_slot, w->driver_file);
    w->refs++;
}

static int64_t vfs_simple_read(void *self, void *file, void *buf, size_t cap)
{
    (void)self;
    if (!file) return NX_EINVAL;
    struct vfs_simple_open *w = file;
    return nx_fs_read(nx_slot_lookup(w->mount), w->driver_file, buf, cap);
}

static int64_t vfs_simple_write(void *self, void *file, const void *buf,
                                size_t len)
{
    (void)self;
    if (!file) return NX_EINVAL;
    struct vfs_simple_open *w = file;
    return nx_fs_write(nx_slot_lookup(w->mount), w->driver_file, buf, len);
}

static int64_t vfs_simple_seek(void *self, void *file,
                               int64_t offset, int whence)
{
    (void)self;
    if (!file) return NX_EINVAL;
    struct vfs_simple_open *w = file;
    return nx_fs_seek(nx_slot_lookup(w->mount), w->driver_file, offset, whence);
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
