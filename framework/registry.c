#include "framework/registry.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Component Graph Registry — first slice (host-side).
 *
 * Internal layout: three singly-linked lists keyed off three node types.
 * Connection edges are linked into both a global list and the per-slot
 * incoming/outgoing lists they belong to, so neighbourhood queries are O(deg)
 * without rescanning the global list.
 *
 * Allocations use plain malloc/free so the registry's lifetime is decoupled
 * from the host test harness's memory tracker (mt_reset would dangle our
 * pointers if we shared its pool).  When the registry lands in-kernel, these
 * call sites become kmalloc/kfree — single point to swap.
 */

/* ---------- Internal node types ------------------------------------------ */

struct conn_node;

struct slot_node {
    struct nx_slot   *slot;
    struct slot_node *next;
    struct conn_node *incoming;   /* edges where this slot is to_slot */
    struct conn_node *outgoing;   /* edges where this slot is from_slot */
    uint64_t          created_gen;
};

struct component_node {
    struct nx_component   *comp;
    struct component_node *next;
    uint64_t               created_gen;
};

struct conn_node {
    struct nx_connection *conn;
    struct conn_node     *next;       /* in global list */
    struct conn_node     *next_in;    /* in to_slot->incoming list */
    struct conn_node     *next_out;   /* in from_slot->outgoing list */
};

/* ---------- Global state -------------------------------------------------- */

static struct slot_node      *g_slots;
static struct component_node *g_components;
static struct conn_node      *g_connections;
static size_t                 g_slot_count;
static size_t                 g_component_count;
static size_t                 g_connection_count;
static uint64_t               g_generation;

static uint64_t bump_gen(void)
{
    return ++g_generation;
}

/* ---------- Timestamp helper --------------------------------------------- */

/* Host build: CLOCK_MONOTONIC in nanoseconds.  Kernel build will replace
 * this with a system-tick reader (single point to swap). */
static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ---------- Subscribers --------------------------------------------------- */

#define NX_MAX_SUBSCRIBERS 16

struct subscriber {
    nx_graph_event_cb cb;
    void             *ctx;
    bool              in_use;
};

static struct subscriber g_subscribers[NX_MAX_SUBSCRIBERS];

static int subscriber_find(nx_graph_event_cb cb, void *ctx)
{
    for (int i = 0; i < NX_MAX_SUBSCRIBERS; i++) {
        if (g_subscribers[i].in_use &&
            g_subscribers[i].cb == cb &&
            g_subscribers[i].ctx == ctx)
            return i;
    }
    return -1;
}

int nx_graph_subscribe(nx_graph_event_cb cb, void *ctx)
{
    if (!cb) return NX_EINVAL;
    if (subscriber_find(cb, ctx) >= 0) return NX_EEXIST;
    for (int i = 0; i < NX_MAX_SUBSCRIBERS; i++) {
        if (!g_subscribers[i].in_use) {
            g_subscribers[i].cb     = cb;
            g_subscribers[i].ctx    = ctx;
            g_subscribers[i].in_use = true;
            return NX_OK;
        }
    }
    return NX_ENOMEM;
}

void nx_graph_unsubscribe(nx_graph_event_cb cb, void *ctx)
{
    int i = subscriber_find(cb, ctx);
    if (i >= 0) g_subscribers[i].in_use = false;
}

/* ---------- Change log (ring buffer, atomic head/total) ----------------- */

#define NX_CHANGE_LOG_CAPACITY 256

static struct nx_graph_event g_log_ring[NX_CHANGE_LOG_CAPACITY];
static _Atomic uint64_t      g_log_total;   /* events ever appended */

static void change_log_append(const struct nx_graph_event *ev)
{
    uint64_t total = atomic_fetch_add_explicit(&g_log_total, 1,
                                               memory_order_relaxed);
    g_log_ring[total % NX_CHANGE_LOG_CAPACITY] = *ev;
}

uint64_t nx_change_log_total(void)
{
    return atomic_load_explicit(&g_log_total, memory_order_relaxed);
}

size_t nx_change_log_size(void)
{
    uint64_t total = nx_change_log_total();
    return total < NX_CHANGE_LOG_CAPACITY ? (size_t)total
                                          : NX_CHANGE_LOG_CAPACITY;
}

size_t nx_change_log_read(uint64_t since_gen,
                          struct nx_graph_event *out, size_t max)
{
    if (!out || max == 0) return 0;

    uint64_t total = nx_change_log_total();
    uint64_t held  = total < NX_CHANGE_LOG_CAPACITY ? total
                                                    : NX_CHANGE_LOG_CAPACITY;
    uint64_t oldest_idx = total - held;   /* logical index of oldest event */
    size_t   n = 0;

