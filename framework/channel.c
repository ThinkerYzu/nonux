#include "framework/channel.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if __STDC_HOSTED__
#include <stdlib.h>
#include <string.h>
#else
#include "core/lib/kheap.h"
#include "core/lib/lib.h"    /* memcpy, memset */
#endif

#include "framework/pollset.h"
#include "framework/syscall.h"   /* NX_POLLIN / NX_POLLOUT / NX_POLLHUP */

/*
 * Channel implementation — slice 5.6.
 *
 * One `struct nx_channel` allocation holds both endpoints and a
 * shared refcount.  Endpoints cross-reference each other via their
 * common `chan` pointer and an index (0 or 1), so `peer_of(e)` is a
 * compile-time-free computation.
 *
 *   send(e, data):  write into peer_of(e)'s ring.tail, bump tail.
 *   recv(e, buf):   read from e's own ring.head, bump head.
 *
 * The ring's head is advanced only by the reader (endpoint owner);
 * the ring's tail is advanced only by the remote sender.  In a v1
 * single-CPU, single-reader, single-writer setup, no explicit
 * synchronization is needed: preemption between the memcpy and the
 * tail bump is safe because the tail isn't observed until after the
 * bump, so the reader either sees the old tail (ring empty) or the
 * new tail (and the freshly written message).  SMP would need
 * release/acquire barriers on the tail; that's a future audit.
 */

struct channel_msg {
    uint16_t len;
    uint8_t  data[NX_CHANNEL_MSG_MAX];
};

struct nx_channel_endpoint {
    struct nx_channel *chan;          /* shared allocation */
    uint8_t            idx;           /* 0 or 1 — this side's slot */
    bool               closed;        /* derived: handle_refs == 0 */
    _Atomic int        handle_refs;   /* slice 7.6 prereq — fork
                                       * inheritance bumps this so
                                       * a single endpoint can
                                       * survive multiple closes */
    unsigned           head;          /* index of next message to recv */
    unsigned           tail;          /* index of next free slot for send */
    struct channel_msg ring[NX_CHANNEL_RING_LEN];
    /* Slice 7.8b: read-readiness watchers.  Listeners are owned by
     * sys_ppoll's pollset on its kstack.  Producers (peer-side
     * nx_channel_send + either-side close) walk this list and wake
     * each listener's parent waitq. */
    struct nx_list_head pollset_listeners;
};

struct nx_channel {
    struct nx_channel_endpoint e[2];
};

static struct nx_channel_endpoint *peer_of(struct nx_channel_endpoint *e)
{
    return &e->chan->e[1u - e->idx];
}

static const struct nx_channel_endpoint *
peer_of_const(const struct nx_channel_endpoint *e)
{
    return &e->chan->e[1u - e->idx];
}

/* ---------- Public API ------------------------------------------------ */

int nx_channel_create(struct nx_channel_endpoint **e0,
                      struct nx_channel_endpoint **e1)
{
    if (!e0 || !e1) return NX_EINVAL;

    struct nx_channel *c = calloc(1, sizeof *c);
    if (!c) return NX_ENOMEM;

    c->e[0].chan = c;
    c->e[0].idx  = 0;
    c->e[1].chan = c;
    c->e[1].idx  = 1;
    /* One handle is allocated per endpoint by the caller (sys_pipe /
     * sys_channel_create) immediately after this returns; track that
     * pre-allocation explicitly so close discipline matches alloc
     * discipline. */
    atomic_init(&c->e[0].handle_refs, 1);
    atomic_init(&c->e[1].handle_refs, 1);
    /* Slice 7.8b: empty pollset listener list per endpoint. */
    nx_list_init(&c->e[0].pollset_listeners);
    nx_list_init(&c->e[1].pollset_listeners);

    *e0 = &c->e[0];
    *e1 = &c->e[1];
    return NX_OK;
}

int nx_channel_send(struct nx_channel_endpoint *e,
                    const void *data, size_t len)
{
    if (!e || !data) return NX_EINVAL;
    if (len > NX_CHANNEL_MSG_MAX) return NX_EINVAL;
    if (e->closed) return NX_EBUSY;

    struct nx_channel_endpoint *p = peer_of(e);
    if (p->closed) return NX_EBUSY;

    unsigned tail = p->tail;
    unsigned next = (tail + 1u) % NX_CHANNEL_RING_LEN;
    if (next == p->head) return NX_EBUSY;   /* ring full */

    memcpy(p->ring[tail].data, data, len);
    p->ring[tail].len = (uint16_t)len;
    p->tail = next;
    /* Slice 7.8b: peer's read-readiness rose from "would NX_EAGAIN"
     * (or "would EOF") to "has data".  Wake every pollset waiting on
     * the peer endpoint. */
    nx_pollset_wake_all(&p->pollset_listeners);
    return (int)len;
}

int nx_channel_recv(struct nx_channel_endpoint *e,
                    void *buf, size_t cap)
{
    if (!e || !buf) return NX_EINVAL;
    if (cap == 0)  return NX_EINVAL;
    if (e->closed) return NX_EBUSY;
    if (e->head == e->tail) {
        /* Empty queue.  POSIX pipe semantic: if the writer side
         * (peer endpoint) is fully closed, this is EOF (return 0).
         * Otherwise it's transient — caller can retry.  Slice
         * 7.6d.N.6b: cat reads from a pipe whose writer (echo) has
         * exited; without this branch cat sees NX_EAGAIN forever. */
        if (peer_of_const(e)->closed) return 0;
        return NX_EAGAIN;
    }

    struct channel_msg *m = &e->ring[e->head];
    if ((size_t)m->len > cap) return NX_ENOMEM;

    memcpy(buf, m->data, m->len);
    int copied = (int)m->len;
    e->head = (e->head + 1u) % NX_CHANNEL_RING_LEN;
    return copied;
}

