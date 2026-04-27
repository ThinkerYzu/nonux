#ifndef NONUX_INTERFACE_VFS_H
#define NONUX_INTERFACE_VFS_H

#include <stddef.h>
#include <stdint.h>

/*
 * Virtual filesystem interface — slice 6.2.
 *
 * `nx_vfs_ops` is the upper-layer API that the syscall entry point
 * (slice 6.3's `NX_SYS_OPEN / _READ / _WRITE`) calls into.  Bound
 * against the `vfs` slot; the live implementation is `vfs_simple`,
 * which delegates to whatever filesystem driver is active at the
 * mount point (currently always `/`, served by whatever implements
 * the `filesystem.root` slot).
 *
 * Shape parity with `interfaces/fs.h` is deliberate for v1.  The VFS
 * layer's job in slice 6.2 is a thin pass-through: path stripping +
 * driver dispatch with no semantic additions.  Slice 6.4 grows this
 * interface with `readdir` and `stat` — ops that the lower fs-driver
 * API also gets but with subtly different contracts (e.g. readdir at
 * the VFS layer iterates across mount boundaries; at the driver
 * layer it stops at the current filesystem's boundary).  Keeping the
 * two headers separate now means that divergence doesn't require a
 * header split later.
 *
 * Ownership / concurrency / error conventions match `interfaces/fs.h`
 * — see that header's contract.  The VFS layer forwards `nx_fs_ops`'s
 * NX_E* return codes unchanged.
 */

/* Open flags — same bit meanings as `NX_FS_OPEN_*` from fs.h.  Defined
 * here so consumers don't have to include fs.h just to call into the
 * VFS.  The VFS layer forwards them to the mounted driver unchanged. */
#define NX_VFS_OPEN_READ     (1U << 0)
#define NX_VFS_OPEN_WRITE    (1U << 1)
#define NX_VFS_OPEN_CREATE   (1U << 2)

/* Seek whence (slice 6.4) — same values as `NX_FS_SEEK_*`. */
#define NX_VFS_SEEK_SET      0
#define NX_VFS_SEEK_CUR      1
#define NX_VFS_SEEK_END      2

/* Forward-declare — full definition in interfaces/fs.h.  Readdir at
 * the VFS layer uses the same entry shape as the fs driver; a future
 * multi-mount VFS may wrap it with mount-path prefixing but the
 * caller-facing struct stays the same. */
struct nx_fs_dirent;

struct nx_vfs_ops {
    /*
     * Open `path` (absolute, rooted at `/`).  Resolves the mount and
     * dispatches to the driver's `nx_fs_ops.open` with the path
     * stripped of its mount prefix.  In v1 only `/` is mounted, so the
     * path passes through unchanged.
     *
     * Returns / errors: see `nx_fs_ops.open` — the VFS layer forwards
     * the driver's status unchanged (NX_OK, NX_ENOENT, NX_EPERM,
     * NX_ENOMEM, NX_EINVAL).  Additional VFS-specific status:
     *   NX_ENOENT  — no filesystem mounted at the resolved mount point.
     */
    int (*open)(void *self, const char *path, uint32_t flags,
                void **out_file);

    /* Release per-open state.  See `nx_fs_ops.close`. */
    void (*close)(void *self, void *file);

    /* Retain (bump refcount on) per-open state — slice 7.6d.N.8.
     * See `nx_fs_ops.retain`.  Forwards to the active mount's
     * driver. */
    void (*retain)(void *self, void *file);

    /* Read / write delegate to the driver's ops.  See `nx_fs_ops.read
     * / .write` for the byte-count return convention. */
    int64_t (*read)(void *self, void *file, void *buf, size_t cap);
    int64_t (*write)(void *self, void *file, const void *buf, size_t len);

    /*
     * Reposition a per-open cursor (slice 6.4).  See `nx_fs_ops.seek`
     * for the contract; v1 VFS forwards unchanged.
     */
    int64_t (*seek)(void *self, void *file, int64_t offset, int whence);

    /*
     * Enumerate the filesystem's flat namespace (slice 6.4).  See
     * `nx_fs_ops.readdir` for the contract; v1 VFS forwards unchanged.
     * A future multi-mount VFS will iterate across mount boundaries
     * here — the caller's cookie will then carry a (mount, fs-cookie)
     * pair rather than the raw driver cookie.
     */
    int (*readdir)(void *self, uint32_t *cookie, struct nx_fs_dirent *out);
};

#endif /* NONUX_INTERFACE_VFS_H */