    for (uint64_t i = oldest_idx; i < total && n < max; i++) {
        const struct nx_graph_event *ev =
            &g_log_ring[i % NX_CHANGE_LOG_CAPACITY];
        if (ev->generation > since_gen) {
            out[n++] = *ev;
        }
    }
    return n;
}

void nx_change_log_reset(void)
{
    atomic_store_explicit(&g_log_total, 0, memory_order_relaxed);
    /* No need to clear the ring: nx_change_log_size() reports 0 and
     * subsequent appends will overwrite slots before they're read. */
}

/* ---------- Event dispatch ------------------------------------------------ */

static void emit_event(struct nx_graph_event *ev)
{
    ev->generation   = g_generation;
    ev->timestamp_ns = now_ns();
    change_log_append(ev);
    for (int i = 0; i < NX_MAX_SUBSCRIBERS; i++) {
        if (g_subscribers[i].in_use)
            g_subscribers[i].cb(ev, g_subscribers[i].ctx);
    }
}

/* ---------- Slot helpers -------------------------------------------------- */

static struct slot_node *slot_node_find(const char *name)
{
    for (struct slot_node *n = g_slots; n; n = n->next) {
        if (n->slot->name && name && strcmp(n->slot->name, name) == 0)
            return n;
    }
    return NULL;
}

static struct slot_node *slot_node_for(const struct nx_slot *s)
{
    if (!s) return NULL;
    for (struct slot_node *n = g_slots; n; n = n->next) {
        if (n->slot == s) return n;
    }
    return NULL;
}

/* ---------- Component helpers -------------------------------------------- */

static struct component_node *component_node_find(const char *manifest_id,
                                                  const char *instance_id)
{
    if (!manifest_id || !instance_id) return NULL;
    for (struct component_node *n = g_components; n; n = n->next) {
        struct nx_component *c = n->comp;
        if (c->manifest_id && c->instance_id &&
            strcmp(c->manifest_id, manifest_id) == 0 &&
            strcmp(c->instance_id, instance_id) == 0)
            return n;
    }
    return NULL;
}

static struct component_node *component_node_for(const struct nx_component *c)
{
    if (!c) return NULL;
    for (struct component_node *n = g_components; n; n = n->next) {
        if (n->comp == c) return n;
    }
    return NULL;
}

bool nx_component_is_bound(const struct nx_component *c)
{
    if (!c) return false;
    for (struct slot_node *n = g_slots; n; n = n->next) {
        if (n->slot->active == c) return true;
    }
    return false;
}

void nx_component_foreach_bound_slot(const struct nx_component *c,
                                     nx_slot_cb cb, void *ctx)
{
    if (!c || !cb) return;
    for (struct slot_node *n = g_slots; n; n = n->next) {
        if (n->slot->active == c) cb(n->slot, ctx);
    }
}

/* ---------- Per-slot connection list helpers ----------------------------- */

static void slot_add_incoming(struct slot_node *sn, struct conn_node *cn)
{
    cn->next_in   = sn->incoming;
    sn->incoming  = cn;
}

static void slot_add_outgoing(struct slot_node *sn, struct conn_node *cn)
{
    cn->next_out  = sn->outgoing;
    sn->outgoing  = cn;
}

static void slot_remove_incoming(struct slot_node *sn, struct conn_node *cn)
{
    struct conn_node **pp = &sn->incoming;
    while (*pp && *pp != cn) pp = &(*pp)->next_in;
    if (*pp) *pp = cn->next_in;
}

static void slot_remove_outgoing(struct slot_node *sn, struct conn_node *cn)
{
    struct conn_node **pp = &sn->outgoing;
    while (*pp && *pp != cn) pp = &(*pp)->next_out;
    if (*pp) *pp = cn->next_out;
}

/* ---------- Slot mutation API -------------------------------------------- */

int nx_slot_register(struct nx_slot *s)
{
    if (!s || !s->name) return NX_EINVAL;
    if (slot_node_find(s->name)) return NX_EEXIST;

    struct slot_node *n = calloc(1, sizeof *n);
    if (!n) return NX_ENOMEM;

    /* atomic_init is the portable way to initialise an _Atomic member;
     * subsequent operations must go through atomic_load / atomic_store.
     * Without this, a caller-owned `struct nx_slot` zeroed by plain
     * struct assignment or memset may not be well-defined per C11 even
     * though it works on every host compiler we test on.  fallback is
     * a plain pointer so a plain assignment is fine. */
    atomic_init(&s->pause_state, NX_SLOT_PAUSE_NONE);
    s->fallback = NULL;

    n->slot        = s;
    n->created_gen = bump_gen();
    n->next        = g_slots;
    g_slots        = n;
    g_slot_count++;

    struct nx_graph_event ev = {
        .type = NX_EV_SLOT_CREATED,
        .u.slot = { .name = s->name },
    };
    emit_event(&ev);
    return NX_OK;
}

