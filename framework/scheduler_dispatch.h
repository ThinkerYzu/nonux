/*
 * GENERATED — DO NOT EDIT.
 * Source: interfaces/idl/scheduler.json
 * Generator: tools/gen-iface.py
 */

#ifndef NONUX_FRAMEWORK_SCHEDULER_DISPATCH_H
#define NONUX_FRAMEWORK_SCHEDULER_DISPATCH_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "framework/ipc.h"
#include "framework/registry.h"
#include "interfaces/scheduler.h"
#include "interfaces/scheduler_msg.h"

/* Receiver-side dispatch function for the `scheduler` interface.
 * The component's handle_msg delegates to this function, which
 * switches on msg->msg_type, unpacks the per-op request struct,
 * calls the matching op on `ops`, writes the reply struct in-place
 * at msg->payload, and sets msg->reply_payload_len so the
 * dispatcher's build_reply picks up the full per-op output. */
static inline int nx_scheduler_dispatch(
    void *self, const struct nx_scheduler_ops *ops,
    struct nx_ipc_message *msg)
{
    switch ((enum nx_scheduler_op_id)(msg->msg_type)) {
    case NX_SCHEDULER_OP_PICK_NEXT: {
        uint64_t _rcptr = (uint64_t)(uintptr_t)ops->pick_next(self);
        struct nx_scheduler_reply_pick_next *_r =
            (struct nx_scheduler_reply_pick_next *)msg->payload;
        _r->rc = _rcptr;
        msg->reply_payload_len = (uint32_t)sizeof *_r;
        return 0;
    }
    case NX_SCHEDULER_OP_ENQUEUE: {
        struct nx_scheduler_msg_enqueue *_req =
            (struct nx_scheduler_msg_enqueue *)msg->payload;
        struct nx_task *_task = (struct nx_task *)(uintptr_t)_req->task;
        int _rc = ops->enqueue(self, _task);
        struct nx_scheduler_reply_enqueue *_r =
            (struct nx_scheduler_reply_enqueue *)msg->payload;
        _r->rc = _rc;
        msg->reply_payload_len = (uint32_t)sizeof *_r;
        return _rc;
    }
    case NX_SCHEDULER_OP_DEQUEUE: {
        struct nx_scheduler_msg_dequeue *_req =
            (struct nx_scheduler_msg_dequeue *)msg->payload;
        struct nx_task *_task = (struct nx_task *)(uintptr_t)_req->task;
        int _rc = ops->dequeue(self, _task);
        struct nx_scheduler_reply_dequeue *_r =
            (struct nx_scheduler_reply_dequeue *)msg->payload;
        _r->rc = _rc;
        msg->reply_payload_len = (uint32_t)sizeof *_r;
        return _rc;
    }
    case NX_SCHEDULER_OP_YIELD: {
        ops->yield(self);
        struct nx_scheduler_reply_yield *_r =
            (struct nx_scheduler_reply_yield *)msg->payload;
        _r->rc = 0;
        msg->reply_payload_len = (uint32_t)sizeof *_r;
        return 0;
    }
    case NX_SCHEDULER_OP_SET_PRIORITY: {
        struct nx_scheduler_msg_set_priority *_req =
            (struct nx_scheduler_msg_set_priority *)msg->payload;
        struct nx_task *_task = (struct nx_task *)(uintptr_t)_req->task;
        int _priority = _req->priority;
        int _rc = ops->set_priority(self, _task, _priority);
        struct nx_scheduler_reply_set_priority *_r =
            (struct nx_scheduler_reply_set_priority *)msg->payload;
        _r->rc = _rc;
        msg->reply_payload_len = (uint32_t)sizeof *_r;
        return _rc;
    }
    case NX_SCHEDULER_OP_TICK: {
        ops->tick(self);
        struct nx_scheduler_reply_tick *_r =
            (struct nx_scheduler_reply_tick *)msg->payload;
        _r->rc = 0;
        msg->reply_payload_len = (uint32_t)sizeof *_r;
        return 0;
    }
    default:
        return NX_EINVAL;
    }
}

#endif /* NONUX_FRAMEWORK_SCHEDULER_DISPATCH_H */
