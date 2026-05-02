/*
 * GENERATED — DO NOT EDIT.
 * Source: interfaces/idl/mm.json
 * Generator: tools/gen-iface.py
 */

#ifndef NONUX_FRAMEWORK_MM_CALL_H
#define NONUX_FRAMEWORK_MM_CALL_H

#include <stddef.h>
#include <stdint.h>

#include "interfaces/mm.h"
#include "interfaces/mm_msg.h"
#include "framework/registry.h"
#include "framework/ipc.h"
#include "framework/slot_call.h"

void *nx_mm_alloc_pages(struct nx_slot *slot, uint32_t order);
void nx_mm_free_pages(struct nx_slot *slot, void *ptr, uint32_t order);
size_t nx_mm_page_size(struct nx_slot *slot);
uint32_t nx_mm_max_order(struct nx_slot *slot);

#endif /* NONUX_FRAMEWORK_MM_CALL_H */
