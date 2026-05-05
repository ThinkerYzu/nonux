/*
 * GENERATED — DO NOT EDIT.
 * Source: interfaces/idl/char_device.json
 * Generator: tools/gen-iface.py
 */

#ifndef NONUX_FRAMEWORK_CHAR_DEVICE_CALL_H
#define NONUX_FRAMEWORK_CHAR_DEVICE_CALL_H

#include <stddef.h>
#include <stdint.h>

#include "interfaces/char_device.h"
#include "interfaces/char_device_msg.h"
#include "framework/registry.h"
#include "framework/ipc.h"
#include "framework/slot_call.h"

int64_t nx_char_device_write(struct nx_slot *slot, const void *buf,
                             size_t len);
int64_t nx_char_device_read(struct nx_slot *slot, uint32_t id, void *buf,
                            size_t cap);
void nx_char_device_rx_byte(struct nx_slot *slot, uint8_t byte);

#endif /* NONUX_FRAMEWORK_CHAR_DEVICE_CALL_H */
