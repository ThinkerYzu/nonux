#include "framework/registry.h"

#include <stdlib.h>
#include <string.h>

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

/* True if any registered slot's `active` is `c`. */
static bool component_is_active_anywhere(const struct nx_component *c)
{
    for (struct slot_node *n = g_slots; n; n = n->next) {
        if (n->slot->active == c) return true;
    }
    return false;
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

    n->slot        = s;
    n->created_gen = bump_gen();
    n->next        = g_slots;
    g_slots        = n;
    g_slot_count++;
    return NX_OK;
}

int nx_slot_swap(struct nx_slot *s, struct nx_component *new_impl)
{
    struct slot_node *sn = slot_node_for(s);
    if (!sn) return NX_ENOENT;
    if (new_impl && !component_node_for(new_impl)) return NX_ENOENT;

    s->active = new_impl;
    bump_gen();
    return NX_OK;
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

    free(sn);
    g_slot_count--;
    bump_gen();
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
    return NX_OK;
}

int nx_component_unregister(struct nx_component *c)
{
    struct component_node *cn = component_node_for(c);
    if (!cn) return NX_ENOENT;
    if (component_is_active_anywhere(c)) return NX_EBUSY;

    struct component_node **pp = &g_components;
    while (*pp && *pp != cn) pp = &(*pp)->next;
    if (*pp) *pp = cn->next;

    free(cn);
    g_component_count--;
    bump_gen();
    return NX_OK;
}

int nx_component_state_set(struct nx_component *c, enum nx_lifecycle_state s)
{
    if (!component_node_for(c)) return NX_ENOENT;
    c->state = s;
    bump_gen();
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

    free(c);
    free(n);
    g_connection_count--;
    bump_gen();
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
}
