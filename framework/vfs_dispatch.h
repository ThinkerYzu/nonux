/*
 * GENERATED — DO NOT EDIT.
 * Source: interfaces/idl/vfs.json
 * Generator: tools/gen-iface.py
 */

#ifndef NONUX_FRAMEWORK_VFS_DISPATCH_H
#define NONUX_FRAMEWORK_VFS_DISPATCH_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "framework/ipc.h"
#include "framework/registry.h"
#include "interfaces/vfs.h"
#include "interfaces/vfs_msg.h"

/* Receiver-side dispatch function for the `vfs` interface.
 * The component's handle_msg delegates to this function, which
 * switches on msg->msg_type, unpacks the per-op request struct,
 * calls the matching op on `ops`, writes the reply struct in-place
 * at msg->payload, and sets msg->reply_payload_len so the
 * dispatcher's build_reply picks up the full per-op output. */
static inline int nx_vfs_dispatch(
    void *self, const struct nx_vfs_ops *ops,
    struct nx_ipc_message *msg)
{
    switch ((enum nx_vfs_op_id)(msg->msg_type)) {
    case NX_VFS_OP_OPEN: {
        struct nx_vfs_msg_open *_req =
            (struct nx_vfs_msg_open *)msg->payload;
        const char *_path = _req->path;
        uint32_t _flags = _req->flags;
        void *_out_file = NULL;
        int _rc = ops->open(self, _path, _flags, &_out_file);
        struct nx_vfs_reply_open *_r =
            (struct nx_vfs_reply_open *)msg->payload;
        _r->rc = _rc;
        _r->out_file = (uint64_t)(uintptr_t)_out_file;
        msg->reply_payload_len = (uint32_t)sizeof *_r;
        return _rc;
    }
    case NX_VFS_OP_CLOSE: {
        struct nx_vfs_msg_close *_req =
            (struct nx_vfs_msg_close *)msg->payload;
        void *_file = (void *)(uintptr_t)_req->file;
        ops->close(self, _file);
        struct nx_vfs_reply_close *_r =
            (struct nx_vfs_reply_close *)msg->payload;
        _r->rc = 0;
        msg->reply_payload_len = (uint32_t)sizeof *_r;
        return 0;
    }
    case NX_VFS_OP_RETAIN: {
        struct nx_vfs_msg_retain *_req =
            (struct nx_vfs_msg_retain *)msg->payload;
        void *_file = (void *)(uintptr_t)_req->file;
        ops->retain(self, _file);
        struct nx_vfs_reply_retain *_r =
            (struct nx_vfs_reply_retain *)msg->payload;
        _r->rc = 0;
        msg->reply_payload_len = (uint32_t)sizeof *_r;
        return 0;
    }
    case NX_VFS_OP_READ: {
        struct nx_vfs_msg_read *_req =
            (struct nx_vfs_msg_read *)msg->payload;
        void *_file = (void *)(uintptr_t)_req->file;
        void *_buf = (void *)(uintptr_t)_req->buf;
        size_t _cap = _req->cap;
        int64_t _rc64 = ops->read(self, _file, _buf, _cap);
        int _rc = (int)_rc64;
        struct nx_vfs_reply_read *_r =
            (struct nx_vfs_reply_read *)msg->payload;
        _r->rc = _rc64;
        _r->bytes_actual = _rc64 > 0 ? (size_t)_rc64 : 0;
        msg->reply_payload_len = (uint32_t)sizeof *_r;
        return _rc;
    }
    case NX_VFS_OP_WRITE: {
        struct nx_vfs_msg_write *_req =
            (struct nx_vfs_msg_write *)msg->payload;
        void *_file = (void *)(uintptr_t)_req->file;
        const void *_buf = (const void *)(uintptr_t)_req->buf;
        size_t _len = _req->len;
        int64_t _rc64 = ops->write(self, _file, _buf, _len);
        int _rc = (int)_rc64;
        struct nx_vfs_reply_write *_r =
            (struct nx_vfs_reply_write *)msg->payload;
        _r->rc = _rc64;
        _r->bytes_actual = _rc64 > 0 ? (size_t)_rc64 : 0;
        msg->reply_payload_len = (uint32_t)sizeof *_r;
        return _rc;
    }
    case NX_VFS_OP_SEEK: {
        struct nx_vfs_msg_seek *_req =
            (struct nx_vfs_msg_seek *)msg->payload;
        void *_file = (void *)(uintptr_t)_req->file;
        int64_t _offset = _req->offset;
        int _whence = _req->whence;
        int64_t _rc64 = ops->seek(self, _file, _offset, _whence);
        int _rc = (int)_rc64;
        struct nx_vfs_reply_seek *_r =
            (struct nx_vfs_reply_seek *)msg->payload;
        _r->rc = _rc64;
        _r->bytes_actual = _rc64 > 0 ? (size_t)_rc64 : 0;
        msg->reply_payload_len = (uint32_t)sizeof *_r;
        return _rc;
    }
    case NX_VFS_OP_READDIR: {
        struct nx_vfs_msg_readdir *_req =
            (struct nx_vfs_msg_readdir *)msg->payload;
        const char *_dir_path = _req->dir_path;
        uint32_t _cookie; memset(&_cookie, 0, sizeof(_cookie)); _cookie = _req->cookie;
        struct nx_fs_dirent _out; memset(&_out, 0, sizeof(_out));
        int _rc = ops->readdir(self, _dir_path, &_cookie, &_out);
        struct nx_vfs_reply_readdir *_r =
            (struct nx_vfs_reply_readdir *)msg->payload;
        _r->rc = _rc;
        _r->cookie = _cookie;
        _r->out = _out;
        msg->reply_payload_len = (uint32_t)sizeof *_r;
        return _rc;
    }
    case NX_VFS_OP_MKDIR: {
        struct nx_vfs_msg_mkdir *_req =
            (struct nx_vfs_msg_mkdir *)msg->payload;
        const char *_path = _req->path;
        int _rc = ops->mkdir(self, _path);
        struct nx_vfs_reply_mkdir *_r =
            (struct nx_vfs_reply_mkdir *)msg->payload;
        _r->rc = _rc;
        msg->reply_payload_len = (uint32_t)sizeof *_r;
        return _rc;
    }
    case NX_VFS_OP_STAT: {
        struct nx_vfs_msg_stat *_req =
            (struct nx_vfs_msg_stat *)msg->payload;
        const char *_path = _req->path;
        struct nx_fs_stat _out; memset(&_out, 0, sizeof(_out));
        int _rc = ops->stat(self, _path, &_out);
        struct nx_vfs_reply_stat *_r =
            (struct nx_vfs_reply_stat *)msg->payload;
        _r->rc = _rc;
        _r->out = _out;
        msg->reply_payload_len = (uint32_t)sizeof *_r;
        return _rc;
    }
    default:
        return NX_EINVAL;
    }
}

#endif /* NONUX_FRAMEWORK_VFS_DISPATCH_H */
