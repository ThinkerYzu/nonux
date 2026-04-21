#include "framework/ipc.h"
#include "framework/component.h"
#include "framework/hook.h"
#include "framework/registry.h"

#include <stdlib.h>
#include <string.h>

/*
 * IPC router — slice 3.6 (sync/async + caps) plus slice 3.8 (pause
 * protocol + hooks + REDIRECT).
 *
 * Data structures kept deliberately small:
 *
 *   g_queues — per-slot inbox head/tail (slice 3.6 async path).
 *   g_holds  — per-(src, dst) hold queue used by NX_PAUSE_QUEUE policy
 *              when the destination slot is paused.  Keyed by slot
 *              pointer pair rather than extending `struct nx_slot`
 *              so the registry type stays stable and a slot can hold
 *              messages from many callers without a ragged array.
 *
 * A singly-linked-list lookup by slot pointer is O(slots) on both;
 * swapping to a hash is slice 3.9's problem.
 *
 * REDIRECT loop guard: `do_send(depth)` recurses with `depth + 1` when
 * the destination is paused and the edge's policy is REDIRECT.  Once
 * depth crosses `NX_IPC_REDIRECT_DEPTH_MAX`, we return NX_ELOOP — the
 * caller gets a clear signal that the fallback chain folded back onto
 * itself rather than silently spinning.
 */

#define NX_IPC_REDIRECT_DEPTH_MAX 4

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

size_t nx_ipc_inbox_depth(struct nx_slot *slot)
{
    struct ipc_slot_queue *q = queue_for(slot, false);
    return q ? q->depth : 0;
}

/* ---------- Per-edge hold queue (NX_PAUSE_QUEUE) ----------------------- */

struct ipc_hold_entry {
    struct nx_slot         *src;       /* NULL for boot/external edges */
    struct nx_slot         *dst;
    struct nx_ipc_message  *head;
    struct nx_ipc_message  *tail;
    size_t                  depth;
    struct ipc_hold_entry  *next;
};

static struct ipc_hold_entry *g_holds;

static struct ipc_hold_entry *hold_entry_for(struct nx_slot *src,
                                             struct nx_slot *dst,
                                             bool create)
{
    for (struct ipc_hold_entry *e = g_holds; e; e = e->next)
        if (e->src == src && e->dst == dst) return e;
    if (!create) return NULL;
    struct ipc_hold_entry *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->src = src;
    e->dst = dst;
    e->next = g_holds;
    g_holds = e;
    return e;
}

static void hold_enqueue(struct ipc_hold_entry *e, struct nx_ipc_message *m)
{
    m->_next = NULL;
    if (!e->head) e->head = m;
    else          e->tail->_next = m;
    e->tail = m;
    e->depth++;
}

static struct nx_ipc_message *hold_dequeue(struct ipc_hold_entry *e)
{
    struct nx_ipc_message *m = e->head;
    if (!m) return NULL;
    e->head = m->_next;
    if (!e->head) e->tail = NULL;
    e->depth--;
    m->_next = NULL;
    return m;
}

size_t nx_ipc_hold_queue_depth(struct nx_slot *src, struct nx_slot *dst)
{
    struct ipc_hold_entry *e = hold_entry_for(src, dst, false);
    return e ? e->depth : 0;
}

/* ---------- Reset ------------------------------------------------------ */

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
    while (g_holds) {
        struct ipc_hold_entry *e = g_holds;
        g_holds = e->next;
        for (struct nx_ipc_message *m = e->head; m; ) {
            struct nx_ipc_message *nxt = m->_next;
            m->_next = NULL;
            m = nxt;
        }
        free(e);
    }
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

/* Slice 3.8: `do_send` is the real router, recursive on REDIRECT with
 * a depth counter.  `nx_ipc_send` is the public zero-depth wrapper. */
