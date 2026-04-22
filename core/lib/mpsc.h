#ifndef NONUX_LIB_MPSC_H
#define NONUX_LIB_MPSC_H

#include <stdatomic.h>
#include <stddef.h>

/*
 * Multi-Producer Single-Consumer lock-free queue — Vyukov-style
 * intrusive linked list.
 *
 *   Producer (any thread, any context including ISRs):
 *       1. Set node->next = NULL (release).
 *       2. prev = atomic_exchange(&tail, node, acq_rel).
 *       3. prev->next = node (release).        <-- link publication
 *
 *   Consumer (exactly one thread at a time):
 *       Pop walks head forward, skipping the stub when needed.
 *       Returns NULL if genuinely empty or if a producer is
 *       mid-push (the "inconsistent" state — retry later).
 *
 * Used by the framework dispatcher (slice 3.9b.1) to accept
 * `struct nx_ipc_message` pointers from any source — kthread,
 * ISR, or the dispatcher feeding back to itself — and drain
 * them on the dispatcher thread.  Since enqueue is a single
 * atomic exchange, ISRs can push without holding the IRQ mask
 * any longer than a handful of instructions.  The slot-resolve
 * locality invariant (R8) stays intact: the ISR pushes a
 * pre-constructed `struct nx_ipc_message` and returns; the
 * dispatcher thread (a regular kthread, not an ISR) is the one
 * that ever dereferences `msg->dst_slot->active`.
 *
 * ABA safety: nodes are never re-added to the same queue
 * (messages are caller-owned; the dispatcher dequeues and lets
 * the caller go).  If a node is recycled, the recycle has to
 * happen after the consumer has fully observed the dequeue,
 * which the kthread scheduler's sequencing handles.
 */

struct nx_mpsc_node {
    _Atomic(struct nx_mpsc_node *) next;
};

struct nx_mpsc_queue {
    _Atomic(struct nx_mpsc_node *) tail;   /* producer-visible */
    struct nx_mpsc_node           *head;   /* consumer-private */
    struct nx_mpsc_node            stub;
};

static inline void nx_mpsc_init(struct nx_mpsc_queue *q)
{
    atomic_init(&q->stub.next, (struct nx_mpsc_node *)NULL);
    q->head = &q->stub;
    atomic_init(&q->tail, &q->stub);
}

static inline void nx_mpsc_push(struct nx_mpsc_queue *q,
                                struct nx_mpsc_node  *node)
{
    atomic_store_explicit(&node->next, (struct nx_mpsc_node *)NULL,
                          memory_order_relaxed);
    struct nx_mpsc_node *prev =
        atomic_exchange_explicit(&q->tail, node, memory_order_acq_rel);
    atomic_store_explicit(&prev->next, node, memory_order_release);
}

/*
 * Pop the oldest node, or NULL if empty.  May also return NULL in a
 * transient "producer mid-push" state — callers should treat NULL
 * as "nothing to do right now" and retry later (yield / wait).
 */
static inline struct nx_mpsc_node *nx_mpsc_pop(struct nx_mpsc_queue *q)
{
    struct nx_mpsc_node *head = q->head;
    struct nx_mpsc_node *next =
        atomic_load_explicit(&head->next, memory_order_acquire);

    if (head == &q->stub) {
        if (!next) return NULL;         /* really empty */
        q->head = next;
        head = next;
        next = atomic_load_explicit(&head->next, memory_order_acquire);
    }

    if (next) {
        q->head = next;
        return head;
    }

    /* head == tail?  push the stub so the producer's chain can link
     * past the current head, and retry.  Classic Vyukov tail-catch. */
    struct nx_mpsc_node *tail =
        atomic_load_explicit(&q->tail, memory_order_acquire);
    if (head != tail) return NULL;      /* producer mid-push; retry later */

    nx_mpsc_push(q, &q->stub);
    next = atomic_load_explicit(&head->next, memory_order_acquire);
    if (next) {
        q->head = next;
        return head;
    }
    return NULL;
}

#endif /* NONUX_LIB_MPSC_H */
