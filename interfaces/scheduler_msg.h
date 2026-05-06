/*
 * GENERATED — DO NOT EDIT.
 * Source: interfaces/idl/scheduler.json
 * Generator: tools/gen-iface.py
 */

#ifndef NONUX_INTERFACE_SCHEDULER_MSG_H
#define NONUX_INTERFACE_SCHEDULER_MSG_H

#include <stddef.h>
#include <stdint.h>
#include "core/sched/task.h"

/* Op-IDs for `scheduler` interface.  Stable across versions; removed ops
 * leave their slot as a gravestone — never reuse a freed id. */
enum nx_scheduler_op_id {
    NX_SCHEDULER_OP_PICK_NEXT = 1,
    NX_SCHEDULER_OP_ENQUEUE = 2,
    NX_SCHEDULER_OP_DEQUEUE = 3,
    NX_SCHEDULER_OP_YIELD = 4,
    NX_SCHEDULER_OP_SET_PRIORITY = 5,
    NX_SCHEDULER_OP_TICK = 6,
    NX_SCHEDULER_OP_RUNQUEUE_SIZE = 7,
};

/* Request: nx_scheduler_pick_next() — Return the task that should run next on this CPU, or NULL if the */
struct nx_scheduler_msg_pick_next {
    char _nx_no_payload;
};

struct nx_scheduler_reply_pick_next {
    uint64_t rc; /* pointer encoded as u64 */
};

/* Request: nx_scheduler_enqueue() — Add `task` to the runqueue.  Borrow. */
struct nx_scheduler_msg_enqueue {
    uint64_t task;
};

struct nx_scheduler_reply_enqueue {
    int rc;
};

/* Request: nx_scheduler_dequeue() — Remove `task` from the runqueue.  Borrow. */
struct nx_scheduler_msg_dequeue {
    uint64_t task;
};

struct nx_scheduler_reply_dequeue {
    int rc;
};

/* Request: nx_scheduler_yield() — The current task voluntarily gives up the rest of its quantum. */
struct nx_scheduler_msg_yield {
    char _nx_no_payload;
};

struct nx_scheduler_reply_yield {
    int rc; /* always NX_OK; placeholder for void ops */
};

/* Request: nx_scheduler_set_priority() — Set `task`'s priority.  `priority` is policy-defined; the only */
struct nx_scheduler_msg_set_priority {
    uint64_t task;
    int priority;
};

struct nx_scheduler_reply_set_priority {
    int rc;
};

/* Request: nx_scheduler_tick() — Called once per timer tick from the core driver's `sched_tick` */
struct nx_scheduler_msg_tick {
    char _nx_no_payload;
};

struct nx_scheduler_reply_tick {
    int rc; /* always NX_OK; placeholder for void ops */
};

/* Request: nx_scheduler_runqueue_size() — Return the number of non-idle tasks currently on the runqueue. */
struct nx_scheduler_msg_runqueue_size {
    char _nx_no_payload;
};

struct nx_scheduler_reply_runqueue_size {
    int rc;
};

#endif /* NONUX_INTERFACE_SCHEDULER_MSG_H */
