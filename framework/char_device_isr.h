/*
 * GENERATED — DO NOT EDIT.
 * Source: interfaces/idl/char_device.json
 * Generator: tools/gen-iface.py
 */

#ifndef NONUX_FRAMEWORK_CHAR_DEVICE_ISR_H
#define NONUX_FRAMEWORK_CHAR_DEVICE_ISR_H

#include <stddef.h>
#include <stdint.h>

#include "interfaces/char_device.h"
#include "interfaces/char_device_msg.h"
#include "framework/registry.h"
#include "framework/ipc.h"

/* IRQ-entry helpers for the `char_device` interface.  Each op
 * declared `context: "irq"` in the IDL gets a `_from_irq`
 * wrapper that grabs a slot from a fixed-size pre-built
 * message pool, fills the per-op request fields, and hands
 * the message off to the framework dispatcher via
 * `nx_ipc_enqueue_from_irq` — bounded instructions, no
 * allocation, no caps (per IDL-SCHEMA.md §IRQ-Entry Ops). */
#define NX_CHAR_DEVICE_ISR_POOL_SIZE 32

/* Slice 8.0a defines the bodies + pool storage; until that
 * lands these wrappers reference the framework-side enqueue
 * helper as extern. */

void nx_char_device_rx_byte_from_irq(struct nx_slot *slot, uint8_t byte);

#endif /* NONUX_FRAMEWORK_CHAR_DEVICE_ISR_H */