int nx_slot_swap(struct nx_slot *s, struct nx_component *new_impl)
{
    struct slot_node *sn = slot_node_for(s);
    if (!sn) return NX_ENOENT;
    if (new_impl && !component_node_for(new_impl)) return NX_ENOENT;

    struct nx_component *old_impl = s->active;
    s->active = new_impl;
    bump_gen();

    struct nx_graph_event ev = {
        .type = NX_EV_SLOT_SWAPPED,
        .u.swap = {
            .slot_name    = s->name,
            .old_manifest = old_impl ? old_impl->manifest_id : NULL,
            .old_instance = old_impl ? old_impl->instance_id : NULL,
            .new_manifest = new_impl ? new_impl->manifest_id : NULL,
            .new_instance = new_impl ? new_impl->instance_id : NULL,
        },
    };
    emit_event(&ev);
    return NX_OK;
}

int nx_slot_set_fallback(struct nx_slot *s, struct nx_slot *fallback)
{
    struct slot_node *sn = slot_node_for(s);
    if (!sn) return NX_ENOENT;
    if (fallback == s) return NX_EINVAL;
    if (fallback && !slot_node_for(fallback)) return NX_ENOENT;
    s->fallback = fallback;
    return NX_OK;
}

int nx_slot_set_pause_state(struct nx_slot *s, enum nx_slot_pause_state st)
{
    if (!slot_node_for(s)) return NX_ENOENT;
    atomic_store_explicit(&s->pause_state, st, memory_order_release);
    return NX_OK;
}

enum nx_slot_pause_state nx_slot_pause_state(const struct nx_slot *s)
{
    if (!s) return NX_SLOT_PAUSE_NONE;
    /* Casting-away-const is sound: atomic_load reads only. */
    return atomic_load_explicit(
        (_Atomic(enum nx_slot_pause_state) *)&s->pause_state,
        memory_order_acquire);
}

int nx_slot_unregister(struct nx_slot *s)
{
    struct slot_node *sn = slot_node_for(s);
    if (!sn) return NX_ENOENT;
    /* Refuse if any edge still references this slot — caller must tear
     * down the wiring first. */
    if (sn->incoming || sn->outgoing) return NX_EBUSY;

    struct slot_node **pp = &g_slots;
    while (*pp && *pp != sn) pp = &(*pp)->next;
    if (*pp) *pp = sn->next;

    const char *name = s->name;
    free(sn);
    g_slot_count--;
    bump_gen();

    struct nx_graph_event ev = {
        .type   = NX_EV_SLOT_DESTROYED,
        .u.slot = { .name = name },
    };
    emit_event(&ev);
    return NX_OK;
}

/* ---------- Component mutation API --------------------------------------- */

int nx_component_register(struct nx_component *c)
{
    if (!c || !c->manifest_id || !c->instance_id) return NX_EINVAL;
    if (component_node_find(c->manifest_id, c->instance_id)) return NX_EEXIST;

    struct component_node *n = calloc(1, sizeof *n);
    if (!n) return NX_ENOMEM;

    n->comp        = c;
    n->created_gen = bump_gen();
    n->next        = g_components;
    g_components   = n;
    g_component_count++;

    struct nx_graph_event ev = {
        .type = NX_EV_COMPONENT_REGISTERED,
        .u.comp = {
            .manifest_id = c->manifest_id,
            .instance_id = c->instance_id,
        },
    };
    emit_event(&ev);
    return NX_OK;
}

int nx_component_unregister(struct nx_component *c)
{
    struct component_node *cn = component_node_for(c);
    if (!cn) return NX_ENOENT;
    if (nx_component_is_bound(c)) return NX_EBUSY;

    struct component_node **pp = &g_components;
    while (*pp && *pp != cn) pp = &(*pp)->next;
    if (*pp) *pp = cn->next;

    const char *manifest = c->manifest_id;
    const char *instance = c->instance_id;
    free(cn);
    g_component_count--;
    bump_gen();

    struct nx_graph_event ev = {
        .type = NX_EV_COMPONENT_UNREGISTERED,
        .u.comp = { .manifest_id = manifest, .instance_id = instance },
    };
    emit_event(&ev);
    return NX_OK;
}

int nx_component_state_set(struct nx_component *c, enum nx_lifecycle_state s)
{
    if (!component_node_for(c)) return NX_ENOENT;

    enum nx_lifecycle_state old = c->state;
    c->state = s;
    bump_gen();

    struct nx_graph_event ev = {
        .type = NX_EV_COMPONENT_STATE,
        .u.state = {
            .manifest_id = c->manifest_id,
            .instance_id = c->instance_id,
            .from = old,
            .to   = s,
        },
    };
    emit_event(&ev);
    return NX_OK;
}