static int do_send(struct nx_ipc_message *msg, int depth)
{
    if (!msg || !msg->src_slot || !msg->dst_slot) return NX_EINVAL;

    /* Pre-route hook.  edge is NULL on purpose — the hook sees the
     * routing intent, not the resolved edge, so filters / tracers
     * can ABORT before the router even looks up the connection. */
    struct nx_hook_context hctx = {
        .point = NX_HOOK_IPC_SEND,
        .u.ipc = { .src = msg->src_slot, .dst = msg->dst_slot,
                   .msg = msg, .edge = NULL },
    };
    if (nx_hook_dispatch(&hctx) == NX_HOOK_ABORT) return NX_EABORT;

    struct nx_connection *edge = find_edge(msg->src_slot, msg->dst_slot);
    if (!edge) return NX_ENOENT;

    int rc = nx_ipc_scan_send_caps(msg->src_slot, msg);
    if (rc != NX_OK) return rc;

    /* Pause protocol.  If dst_slot is in any non-NONE state, the
     * edge's policy decides what to do.  NONE is the fast path. */
    enum nx_slot_pause_state ps = nx_slot_pause_state(msg->dst_slot);
    if (ps != NX_SLOT_PAUSE_NONE) {
        switch (edge->policy) {
        case NX_PAUSE_QUEUE: {
            struct ipc_hold_entry *e =
                hold_entry_for(msg->src_slot, msg->dst_slot, true);
            if (!e) return NX_ENOMEM;
            hold_enqueue(e, msg);
            return NX_OK;
        }
        case NX_PAUSE_REJECT:
            return NX_EBUSY;
        case NX_PAUSE_REDIRECT: {
            if (depth >= NX_IPC_REDIRECT_DEPTH_MAX) return NX_ELOOP;
            struct nx_slot *fb = msg->dst_slot->fallback;
            if (!fb) return NX_ENOENT;
            /* Retarget in place — the caller's msg reflects the final
             * hop (documented behaviour).  Async enqueue relies on
             * msg->dst_slot holding the queue key when the dispatcher
             * runs, so we can't restore it on the way out. */
            msg->dst_slot = fb;
            return do_send(msg, depth + 1);
        }
        }
    }

    if (edge->mode == NX_CONN_SYNC) {
        return invoke_handler(msg->dst_slot, msg);
    }

    /* Async: enqueue for a later nx_ipc_dispatch call. */
    struct ipc_slot_queue *q = queue_for(msg->dst_slot, true);
    if (!q) return NX_ENOMEM;
    enqueue(q, msg);
    return NX_OK;
}

int nx_ipc_send(struct nx_ipc_message *msg)
{
    return do_send(msg, 0);
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

        /* Pre-handler hook.  ABORT drops this message and continues
         * the drain — same ledger as a delivered msg so observability
         * hooks can filter noise without stalling the queue. */
        struct nx_hook_context hctx = {
            .point = NX_HOOK_IPC_RECV,
            .u.ipc = { .src  = m->src_slot, .dst = slot,
                       .msg  = m,
                       .edge = find_edge(m->src_slot, slot) },
        };
        if (nx_hook_dispatch(&hctx) == NX_HOOK_ABORT) {
            dispatched++;
            continue;
        }

        int rc = invoke_handler(slot, m);
        dispatched++;
        if (rc != NX_OK) break;
    }
    return dispatched;
}

/* ---------- Hold-queue flush ------------------------------------------ */

void nx_ipc_flush_hold_queue(struct nx_slot *dst)
{
    if (!dst) return;
    /* Walk every (src, dst) entry matching `dst`.  We detach each
     * matching entry from g_holds first so nested flushes (a hook
     * that somehow triggers another flush) don't revisit the same
     * entry.  Messages are then replayed via nx_ipc_send so they
     * traverse hooks, cap-scan, and pause-policy normally. */
    struct ipc_hold_entry **pp = &g_holds;
    while (*pp) {
        struct ipc_hold_entry *e = *pp;
        if (e->dst != dst) { pp = &e->next; continue; }
        *pp = e->next;

        for (struct nx_ipc_message *m = hold_dequeue(e); m;
             m = hold_dequeue(e)) {
            (void)nx_ipc_send(m);    /* caller may inspect return via hook */
        }
        free(e);
        /* pp is unchanged — *pp now points to the node that used to
         * follow `e`, which is the next candidate. */
    }
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
