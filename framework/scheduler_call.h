/*
 * GENERATED — DO NOT EDIT.
 * Source: interfaces/idl/scheduler.json
 * Generator: tools/gen-iface.py
 */

#ifndef NONUX_FRAMEWORK_SCHEDULER_CALL_H
#define NONUX_FRAMEWORK_SCHEDULER_CALL_H

#include <stddef.h>
#include <stdint.h>

#include "interfaces/scheduler.h"
#include "interfaces/scheduler_msg.h"
#include "framework/registry.h"
#include "framework/ipc.h"

/* Slice 8.0a defines `nx_slot_call_blocking` in
 * framework/slot_call.{h,c}; until that lands these wrappers
 * reference it as extern. */
extern int nx_slot_call_blocking(struct nx_slot *slot,
                                 struct nx_ipc_message *msg);

struct nx_task *nx_scheduler_pick_next(struct nx_slot *slot);
int nx_scheduler_enqueue(struct nx_slot *slot, struct nx_task *task);
int nx_scheduler_dequeue(struct nx_slot *slot, struct nx_task *task);
void nx_scheduler_yield(struct nx_slot *slot);
int nx_scheduler_set_priority(struct nx_slot *slot, struct nx_task *task,
                              int priority);
void nx_scheduler_tick(struct nx_slot *slot);

#endif /* NONUX_FRAMEWORK_SCHEDULER_CALL_H */