/* ---------- Connection mutation API -------------------------------------- */

struct nx_connection *nx_connection_register(struct nx_slot *from,
                                             struct nx_slot *to,
                                             enum nx_conn_mode mode,
                                             bool stateful,
                                             enum nx_pause_policy policy,
                                             int *err)
{
    int dummy;
    if (!err) err = &dummy;

    if (!to)                   { *err = NX_EINVAL; return NULL; }
    struct slot_node *to_sn   = slot_node_for(to);
    if (!to_sn)                { *err = NX_ENOENT; return NULL; }
    struct slot_node *from_sn = NULL;
    if (from) {
        from_sn = slot_node_for(from);
        if (!from_sn)          { *err = NX_ENOENT; return NULL; }
    }

    struct nx_connection *c = calloc(1, sizeof *c);
    struct conn_node     *n = calloc(1, sizeof *n);
    if (!c || !n) {
        free(c); free(n);
        *err = NX_ENOMEM;
        return NULL;
    }

    c->from_slot     = from;
    c->to_slot       = to;
    c->mode          = mode;
    c->stateful      = stateful;
    c->policy        = policy;
    c->installed_gen = bump_gen();

    n->conn = c;
    n->next = g_connections;
    g_connections = n;
    g_connection_count++;

    slot_add_incoming(to_sn, n);
    if (from_sn) slot_add_outgoing(from_sn, n);

    struct nx_graph_event ev = {
        .type = NX_EV_CONNECTION_ADDED,
        .u.conn = {
            .from_slot = from ? from->name : NULL,
            .to_slot   = to->name,
            .mode      = mode,
            .stateful  = stateful,
        },
    };
    emit_event(&ev);

    *err = NX_OK;
    return c;
}

int nx_connection_retune(struct nx_connection *c,
                         enum nx_conn_mode mode, bool stateful)
{
    if (!c) return NX_EINVAL;
    /* Membership check: caller must hold a registered connection. */
    bool found = false;
    for (struct conn_node *n = g_connections; n; n = n->next) {
        if (n->conn == c) { found = true; break; }
    }
    if (!found) return NX_ENOENT;

    c->mode     = mode;
    c->stateful = stateful;
    bump_gen();

    struct nx_graph_event ev = {
        .type = NX_EV_CONNECTION_RETUNED,
        .u.conn = {
            .from_slot = c->from_slot ? c->from_slot->name : NULL,
            .to_slot   = c->to_slot   ? c->to_slot->name   : NULL,
            .mode      = mode,
            .stateful  = stateful,
        },
    };
    emit_event(&ev);
    return NX_OK;
}

int nx_connection_unregister(struct nx_connection *c)
{
    if (!c) return NX_EINVAL;

    struct conn_node **pp = &g_connections;
    while (*pp && (*pp)->conn != c) pp = &(*pp)->next;
    if (!*pp) return NX_ENOENT;
    struct conn_node *n = *pp;
    *pp = n->next;

    struct slot_node *to_sn = slot_node_for(c->to_slot);
    if (to_sn) slot_remove_incoming(to_sn, n);
    struct slot_node *from_sn = slot_node_for(c->from_slot);
    if (from_sn) slot_remove_outgoing(from_sn, n);

    /* Capture names before freeing the connection. */
    const char       *from_name = c->from_slot ? c->from_slot->name : NULL;
    const char       *to_name   = c->to_slot   ? c->to_slot->name   : NULL;
    enum nx_conn_mode mode      = c->mode;
    bool              stateful  = c->stateful;

    free(c);
    free(n);
    g_connection_count--;
    bump_gen();

    struct nx_graph_event ev = {
        .type = NX_EV_CONNECTION_REMOVED,
        .u.conn = {
            .from_slot = from_name,
            .to_slot   = to_name,
            .mode      = mode,
            .stateful  = stateful,
        },
    };
    emit_event(&ev);
    return NX_OK;
}

/* ---------- Lookup -------------------------------------------------------- */

struct nx_slot *nx_slot_lookup(const char *name)
{
    struct slot_node *n = slot_node_find(name);
    return n ? n->slot : NULL;
}

struct nx_component *nx_component_lookup(const char *manifest_id,
                                         const char *instance_id)
{
    struct component_node *n = component_node_find(manifest_id, instance_id);
    return n ? n->comp : NULL;
}