void nx_channel_endpoint_retain(struct nx_channel_endpoint *e)
{
    if (!e) return;
    /* Caller must already hold a reference (parent's handle is live),
     * so the endpoint can't transition to closed underneath us — the
     * load + add is safe with relaxed ordering on a single CPU.  SMP
     * would need acq_rel semantics on a fetch_add. */
    atomic_fetch_add_explicit(&e->handle_refs, 1, memory_order_relaxed);
}

void nx_channel_endpoint_close(struct nx_channel_endpoint *e)
{
    if (!e) return;
    /* Decrement this endpoint's handle refcount.  Only when it hits
     * zero do we mark the endpoint closed (which causes peer sends to
     * see NX_EBUSY).  When BOTH endpoints in the channel pair are
     * closed, the whole channel allocation is freed. */
    int prev = atomic_fetch_sub_explicit(&e->handle_refs, 1,
                                         memory_order_acq_rel);
    if (prev != 1) return;   /* still other handle refs to this endpoint */
    e->closed = true;
    /* Slice 7.8b: peer's recv now sees EOF (POLLHUP) and a writer
     * blocked on space-in-our-ring would unblock with the channel
     * gone.  Wake watchers on both endpoints — peer's listeners want
     * the EOF transition; this side's listeners (if any survived a
     * race) want POLLHUP-on-self. */
    nx_pollset_wake_all(&peer_of(e)->pollset_listeners);
    nx_pollset_wake_all(&e->pollset_listeners);

    /* Last-handle-on-this-endpoint owner.  If the peer endpoint is
     * also already closed (zero handle_refs), nobody else holds the
     * channel allocation — free it.  We re-read the peer's
     * `handle_refs` rather than caching anything because two final
     * closers on opposite endpoints could race; only one of them
     * sees both refs at zero, and that one frees. */
    struct nx_channel *c = e->chan;
    int peer_refs = atomic_load_explicit(&c->e[1u - e->idx].handle_refs,
                                         memory_order_acquire);
    if (peer_refs == 0) free(c);
}

/* ---------- Test helpers --------------------------------------------- */

size_t nx_channel_endpoint_depth(const struct nx_channel_endpoint *e)
{
    if (!e) return 0;
    if (e->tail >= e->head)
        return (size_t)(e->tail - e->head);
    return (size_t)(e->tail + NX_CHANNEL_RING_LEN - e->head);
}

bool nx_channel_endpoint_is_closed(const struct nx_channel_endpoint *e)
{
    return e ? e->closed : true;
}

bool nx_channel_endpoint_peer_closed(const struct nx_channel_endpoint *e)
{
    if (!e) return true;
    return peer_of_const(e)->closed;
}

/* ---------- Slice 7.8b: pollset integration --------------------------- */

void nx_channel_endpoint_register_pollset(struct nx_channel_endpoint *e,
                                          struct nx_pollset_listener *l)
{
    if (!e || !l) return;
    /* The caller has already initialised `l->waitq`.  Append to the
     * tail so wake order reflects ppoll registration order — not
     * load-bearing for correctness but matches the FIFO discipline
     * the slice 7.8a waitq primitive uses. */
    nx_list_add_tail(&e->pollset_listeners, &l->node);
}

void nx_channel_endpoint_unregister_pollset(struct nx_pollset_listener *l)
{
    if (!l) return;
    /* Idempotent: nx_list_remove on a self-pointing node is a no-op
     * (but leaves the node still self-pointing).  Caller must not
     * call us after the listener's storage has been freed. */
    nx_list_remove(&l->node);
}

short nx_channel_endpoint_readiness(const struct nx_channel_endpoint *e,
                                    short want)
{
    if (!e) return NX_POLLNVAL;

    short revents = 0;
    /* POLLIN: matches `nx_channel_recv`'s "would not return EAGAIN"
     * predicate.  Either the ring is non-empty OR the peer is closed
     * (peer-closed is reported as readable so the caller's recv loop
     * sees the EOF return value, matching POSIX pipe semantics). */
    if (want & NX_POLLIN) {
        if (e->head != e->tail || peer_of_const(e)->closed)
            revents |= NX_POLLIN;
    }
    /* POLLOUT: matches `nx_channel_send`'s "would not return EBUSY"
     * predicate, except on the closed-peer side where the producer
     * is going to fail anyway — report POLLOUT clear. */
    if (want & NX_POLLOUT) {
        const struct nx_channel_endpoint *p = peer_of_const(e);
        if (!e->closed && !p->closed) {
            unsigned next = (p->tail + 1u) % NX_CHANNEL_RING_LEN;
            if (next != p->head) revents |= NX_POLLOUT;
        }
    }
    /* POLLHUP: peer-closed is always reported regardless of `want`
     * mask — POSIX poll semantics. */
    if (peer_of_const(e)->closed) revents |= NX_POLLHUP;
    return revents;
}
