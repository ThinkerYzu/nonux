/*
 * GENERATED — DO NOT EDIT.
 * Source: interfaces/idl/char_device.json
 * Generator: tools/gen-iface.py
 */

#ifndef NONUX_INTERFACE_CHAR_DEVICE_MSG_H
#define NONUX_INTERFACE_CHAR_DEVICE_MSG_H

#include <stddef.h>
#include <stdint.h>

/* Op-IDs for `char_device` interface.  Stable across versions; removed ops
 * leave their slot as a gravestone — never reuse a freed id. */
enum nx_char_device_op_id {
    NX_CHAR_DEVICE_OP_WRITE = 1,
    NX_CHAR_DEVICE_OP_RX_BYTE = 2,
};

/* Request: nx_char_device_write() — Write `len` bytes from `buf` to the device.  Blocking in v1 */
struct nx_char_device_msg_write {
    uint64_t buf; /* const void * encoded as u64 */
    size_t len;
};

struct nx_char_device_reply_write {
    int64_t rc;
    size_t bytes_actual;
};

/* Request: nx_char_device_rx_byte() — Deliver one received byte to the driver.  Called from ISR */
struct nx_char_device_msg_rx_byte {
    uint8_t byte;
};

struct nx_char_device_reply_rx_byte {
    int rc; /* always NX_OK; placeholder for void ops */
};

#endif /* NONUX_INTERFACE_CHAR_DEVICE_MSG_H */