uint64_t nx_graph_generation(void)       { return g_generation; }
size_t   nx_graph_slot_count(void)       { return g_slot_count; }
size_t   nx_graph_component_count(void)  { return g_component_count; }
size_t   nx_graph_connection_count(void) { return g_connection_count; }

/* ---------- Traversal ----------------------------------------------------- */

void nx_graph_foreach_slot(nx_slot_cb cb, void *ctx)
{
    for (struct slot_node *n = g_slots; n; n = n->next)
        cb(n->slot, ctx);
}

void nx_graph_foreach_component(nx_component_cb cb, void *ctx)
{
    for (struct component_node *n = g_components; n; n = n->next)
        cb(n->comp, ctx);
}

void nx_graph_foreach_connection(nx_connection_cb cb, void *ctx)
{
    for (struct conn_node *n = g_connections; n; n = n->next)
        cb(n->conn, ctx);
}

void nx_slot_foreach_dependent(struct nx_slot *s, nx_connection_cb cb, void *ctx)
{
    struct slot_node *sn = slot_node_for(s);
    if (!sn) return;
    for (struct conn_node *n = sn->incoming; n; n = n->next_in)
        cb(n->conn, ctx);
}

void nx_slot_foreach_dependency(struct nx_slot *s, nx_connection_cb cb, void *ctx)
{
    struct slot_node *sn = slot_node_for(s);
    if (!sn) return;
    for (struct conn_node *n = sn->outgoing; n; n = n->next_out)
        cb(n->conn, ctx);
}

/* ---------- Snapshot ----------------------------------------------------- */

struct nx_graph_snapshot {
    int                            refcount;
    uint64_t                       generation;
    uint64_t                       timestamp_ns;
    size_t                         slot_count;
    size_t                         component_count;
    size_t                         connection_count;
    struct nx_snapshot_slot       *slots;
    struct nx_snapshot_component  *components;
    struct nx_snapshot_connection *connections;
};

struct nx_graph_snapshot *nx_graph_snapshot_take(void)
{
    struct nx_graph_snapshot *s = calloc(1, sizeof *s);
    if (!s) return NULL;

    s->refcount         = 1;
    s->generation       = g_generation;
    s->timestamp_ns     = now_ns();
    s->slot_count       = g_slot_count;
    s->component_count  = g_component_count;
    s->connection_count = g_connection_count;

    if (s->slot_count) {
        s->slots = calloc(s->slot_count, sizeof *s->slots);
        if (!s->slots) goto fail;
    }
    if (s->component_count) {
        s->components = calloc(s->component_count, sizeof *s->components);
        if (!s->components) goto fail;
    }
    if (s->connection_count) {
        s->connections = calloc(s->connection_count, sizeof *s->connections);
        if (!s->connections) goto fail;
    }

    size_t i = 0;
    for (struct slot_node *n = g_slots; n; n = n->next) {
        struct nx_snapshot_slot *out = &s->slots[i++];
        out->name        = n->slot->name;
        out->iface       = n->slot->iface;
        out->mutability  = n->slot->mutability;
        out->concurrency = n->slot->concurrency;
        if (n->slot->active) {
            out->active_manifest = n->slot->active->manifest_id;
            out->active_instance = n->slot->active->instance_id;
        }
    }

    i = 0;
    for (struct component_node *n = g_components; n; n = n->next) {
        struct nx_snapshot_component *out = &s->components[i++];
        out->manifest_id = n->comp->manifest_id;
        out->instance_id = n->comp->instance_id;
        out->state       = n->comp->state;
    }

    i = 0;
    for (struct conn_node *n = g_connections; n; n = n->next) {
        struct nx_snapshot_connection *out = &s->connections[i++];
        out->from_slot     = n->conn->from_slot ? n->conn->from_slot->name : NULL;
        out->to_slot       = n->conn->to_slot   ? n->conn->to_slot->name   : NULL;
        out->mode          = n->conn->mode;
        out->stateful      = n->conn->stateful;
        out->policy        = n->conn->policy;
        out->installed_gen = n->conn->installed_gen;
    }

    return s;

fail:
    free(s->slots);
    free(s->components);
    free(s->connections);
    free(s);
    return NULL;
}

void nx_graph_snapshot_retain(struct nx_graph_snapshot *s)
{
    if (s) s->refcount++;
}

void nx_graph_snapshot_put(struct nx_graph_snapshot *s)
{
    if (!s) return;
    if (--s->refcount > 0) return;
    free(s->slots);
    free(s->components);
    free(s->connections);
    free(s);
}

uint64_t nx_graph_snapshot_generation(const struct nx_graph_snapshot *s)
{ return s ? s->generation : 0; }

uint64_t nx_graph_snapshot_timestamp_ns(const struct nx_graph_snapshot *s)
{ return s ? s->timestamp_ns : 0; }

