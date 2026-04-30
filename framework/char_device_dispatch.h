/*
 * GENERATED — DO NOT EDIT.
 * Source: interfaces/idl/char_device.json
 * Generator: tools/gen-iface.py
 */

#ifndef NONUX_FRAMEWORK_CHAR_DEVICE_DISPATCH_H
#define NONUX_FRAMEWORK_CHAR_DEVICE_DISPATCH_H

#include "framework/ipc.h"
#include "framework/registry.h"
#include "interfaces/char_device.h"
#include "interfaces/char_device_msg.h"

/* Receiver-side dispatch macro for the `char_device` interface.
 * The component supplies a static handle_msg function and
 * delegates to NX_CHAR_DEVICE_DISPATCH(self, ops, msg) which expands
 * into a switch over msg->msg_type that unpacks each request
 * struct and calls the matching op on `ops`. */
#define NX_CHAR_DEVICE_DISPATCH(self, ops, msg) \
    do { \
        switch ((enum nx_char_device_op_id)((msg)->msg_type)) { \
        case NX_CHAR_DEVICE_OP_WRITE: \
            /* impl: (ops)->write(self, ...) — see template body. */ \
            break; \
        case NX_CHAR_DEVICE_OP_RX_BYTE: \
            /* impl: (ops)->rx_byte(self, ...) — see template body. */ \
            break; \
        default: \
            /* unknown op — return NX_EINVAL via reply. */ \
            break; \
        } \
    } while (0)

#endif /* NONUX_FRAMEWORK_CHAR_DEVICE_DISPATCH_H */
