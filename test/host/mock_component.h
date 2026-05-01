/*
 * Slice 8.0a.8 — programmable mock component for host unit tests.
 *
 * Provides a self-contained component whose handle_msg records call count,
 * captures the most-recent message pointer, and returns a caller-configured
 * rc — no globals, so two independent instances can coexist in the same test.
 *
 * Usage (all functions are static; include in exactly one .c per TU):
 *
 *   struct mock_handle m;
 *   mock_handle_init(&m, "svc_mock", "0");
 *   nx_component_register(&m.comp);
 *   nx_slot_swap(&slot, &m.comp);
 *   // drive dispatch ...
 *   ASSERT_EQ_U(m.state.call_count, expected);
 *   nx_slot_swap(&slot, NULL);
 *   nx_component_unregister(&m.comp);
 *
 * `mock_handle_reset` zeroes call_count and last_msg without touching
 * handler_rc or custom_fn, so a fresh call sequence can start mid-test.
 */

#ifndef TEST_MOCK_COMPONENT_H
#define TEST_MOCK_COMPONENT_H

#include "framework/component.h"
#include "framework/ipc.h"
#include "framework/registry.h"

#include <string.h>

/* ---- Per-instance state ------------------------------------------------- */

struct mock_state {
    int                    handler_rc;   /* returned from handle_msg */
    int                    call_count;   /* incremented on each call */
    struct nx_ipc_message *last_msg;     /* pointer to most-recent message */

    /* Optional override: called instead of returning handler_rc when set. */
    int (*custom_fn)(struct mock_state *s, struct nx_ipc_message *msg);
};

/* ---- Full mock handle (storage for comp + descriptor + ops) ------------- */

struct mock_handle {
    struct mock_state            state;
    struct nx_component          comp;
    struct nx_component_descriptor desc;
    struct nx_component_ops      ops_storage;
};

/* ---- Implementation (static so multiple TUs can include this header) ---- */

static int mock_handle_msg_fn(void *self, struct nx_ipc_message *msg)
{
    struct mock_state *s = self;
    s->call_count++;
    s->last_msg = msg;
    if (s->custom_fn) return s->custom_fn(s, msg);
    return s->handler_rc;
}

static void mock_handle_init(struct mock_handle *m,
                              const char *manifest_id,
                              const char *instance_id)
{
    memset(m, 0, sizeof *m);
    m->ops_storage.handle_msg = mock_handle_msg_fn;
    m->desc.name        = manifest_id;
    m->desc.ops         = &m->ops_storage;
    m->desc.state_size  = 0;
    m->desc.n_deps      = 0;
    m->desc.deps        = NULL;
    m->comp.manifest_id = manifest_id;
    m->comp.instance_id = instance_id;
    m->comp.state       = NX_LC_ACTIVE;
    m->comp.descriptor  = &m->desc;
    m->comp.impl        = &m->state;   /* self passed to handle_msg */
}

static void mock_handle_reset(struct mock_handle *m)
{
    m->state.call_count = 0;
    m->state.last_msg   = NULL;
    /* handler_rc and custom_fn intentionally preserved */
}

#endif /* TEST_MOCK_COMPONENT_H */
