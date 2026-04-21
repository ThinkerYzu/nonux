#include "framework/ipc.h"
#include "framework/component.h"
#include "framework/registry.h"

#include <stdlib.h>
#include <string.h>

/*
 * IPC router — slice 3.6 host-side.
 *
 * Data structures kept deliberately small: a per-slot inbox head +
 * tail pointer, and a side table keyed by slot pointer so we don't
 * have to extend `struct nx_slot` for framework bookkeeping.  A
 * singly-linked-list lookup by slot pointer is O(slots); swapping
 * to a hash is slice 3.9's problem.
 *
 * Slice 3.9 replaces the inbox with an MPSC lock-free queue and lets
 * a pinned per-CPU dispatcher run `nx_ipc_dispatch` continuously.
 * The public contract in ipc.h stays the same.
 */

/* ---------- Per-slot inbox --------------------------------------------- */

struct ipc_slot_queue {
    struct nx_slot         *slot;
    struct nx_ipc_message  *head;
    struct nx_ipc_message  *tail;
    size_t                  depth;
    struct ipc_slot_queue  *next;   /* in g_queues list */
};

static struct ipc_slot_queue *g_queues;

static struct ipc_slot_queue *queue_for(struct nx_slot *slot, bool create)
{
    for (struct ipc_slot_queue *q = g_queues; q; q = q->next)
        if (q->slot == slot) return q;
    if (!create) return NULL;
    struct ipc_slot_queue *q = calloc(1, sizeof *q);
    if (!q) return NULL;
    q->slot  = slot;
    q->next  = g_queues;
    g_queues = q;
    return q;
}

void nx_ipc_reset(void)
{
    while (g_queues) {
        struct ipc_slot_queue *q = g_queues;
        g_queues = q->next;
        /* Messages themselves are caller-owned storage; we just clear
         * the `_next` links so stale pointers don't linger. */
        for (struct nx_ipc_message *m = q->head; m; ) {
            struct nx_ipc_message *nxt = m->_next;
            m->_next = NULL;
            m = nxt;
        }
        free(q);
    }
}

size_t nx_ipc_inbox_depth(struct nx_slot *slot)
{
    struct ipc_slot_queue *q = queue_for(slot, false);
    return q ? q->depth : 0;
}

static void enqueue(struct ipc_slot_queue *q, struct nx_ipc_message *m)
{
    m->_next = NULL;
    if (!q->head) q->head = m;
    else          q->tail->_next = m;
    q->tail = m;
    q->depth++;
}

static struct nx_ipc_message *dequeue(struct ipc_slot_queue *q)
{
    struct nx_ipc_message *m = q->head;
    if (!m) return NULL;
    q->head = m->_next;
    if (!q->head) q->tail = NULL;
    q->depth--;
    m->_next = NULL;
    return m;
}

/* ---------- Connection edge lookup ------------------------------------ */

/* Find a registered connection edge from `from` to `to`.  Returns NULL
 * if none.  If multiple edges exist between the pair (allowed by the
 * registry but disallowed by IPC contract), we return the first the
 * per-slot traversal hands us. */
struct conn_search {
    struct nx_slot       *wanted_to;
    struct nx_connection *hit;
};

static void conn_match_to(struct nx_connection *c, void *ctx)
{
    struct conn_search *s = ctx;
    if (s->hit) return;
    if (c->to_slot == s->wanted_to) s->hit = c;
}

static struct nx_connection *find_edge(struct nx_slot *from,
                                       struct nx_slot *to)
{
    if (!from || !to) return NULL;
    struct conn_search s = { .wanted_to = to, .hit = NULL };
    nx_slot_foreach_dependency(from, conn_match_to, &s);
    return s.hit;
}

/* ---------- Cap scanning --------------------------------------------- */

int nx_ipc_scan_send_caps(struct nx_slot *sender,
                          struct nx_ipc_message *msg)
{
    if (!sender || !msg) return NX_EINVAL;
    for (uint32_t i = 0; i < msg->n_caps; i++) {
        struct nx_ipc_cap *c = &msg->caps[i];
        if (c->kind != NX_CAP_SLOT_REF) continue;
        if (!c->u.slot_ref) return NX_EINVAL;
        if (!find_edge(sender, c->u.slot_ref)) return NX_EINVAL;
    }
    return NX_OK;
}

