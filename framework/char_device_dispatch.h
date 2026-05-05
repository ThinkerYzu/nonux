/*
 * GENERATED — DO NOT EDIT.
 * Source: interfaces/idl/char_device.json
 * Generator: tools/gen-iface.py
 */

#ifndef NONUX_FRAMEWORK_CHAR_DEVICE_DISPATCH_H
#define NONUX_FRAMEWORK_CHAR_DEVICE_DISPATCH_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "framework/ipc.h"
#include "framework/registry.h"
#include "interfaces/char_device.h"
#include "interfaces/char_device_msg.h"

/* Receiver-side dispatch function for the `char_device` interface.
 * The component's handle_msg delegates to this function, which
 * switches on msg->msg_type, unpacks the per-op request struct,
 * calls the matching op on `ops`, writes the reply struct in-place
 * at msg->payload, and sets msg->reply_payload_len so the
 * dispatcher's build_reply picks up the full per-op output. */
static inline int nx_char_device_dispatch(
    void *self, const struct nx_char_device_ops *ops,
    struct nx_ipc_message *msg)
{
    switch ((enum nx_char_device_op_id)(msg->msg_type)) {
    case NX_CHAR_DEVICE_OP_WRITE: {
        struct nx_char_device_msg_write *_req =
            (struct nx_char_device_msg_write *)msg->payload;
        const void *_buf = (const void *)(uintptr_t)_req->buf;
        size_t _len = _req->len;
        int64_t _rc64 = ops->write(self, _buf, _len);
        int _rc = (int)_rc64;
        struct nx_char_device_reply_write *_r =
            (struct nx_char_device_reply_write *)msg->payload;
        _r->rc = _rc64;
        _r->bytes_actual = _rc64 > 0 ? (size_t)_rc64 : 0;
        msg->reply_payload_len = (uint32_t)sizeof *_r;
        return _rc;
    }
    case NX_CHAR_DEVICE_OP_READ: {
        struct nx_char_device_msg_read *_req =
            (struct nx_char_device_msg_read *)msg->payload;
        uint32_t _id = _req->id;
        void *_buf = (void *)(uintptr_t)_req->buf;
        size_t _cap = _req->cap;
        int64_t _rc64 = ops->read(self, _id, _buf, _cap);
        int _rc = (int)_rc64;
        struct nx_char_device_reply_read *_r =
            (struct nx_char_device_reply_read *)msg->payload;
        _r->rc = _rc64;
        _r->bytes_actual = _rc64 > 0 ? (size_t)_rc64 : 0;
        msg->reply_payload_len = (uint32_t)sizeof *_r;
        return _rc;
    }
    case NX_CHAR_DEVICE_OP_RX_BYTE: {
        struct nx_char_device_msg_rx_byte *_req =
            (struct nx_char_device_msg_rx_byte *)msg->payload;
        uint8_t _byte = _req->byte;
        ops->rx_byte(self, _byte);
        struct nx_char_device_reply_rx_byte *_r =
            (struct nx_char_device_reply_rx_byte *)msg->payload;
        _r->rc = 0;
        msg->reply_payload_len = (uint32_t)sizeof *_r;
        return 0;
    }
    default:
        return NX_EINVAL;
    }
}

#endif /* NONUX_FRAMEWORK_CHAR_DEVICE_DISPATCH_H */
