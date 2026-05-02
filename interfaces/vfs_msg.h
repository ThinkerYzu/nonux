/*
 * GENERATED — DO NOT EDIT.
 * Source: interfaces/idl/vfs.json
 * Generator: tools/gen-iface.py
 */

#ifndef NONUX_INTERFACE_VFS_MSG_H
#define NONUX_INTERFACE_VFS_MSG_H

#include <stddef.h>
#include <stdint.h>
#include "interfaces/fs_types.h"

/* Op-IDs for `vfs` interface.  Stable across versions; removed ops
 * leave their slot as a gravestone — never reuse a freed id. */
enum nx_vfs_op_id {
    NX_VFS_OP_OPEN = 1,
    NX_VFS_OP_CLOSE = 2,
    NX_VFS_OP_RETAIN = 3,
    NX_VFS_OP_READ = 4,
    NX_VFS_OP_WRITE = 5,
    NX_VFS_OP_SEEK = 6,
    NX_VFS_OP_READDIR = 7,
    NX_VFS_OP_MKDIR = 8,
    NX_VFS_OP_STAT = 9,
};

/* Request: nx_vfs_open() — Open `path` (absolute, rooted at `/`).  Resolves the mount and */
struct nx_vfs_msg_open {
    char path[128];
    uint32_t flags;
    uint64_t out_file;
};

struct nx_vfs_reply_open {
    int rc;
    uint64_t out_file;
};

/* Request: nx_vfs_close() — Release per-open state.  See `nx_fs_ops.close`. */
struct nx_vfs_msg_close {
    uint64_t file;
};

struct nx_vfs_reply_close {
    int rc; /* always NX_OK; placeholder for void ops */
};

/* Request: nx_vfs_retain() — Retain (bump refcount on) per-open state — slice 7.6d.N.8. */
struct nx_vfs_msg_retain {
    uint64_t file;
};

struct nx_vfs_reply_retain {
    int rc; /* always NX_OK; placeholder for void ops */
};

/* Request: nx_vfs_read() — Read / write delegate to the driver's ops.  See `nx_fs_ops.read */
struct nx_vfs_msg_read {
    uint64_t file;
    uint64_t buf; /* void * encoded as u64 */
    size_t cap;
};

struct nx_vfs_reply_read {
    int64_t rc;
    size_t bytes_actual;
};

struct nx_vfs_msg_write {
    uint64_t file;
    uint64_t buf; /* const void * encoded as u64 */
    size_t len;
};

struct nx_vfs_reply_write {
    int64_t rc;
    size_t bytes_actual;
};

/* Request: nx_vfs_seek() — Reposition a per-open cursor (slice 6.4).  See `nx_fs_ops.seek` */
struct nx_vfs_msg_seek {
    uint64_t file;
    int64_t offset;
    int whence;
};

struct nx_vfs_reply_seek {
    int64_t rc;
    size_t bytes_actual;
};

/* Request: nx_vfs_readdir() — Enumerate the immediate children of `dir_path` (slice 7.7b.1). */
struct nx_vfs_msg_readdir {
    char dir_path[128];
    uint32_t cookie;
    struct nx_fs_dirent out;
};

struct nx_vfs_reply_readdir {
    int rc;
    uint32_t cookie;
    struct nx_fs_dirent out;
};

/* Request: nx_vfs_mkdir() — Create a directory (slice 7.7b.1).  See `nx_fs_ops.mkdir`. */
struct nx_vfs_msg_mkdir {
    char path[128];
};

struct nx_vfs_reply_mkdir {
    int rc;
};

/* Request: nx_vfs_stat() — Report metadata for `path` (slice 7.7b.1).  See `nx_fs_ops.stat`. */
struct nx_vfs_msg_stat {
    char path[128];
    struct nx_fs_stat out;
};

struct nx_vfs_reply_stat {
    int rc;
    struct nx_fs_stat out;
};

#endif /* NONUX_INTERFACE_VFS_MSG_H */