size_t nx_graph_snapshot_slot_count(const struct nx_graph_snapshot *s)
{ return s ? s->slot_count : 0; }

size_t nx_graph_snapshot_component_count(const struct nx_graph_snapshot *s)
{ return s ? s->component_count : 0; }

size_t nx_graph_snapshot_connection_count(const struct nx_graph_snapshot *s)
{ return s ? s->connection_count : 0; }

const struct nx_snapshot_slot *
nx_graph_snapshot_slot(const struct nx_graph_snapshot *s, size_t i)
{
    if (!s || i >= s->slot_count) return NULL;
    return &s->slots[i];
}

const struct nx_snapshot_component *
nx_graph_snapshot_component(const struct nx_graph_snapshot *s, size_t i)
{
    if (!s || i >= s->component_count) return NULL;
    return &s->components[i];
}

const struct nx_snapshot_connection *
nx_graph_snapshot_connection(const struct nx_graph_snapshot *s, size_t i)
{
    if (!s || i >= s->connection_count) return NULL;
    return &s->connections[i];
}

/* ---------- JSON serialization ------------------------------------------ */

/*
 * Tiny appender that tracks remaining buffer space.  All string sources
 * (slot/component names, manifest IDs, etc.) are caller-trusted — the
 * registry doesn't accept arbitrary user strings, so JSON-escaping is
 * limited to backslash, double-quote, and control characters.  Future
 * gen-config will validate identifier shape upstream.
 */

struct json_buf {
    char  *buf;
    size_t cap;
    size_t pos;
    bool   truncated;
};

static void jb_init(struct json_buf *jb, char *buf, size_t cap)
{
    jb->buf       = buf;
    jb->cap       = cap;
    jb->pos       = 0;
    jb->truncated = false;
    if (cap) buf[0] = '\0';
}

static void jb_putc(struct json_buf *jb, char c)
{
    if (jb->pos + 1 < jb->cap) {
        jb->buf[jb->pos++] = c;
        jb->buf[jb->pos]   = '\0';
    } else {
        jb->truncated = true;
    }
}

static void jb_puts(struct json_buf *jb, const char *s)
{
    while (*s) jb_putc(jb, *s++);
}

static void jb_puts_escaped(struct json_buf *jb, const char *s)
{
    if (!s) { jb_puts(jb, "null"); return; }
    jb_putc(jb, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  jb_puts(jb, "\\\""); break;
        case '\\': jb_puts(jb, "\\\\"); break;
        case '\n': jb_puts(jb, "\\n");  break;
        case '\r': jb_puts(jb, "\\r");  break;
        case '\t': jb_puts(jb, "\\t");  break;
        default:
            if (c < 0x20) {
                char tmp[8];
                snprintf(tmp, sizeof tmp, "\\u%04x", c);
                jb_puts(jb, tmp);
            } else {
                jb_putc(jb, (char)c);
            }
        }
    }
    jb_putc(jb, '"');
}

static void jb_putu64(struct json_buf *jb, uint64_t v)
{
    char tmp[24];
    snprintf(tmp, sizeof tmp, "%llu", (unsigned long long)v);
    jb_puts(jb, tmp);
}

static const char *mutability_str(enum nx_slot_mutability m)
{
    switch (m) {
    case NX_MUT_HOT:    return "hot";
    case NX_MUT_WARM:   return "warm";
    case NX_MUT_FROZEN: return "frozen";
    }
    return "unknown";
}

static const char *concurrency_str(enum nx_slot_concurrency c)
{
    switch (c) {
    case NX_CONC_SHARED:     return "shared";
    case NX_CONC_SERIALIZED: return "serialized";
    case NX_CONC_PER_CPU:    return "per_cpu";
    case NX_CONC_DEDICATED:  return "dedicated";
    }
    return "unknown";
}

static const char *mode_str(enum nx_conn_mode m)
{
    return m == NX_CONN_SYNC ? "sync" : "async";
}

static const char *policy_str(enum nx_pause_policy p)
{
    switch (p) {
    case NX_PAUSE_QUEUE:    return "queue";
    case NX_PAUSE_REJECT:   return "reject";
    case NX_PAUSE_REDIRECT: return "redirect";
    }
    return "unknown";
}

static const char *lifecycle_str(enum nx_lifecycle_state s)
{
    switch (s) {
    case NX_LC_UNINIT:    return "uninit";
    case NX_LC_INIT:      return "init";
    case NX_LC_READY:     return "ready";
    case NX_LC_ACTIVE:    return "active";
    case NX_LC_PAUSED:    return "paused";
    case NX_LC_DESTROYED: return "destroyed";
    }
    return "unknown";
}

