/*
 * GENERATED — DO NOT EDIT.
 * Source: interfaces/idl/vfs.json
 * Generator: tools/gen-iface.py
 */

#ifndef NONUX_FRAMEWORK_VFS_DISPATCH_H
#define NONUX_FRAMEWORK_VFS_DISPATCH_H

#include "framework/ipc.h"
#include "framework/registry.h"
#include "interfaces/vfs.h"
#include "interfaces/vfs_msg.h"

/* Receiver-side dispatch macro for the `vfs` interface.
 * The component supplies a static handle_msg function and
 * delegates to NX_VFS_DISPATCH(self, ops, msg) which expands
 * into a switch over msg->msg_type that unpacks each request
 * struct and calls the matching op on `ops`. */
#define NX_VFS_DISPATCH(self, ops, msg) \
    do { \
        switch ((enum nx_vfs_op_id)((msg)->msg_type)) { \
        case NX_VFS_OP_OPEN: \
            /* impl: (ops)->open(self, ...) — see template body. */ \
            break; \
        case NX_VFS_OP_CLOSE: \
            /* impl: (ops)->close(self, ...) — see template body. */ \
            break; \
        case NX_VFS_OP_RETAIN: \
            /* impl: (ops)->retain(self, ...) — see template body. */ \
            break; \
        case NX_VFS_OP_READ: \
            /* impl: (ops)->read(self, ...) — see template body. */ \
            break; \
        case NX_VFS_OP_WRITE: \
            /* impl: (ops)->write(self, ...) — see template body. */ \
            break; \
        case NX_VFS_OP_SEEK: \
            /* impl: (ops)->seek(self, ...) — see template body. */ \
            break; \
        case NX_VFS_OP_READDIR: \
            /* impl: (ops)->readdir(self, ...) — see template body. */ \
            break; \
        case NX_VFS_OP_MKDIR: \
            /* impl: (ops)->mkdir(self, ...) — see template body. */ \
            break; \
        case NX_VFS_OP_STAT: \
            /* impl: (ops)->stat(self, ...) — see template body. */ \
            break; \
        default: \
            /* unknown op — return NX_EINVAL via reply. */ \
            break; \
        } \
    } while (0)

#endif /* NONUX_FRAMEWORK_VFS_DISPATCH_H */
