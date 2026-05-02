/*
 * GENERATED — DO NOT EDIT.
 * Source: interfaces/idl/fs.json
 * Generator: tools/gen-iface.py
 */

#ifndef NONUX_INTERFACE_FS_MSG_H
#define NONUX_INTERFACE_FS_MSG_H

#include <stddef.h>
#include <stdint.h>
#include "interfaces/fs_types.h"

/* Op-IDs for `fs` interface.  Stable across versions; removed ops
 * leave their slot as a gravestone — never reuse a freed id. */
enum nx_fs_op_id {
    NX_FS_OP_OPEN = 1,
    NX_FS_OP_CLOSE = 2,
    NX_FS_OP_RETAIN = 3,
    NX_FS_OP_READ = 4,
    NX_FS_OP_WRITE = 5,
    NX_FS_OP_SEEK = 6,
    NX_FS_OP_READDIR = 7,
    NX_FS_OP_MKDIR = 8,
    NX_FS_OP_STAT = 9,
};

/* Request: nx_fs_open() — Open `path` on the driver instance `self`.  `flags` is a bitmask */
struct nx_fs_msg_open {
    char path[128];
    uint32_t flags;
    uint64_t out_file;
};

struct nx_fs_reply_open {
    int rc;
    uint64_t out_file;
};

/* Request: nx_fs_close() — Release per-open state returned by `open`.  Idempotent against */
struct nx_fs_msg_close {
    uint64_t file;
};

struct nx_fs_reply_close {
    int rc; /* always NX_OK; placeholder for void ops */
};

/* Request: nx_fs_retain() — Bump the per-open's reference count (slice 7.6d.N.8).  Used by */
struct nx_fs_msg_retain {
    uint64_t file;
};

struct nx_fs_reply_retain {
    int rc; /* always NX_OK; placeholder for void ops */
};

/* Request: nx_fs_read() — Read up to `cap` bytes from `file` into `buf`, starting at the */
struct nx_fs_msg_read {
    uint64_t file;
    uint64_t buf; /* void * encoded as u64 */
    size_t cap;
};

struct nx_fs_reply_read {
    int64_t rc;
    size_t bytes_actual;
};

/* Request: nx_fs_write() — Write `len` bytes from `buf` into `file` at the per-open cursor, */
struct nx_fs_msg_write {
    uint64_t file;
    uint64_t buf; /* const void * encoded as u64 */
    size_t len;
};

struct nx_fs_reply_write {
    int64_t rc;
    size_t bytes_actual;
};

/* Request: nx_fs_seek() — Reposition the per-open cursor (slice 6.4).  `whence` is one of */
struct nx_fs_msg_seek {
    uint64_t file;
    int64_t offset;
    int whence;
};

struct nx_fs_reply_seek {
    int64_t rc;
    size_t bytes_actual;
};

/* Request: nx_fs_readdir() — Read the next entry under `dir_path` (slice 7.7b.1). */
struct nx_fs_msg_readdir {
    char dir_path[128];
    uint32_t cookie;
    struct nx_fs_dirent out;
};

struct nx_fs_reply_readdir {
    int rc;
    uint32_t cookie;
    struct nx_fs_dirent out;
};

/* Request: nx_fs_mkdir() — Create a directory at `path` (slice 7.7b.1).  `path` is absolute. */
struct nx_fs_msg_mkdir {
    char path[128];
};

struct nx_fs_reply_mkdir {
    int rc;
};

/* Request: nx_fs_stat() — Report metadata for `path` (slice 7.7b.1).  Used by the syscall */
struct nx_fs_msg_stat {
    char path[128];
    struct nx_fs_stat out;
};

struct nx_fs_reply_stat {
    int rc;
    struct nx_fs_stat out;
};

#endif /* NONUX_INTERFACE_FS_MSG_H */