static const char *event_type_str(enum nx_graph_event_type t)
{
    switch (t) {
    case NX_EV_SLOT_CREATED:           return "slot_created";
    case NX_EV_SLOT_DESTROYED:         return "slot_destroyed";
    case NX_EV_SLOT_SWAPPED:           return "slot_swapped";
    case NX_EV_COMPONENT_REGISTERED:   return "component_registered";
    case NX_EV_COMPONENT_UNREGISTERED: return "component_unregistered";
    case NX_EV_COMPONENT_STATE:        return "component_state";
    case NX_EV_CONNECTION_ADDED:       return "connection_added";
    case NX_EV_CONNECTION_REMOVED:     return "connection_removed";
    case NX_EV_CONNECTION_RETUNED:     return "connection_retuned";
    }
    return "unknown";
}

int nx_graph_snapshot_to_json(const struct nx_graph_snapshot *s,
                              char *buf, size_t buflen)
{
    if (!s || !buf || buflen == 0) return NX_EINVAL;

    struct json_buf jb;
    jb_init(&jb, buf, buflen);

    jb_puts(&jb, "{\"generation\":");
    jb_putu64(&jb, s->generation);
    jb_puts(&jb, ",\"timestamp_ns\":");
    jb_putu64(&jb, s->timestamp_ns);

    jb_puts(&jb, ",\"slots\":[");
    for (size_t i = 0; i < s->slot_count; i++) {
        const struct nx_snapshot_slot *sl = &s->slots[i];
        if (i) jb_putc(&jb, ',');
        jb_puts(&jb, "{\"name\":");      jb_puts_escaped(&jb, sl->name);
        jb_puts(&jb, ",\"iface\":");     jb_puts_escaped(&jb, sl->iface);
        jb_puts(&jb, ",\"mutability\":"); jb_puts_escaped(&jb, mutability_str(sl->mutability));
        jb_puts(&jb, ",\"concurrency\":"); jb_puts_escaped(&jb, concurrency_str(sl->concurrency));
        jb_puts(&jb, ",\"active\":");
        if (sl->active_manifest) {
            jb_puts(&jb, "{\"manifest\":"); jb_puts_escaped(&jb, sl->active_manifest);
            jb_puts(&jb, ",\"instance\":"); jb_puts_escaped(&jb, sl->active_instance);
            jb_puts(&jb, "}");
        } else {
            jb_puts(&jb, "null");
        }
        jb_puts(&jb, "}");
    }
    jb_puts(&jb, "]");

    jb_puts(&jb, ",\"components\":[");
    for (size_t i = 0; i < s->component_count; i++) {
        const struct nx_snapshot_component *c = &s->components[i];
        if (i) jb_putc(&jb, ',');
        jb_puts(&jb, "{\"manifest\":"); jb_puts_escaped(&jb, c->manifest_id);
        jb_puts(&jb, ",\"instance\":"); jb_puts_escaped(&jb, c->instance_id);
        jb_puts(&jb, ",\"state\":");    jb_puts_escaped(&jb, lifecycle_str(c->state));
        jb_puts(&jb, "}");
    }
    jb_puts(&jb, "]");

    jb_puts(&jb, ",\"connections\":[");
    for (size_t i = 0; i < s->connection_count; i++) {
        const struct nx_snapshot_connection *cn = &s->connections[i];
        if (i) jb_putc(&jb, ',');
        jb_puts(&jb, "{\"from\":");          jb_puts_escaped(&jb, cn->from_slot);
        jb_puts(&jb, ",\"to\":");            jb_puts_escaped(&jb, cn->to_slot);
        jb_puts(&jb, ",\"mode\":");          jb_puts_escaped(&jb, mode_str(cn->mode));
        jb_puts(&jb, ",\"stateful\":");      jb_puts(&jb, cn->stateful ? "true" : "false");
        jb_puts(&jb, ",\"policy\":");        jb_puts_escaped(&jb, policy_str(cn->policy));
        jb_puts(&jb, ",\"installed_gen\":"); jb_putu64(&jb, cn->installed_gen);
        jb_puts(&jb, "}");
    }
    jb_puts(&jb, "]}");

    if (jb.truncated) return NX_ENOMEM;
    return (int)jb.pos;
}

