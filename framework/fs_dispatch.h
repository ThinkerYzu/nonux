/*
 * GENERATED — DO NOT EDIT.
 * Source: interfaces/idl/fs.json
 * Generator: tools/gen-iface.py
 */

#ifndef NONUX_FRAMEWORK_FS_DISPATCH_H
#define NONUX_FRAMEWORK_FS_DISPATCH_H

#include "framework/ipc.h"
#include "framework/registry.h"
#include "interfaces/fs.h"
#include "interfaces/fs_msg.h"

/* Receiver-side dispatch macro for the `fs` interface.
 * The component supplies a static handle_msg function and
 * delegates to NX_FS_DISPATCH(self, ops, msg) which expands
 * into a switch over msg->msg_type that unpacks each request
 * struct and calls the matching op on `ops`. */
#define NX_FS_DISPATCH(self, ops, msg) \
    do { \
        switch ((enum nx_fs_op_id)((msg)->msg_type)) { \
        case NX_FS_OP_OPEN: \
            /* impl: (ops)->open(self, ...) — see template body. */ \
            break; \
        case NX_FS_OP_CLOSE: \
            /* impl: (ops)->close(self, ...) — see template body. */ \
            break; \
        case NX_FS_OP_RETAIN: \
            /* impl: (ops)->retain(self, ...) — see template body. */ \
            break; \
        case NX_FS_OP_READ: \
            /* impl: (ops)->read(self, ...) — see template body. */ \
            break; \
        case NX_FS_OP_WRITE: \
            /* impl: (ops)->write(self, ...) — see template body. */ \
            break; \
        case NX_FS_OP_SEEK: \
            /* impl: (ops)->seek(self, ...) — see template body. */ \
            break; \
        case NX_FS_OP_READDIR: \
            /* impl: (ops)->readdir(self, ...) — see template body. */ \
            break; \
        case NX_FS_OP_MKDIR: \
            /* impl: (ops)->mkdir(self, ...) — see template body. */ \
            break; \
        case NX_FS_OP_STAT: \
            /* impl: (ops)->stat(self, ...) — see template body. */ \
            break; \
        default: \
            /* unknown op — return NX_EINVAL via reply. */ \
            break; \
        } \
    } while (0)

#endif /* NONUX_FRAMEWORK_FS_DISPATCH_H */
