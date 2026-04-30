#ifndef NONUX_INTERFACE_FS_TYPES_H
#define NONUX_INTERFACE_FS_TYPES_H

#include <stddef.h>
#include <stdint.h>

/*
 * Data shapes shared between the fs-driver interface (interfaces/fs.h)
 * and the upper VFS interface (interfaces/vfs.h).  Hand-written; the
 * IDL-driven ops headers reference these via "includes:" rather than
 * forward-declaring or redefining them.  Slice 8.0pre.2.
 *
 * The constants on this side of the split are those tightly coupled to
 * the struct layouts: NX_FS_DIRENT_NAME_MAX is the array bound in
 * struct nx_fs_dirent; NX_FS_KIND_* is the discriminant in struct
 * nx_fs_stat.  Operational constants (NX_FS_OPEN_*, NX_FS_SEEK_*) live
 * in interfaces/fs.h alongside the ops table that consumes them.
 */

/*
 * A single directory entry.  Fixed-size `name[]` for v1 so readdir can
 * bulk-copy into user space without per-entry malloc; `name_len` is
 * the byte count excluding the trailing NUL.  Name is always NUL-
 * terminated even when `name_len == NX_FS_DIRENT_NAME_MAX - 1`.
 *
 * Slice 7.7b.1: readdir takes a `dir_path` argument and returns
 * basenames of immediate children of that directory.  Drivers that
 * stored hierarchical paths verbatim (e.g. ramfs's `/bin/busybox`)
 * project them to the segment immediately following `dir_path` and
 * deduplicate within a single iteration so each directory child is
 * yielded exactly once.
 */
#define NX_FS_DIRENT_NAME_MAX  64u

struct nx_fs_dirent {
    uint32_t name_len;
    char     name[NX_FS_DIRENT_NAME_MAX];
};

/*
 * Minimum metadata the syscall layer needs to distinguish a file from
 * a directory and report a size.  Filesystems may surface richer info
 * later; for v1 the shape is "kind + size" only — the syscall layer
 * synthesises owner/perms/timestamps when packing the Linux struct
 * stat for `sys_fstatat`.
 */
#define NX_FS_KIND_FILE   1u
#define NX_FS_KIND_DIR    2u

struct nx_fs_stat {
    uint32_t kind;     /* NX_FS_KIND_FILE or NX_FS_KIND_DIR */
    int64_t  size;     /* bytes; 0 for directories in v1 */
};

#endif /* NONUX_INTERFACE_FS_TYPES_H */