static void emit_event_json(struct json_buf *jb, const struct nx_graph_event *ev)
{
    jb_puts(jb, "{\"type\":");         jb_puts_escaped(jb, event_type_str(ev->type));
    jb_puts(jb, ",\"generation\":");   jb_putu64(jb, ev->generation);
    jb_puts(jb, ",\"timestamp_ns\":"); jb_putu64(jb, ev->timestamp_ns);

    switch (ev->type) {
    case NX_EV_SLOT_CREATED:
    case NX_EV_SLOT_DESTROYED:
        jb_puts(jb, ",\"slot\":"); jb_puts_escaped(jb, ev->u.slot.name);
        break;
    case NX_EV_SLOT_SWAPPED:
        jb_puts(jb, ",\"slot\":");          jb_puts_escaped(jb, ev->u.swap.slot_name);
        jb_puts(jb, ",\"old_manifest\":");  jb_puts_escaped(jb, ev->u.swap.old_manifest);
        jb_puts(jb, ",\"old_instance\":");  jb_puts_escaped(jb, ev->u.swap.old_instance);
        jb_puts(jb, ",\"new_manifest\":");  jb_puts_escaped(jb, ev->u.swap.new_manifest);
        jb_puts(jb, ",\"new_instance\":");  jb_puts_escaped(jb, ev->u.swap.new_instance);
        break;
    case NX_EV_COMPONENT_REGISTERED:
    case NX_EV_COMPONENT_UNREGISTERED:
        jb_puts(jb, ",\"manifest\":"); jb_puts_escaped(jb, ev->u.comp.manifest_id);
        jb_puts(jb, ",\"instance\":"); jb_puts_escaped(jb, ev->u.comp.instance_id);
        break;
    case NX_EV_COMPONENT_STATE:
        jb_puts(jb, ",\"manifest\":"); jb_puts_escaped(jb, ev->u.state.manifest_id);
        jb_puts(jb, ",\"instance\":"); jb_puts_escaped(jb, ev->u.state.instance_id);
        jb_puts(jb, ",\"from\":");     jb_puts_escaped(jb, lifecycle_str(ev->u.state.from));
        jb_puts(jb, ",\"to\":");       jb_puts_escaped(jb, lifecycle_str(ev->u.state.to));
        break;
    case NX_EV_CONNECTION_ADDED:
    case NX_EV_CONNECTION_REMOVED:
    case NX_EV_CONNECTION_RETUNED:
        jb_puts(jb, ",\"from\":");      jb_puts_escaped(jb, ev->u.conn.from_slot);
        jb_puts(jb, ",\"to\":");        jb_puts_escaped(jb, ev->u.conn.to_slot);
        jb_puts(jb, ",\"mode\":");      jb_puts_escaped(jb, mode_str(ev->u.conn.mode));
        jb_puts(jb, ",\"stateful\":");  jb_puts(jb, ev->u.conn.stateful ? "true" : "false");
        break;
    }
    jb_puts(jb, "}");
}

int nx_change_log_to_json(char *buf, size_t buflen)
{
    if (!buf || buflen == 0) return NX_EINVAL;

    struct json_buf jb;
    jb_init(&jb, buf, buflen);

    uint64_t total = nx_change_log_total();
    uint64_t held  = total < NX_CHANGE_LOG_CAPACITY ? total
                                                    : NX_CHANGE_LOG_CAPACITY;
    uint64_t oldest_idx = total - held;

    jb_puts(&jb, "{\"total\":"); jb_putu64(&jb, total);
    jb_puts(&jb, ",\"held\":");  jb_putu64(&jb, held);
    jb_puts(&jb, ",\"events\":[");
    bool first = true;
    for (uint64_t i = oldest_idx; i < total; i++) {
        if (!first) jb_putc(&jb, ',');
        first = false;
        emit_event_json(&jb, &g_log_ring[i % NX_CHANGE_LOG_CAPACITY]);
    }
    jb_puts(&jb, "]}");

    if (jb.truncated) return NX_ENOMEM;
    return (int)jb.pos;
}

/* ---------- Wholesale reset (test setup) --------------------------------- */

void nx_graph_reset(void)
{
    for (struct conn_node *n = g_connections; n; ) {
        struct conn_node *next = n->next;
        free(n->conn);
        free(n);
        n = next;
    }
    g_connections = NULL;
    g_connection_count = 0;

    /* Do NOT dereference n->slot here — caller-owned storage may already be
     * gone (e.g. test-local stack vars).  Reset only the registry's own
     * bookkeeping. */
    for (struct slot_node *n = g_slots; n; ) {
        struct slot_node *next = n->next;
        free(n);
        n = next;
    }
    g_slots = NULL;
    g_slot_count = 0;

    for (struct component_node *n = g_components; n; ) {
        struct component_node *next = n->next;
        free(n);
        n = next;
    }
    g_components = NULL;
    g_component_count = 0;

    g_generation = 0;

    /* Subscribers + change log: a fresh test starts with an empty ring
     * and no subscribers. */
    for (int i = 0; i < NX_MAX_SUBSCRIBERS; i++)
        g_subscribers[i].in_use = false;
    nx_change_log_reset();
}