size_t nx_ipc_scan_recv_caps(struct nx_slot *recv,
                             struct nx_ipc_message *msg)
{
    (void)recv;   /* recv is reserved for future per-component logging */
    if (!msg) return 0;
    size_t unclaimed_transfers = 0;
    for (uint32_t i = 0; i < msg->n_caps; i++) {
        struct nx_ipc_cap *c = &msg->caps[i];
        if (c->kind != NX_CAP_SLOT_REF) continue;
        if (c->claimed) continue;
        if (c->ownership == NX_CAP_TRANSFER) unclaimed_transfers++;
        /* BORROW + unclaimed is the expected common case. */
    }
    return unclaimed_transfers;
}

/* ---------- Dispatch (sync shortcut + async drain) -------------------- */

static int invoke_handler(struct nx_slot *dst, struct nx_ipc_message *msg)
{
    struct nx_component *active = dst->active;
    if (!active || !active->descriptor || !active->descriptor->ops ||
        !active->descriptor->ops->handle_msg)
        return NX_ENOENT;
    int rc = active->descriptor->ops->handle_msg(active->impl, msg);
    /* Recv-side cap sweep happens regardless of handler rc — even a
     * failing handler is responsible for borrowed caps being dropped
     * and transfers being accounted for. */
    nx_ipc_scan_recv_caps(dst, msg);
    return rc;
}

int nx_ipc_send(struct nx_ipc_message *msg)
{
    if (!msg || !msg->src_slot || !msg->dst_slot) return NX_EINVAL;

    struct nx_connection *edge = find_edge(msg->src_slot, msg->dst_slot);
    if (!edge) return NX_ENOENT;

    int rc = nx_ipc_scan_send_caps(msg->src_slot, msg);
    if (rc != NX_OK) return rc;

    if (edge->mode == NX_CONN_SYNC) {
        return invoke_handler(msg->dst_slot, msg);
    }

    /* Async: enqueue for a later nx_ipc_dispatch call. */
    struct ipc_slot_queue *q = queue_for(msg->dst_slot, true);
    if (!q) return NX_ENOMEM;
    enqueue(q, msg);
    return NX_OK;
}

size_t nx_ipc_dispatch(struct nx_slot *slot, size_t max)
{
    if (!slot || max == 0) return 0;
    struct ipc_slot_queue *q = queue_for(slot, false);
    if (!q) return 0;
    size_t dispatched = 0;
    while (dispatched < max) {
        struct nx_ipc_message *m = dequeue(q);
        if (!m) break;
        int rc = invoke_handler(slot, m);
        dispatched++;
        if (rc != NX_OK) break;
    }
    return dispatched;
}

/* ---------- slot_ref_retain / _release -------------------------------- */

int nx_slot_ref_retain(struct nx_slot        *self,
                       struct nx_ipc_cap     *cap,
                       const char            *purpose,
                       struct nx_connection **out_conn)
{
    (void)purpose;   /* reserved for future change-log annotations */
    if (out_conn) *out_conn = NULL;

    if (!self || !cap) return NX_EINVAL;
    if (cap->kind != NX_CAP_SLOT_REF) return NX_EINVAL;
    if (cap->claimed) return NX_EINVAL;
    if (!cap->u.slot_ref) return NX_EINVAL;

    int err = NX_OK;
    struct nx_connection *c =
        nx_connection_register(self, cap->u.slot_ref,
                               NX_CONN_ASYNC, /* default; retain doesn't
                                               * carry mode metadata */
                               false, NX_PAUSE_QUEUE, &err);
    if (!c) return err;

    cap->claimed = true;
    if (out_conn) *out_conn = c;
    return NX_OK;
}

void nx_slot_ref_release(struct nx_slot *self, struct nx_slot *target)
{
    if (!self || !target) return;
    struct nx_connection *c = find_edge(self, target);
    if (c) nx_connection_unregister(c);
}
