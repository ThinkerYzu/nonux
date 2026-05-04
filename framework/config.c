/*
 * framework/config.c — runtime config manager (slice 8.3).
 *
 * Three public entry points:
 *   nx_config_open()           — allocate a NX_HANDLE_CONFIG handle
 *   nx_config_query_snapshot() — snapshot the live composition
 *   nx_config_swap_component() — single-slot recompose via nx_recompose()
 *
 * The config handle carries no per-instance state in v1.  A static
 * sentinel provides a non-NULL object pointer for nx_handle_alloc.
 * The handle type (NX_HANDLE_CONFIG) is the revocation point.
 */

#include "framework/config.h"
#include "framework/recompose.h"
#include "framework/registry.h"
#include "framework/handle.h"

#if !__STDC_HOSTED__
#include "core/lib/lib.h"   /* memset, memcpy, strlen */
#else
#include <string.h>
#endif

#include <stddef.h>

/* ---- Non-NULL sentinel object for every config handle --------------- */

static int g_config_sentinel = 1;

/* ---- Snapshot traversal -------------------------------------------- */

struct snap_ctx {
    struct nx_config_snapshot *snap;
};

/*
 * Safe copy: copies at most cap-1 bytes from src into dst, always
 * NUL-terminates.  Uses only memcpy + strlen (available in both builds).
 */
static void safe_copy(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void snap_slot_cb(struct nx_slot *s, void *ctx)
{
    struct snap_ctx *sc = (struct snap_ctx *)ctx;
    struct nx_config_snapshot *snap = sc->snap;

    if (snap->num_slots >= NX_CONFIG_SNAPSHOT_MAX_SLOTS)
        return;

    uint32_t i = snap->num_slots;
    safe_copy(snap->slots[i].slot_name, s->name, sizeof(snap->slots[i].slot_name));

    if (s->active) {
        safe_copy(snap->slots[i].impl_name, s->active->manifest_id,
                  sizeof(snap->slots[i].impl_name));
        snap->slots[i].state = (uint32_t)s->active->state;
    } else {
        snap->slots[i].impl_name[0] = '\0';
        snap->slots[i].state = (uint32_t)NX_LC_UNINIT;
    }

    snap->num_slots++;
}

/* ---- Component finder (by manifest_id, NX_LC_READY) ---------------- */

struct find_ctx {
    const char         *manifest_id;
    struct nx_component *found;
};

static int cfg_streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static void find_ready_comp_cb(struct nx_component *c, void *ctx)
{
    struct find_ctx *fc = (struct find_ctx *)ctx;
    if (fc->found)
        return;
    if (cfg_streq(c->manifest_id, fc->manifest_id) && c->state == NX_LC_READY)
        fc->found = c;
}

/* ---- Public API ---------------------------------------------------- */

int nx_config_open(struct nx_handle_table *t, nx_handle_t *out_h)
{
    if (!t || !out_h)
        return NX_EINVAL;
    return nx_handle_alloc(t, NX_HANDLE_CONFIG, NX_RIGHTS_ALL,
                           &g_config_sentinel, out_h);
}

int nx_config_query_snapshot(struct nx_config_snapshot *snap)
{
    if (!snap)
        return NX_EINVAL;

    memset(snap, 0, sizeof(*snap));
    snap->generation = nx_graph_generation();

    struct snap_ctx sc = { .snap = snap };
    nx_graph_foreach_slot(snap_slot_cb, &sc);
    return NX_OK;
}

int nx_config_swap_component(const char *slot_name, const char *new_impl)
{
    if (!slot_name || !new_impl)
        return NX_EINVAL;

    struct nx_slot *slot = nx_slot_lookup(slot_name);
    if (!slot)
        return NX_ENOENT;

    if (slot->mutability == NX_MUT_FROZEN)
        return NX_EPERM;

    struct find_ctx fc = { .manifest_id = new_impl, .found = NULL };
    nx_graph_foreach_component(find_ready_comp_cb, &fc);
    if (!fc.found)
        return NX_ENOENT;

    struct nx_slot_change change = {
        .slot       = slot,
        .action     = NX_SLOT_REPLACE,
        .new_comp   = fc.found,
        .state_lost = false,
    };
    struct recomp_plan plan = {
        .changes         = &change,
        .num_changes     = 1,
        .connections     = NULL,
        .num_connections = 0,
        .timeout_ms      = 0,
    };
    return nx_recompose(&plan);
}

int nx_config_set_conn_mode(const char *from_slot_name,
                             const char *to_slot_name,
                             enum nx_conn_mode mode)
{
    if (!from_slot_name || !to_slot_name) return NX_EINVAL;

    struct nx_slot *from_slot = nx_slot_lookup(from_slot_name);
    if (!from_slot) return NX_ENOENT;

    struct nx_slot *to_slot = nx_slot_lookup(to_slot_name);
    if (!to_slot) return NX_ENOENT;

    struct nx_conn_change cc = {
        .from_slot = from_slot,
        .to_slot   = to_slot,
        .action    = NX_CONN_REWIRE,
        .mode      = mode,
    };
    struct recomp_plan plan = {
        .changes         = NULL,
        .num_changes     = 0,
        .connections     = &cc,
        .num_connections = 1,
        .timeout_ms      = 0,
    };
    return nx_recompose(&plan);
}
