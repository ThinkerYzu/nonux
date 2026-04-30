/*
 * GENERATED — DO NOT EDIT.
 * Source: interfaces/idl/scheduler.json
 * Generator: tools/gen-iface.py
 */

#ifndef NONUX_FRAMEWORK_SCHEDULER_DISPATCH_H
#define NONUX_FRAMEWORK_SCHEDULER_DISPATCH_H

#include "framework/ipc.h"
#include "framework/registry.h"
#include "interfaces/scheduler.h"
#include "interfaces/scheduler_msg.h"

/* Receiver-side dispatch macro for the `scheduler` interface.
 * The component supplies a static handle_msg function and
 * delegates to NX_SCHEDULER_DISPATCH(self, ops, msg) which expands
 * into a switch over msg->msg_type that unpacks each request
 * struct and calls the matching op on `ops`. */
#define NX_SCHEDULER_DISPATCH(self, ops, msg) \
    do { \
        switch ((enum nx_scheduler_op_id)((msg)->msg_type)) { \
        case NX_SCHEDULER_OP_PICK_NEXT: \
            /* impl: (ops)->pick_next(self, ...) — see template body. */ \
            break; \
        case NX_SCHEDULER_OP_ENQUEUE: \
            /* impl: (ops)->enqueue(self, ...) — see template body. */ \
            break; \
        case NX_SCHEDULER_OP_DEQUEUE: \
            /* impl: (ops)->dequeue(self, ...) — see template body. */ \
            break; \
        case NX_SCHEDULER_OP_YIELD: \
            /* impl: (ops)->yield(self, ...) — see template body. */ \
            break; \
        case NX_SCHEDULER_OP_SET_PRIORITY: \
            /* impl: (ops)->set_priority(self, ...) — see template body. */ \
            break; \
        case NX_SCHEDULER_OP_TICK: \
            /* impl: (ops)->tick(self, ...) — see template body. */ \
            break; \
        default: \
            /* unknown op — return NX_EINVAL via reply. */ \
            break; \
        } \
    } while (0)

#endif /* NONUX_FRAMEWORK_SCHEDULER_DISPATCH_H */
