#ifndef NX_FRAMEWORK_HOOK_H
#define NX_FRAMEWORK_HOOK_H

#include "framework/registry.h"

#include <stdbool.h>
#include <stddef.h>

/*
 * Hook framework — Phase 3 slice 3.8.
 *
 * Per-hook-point chains of observers + filters.  The framework dispatches
 * a chain when an event of interest happens (a message about to be sent,
 * a component about to transition, a slot about to swap); each hook sees
 * a typed context and returns CONTINUE (keep dispatching) or ABORT (stop
 * and tell the caller to bail out).
 *
 * Callers own `struct nx_hook` storage.  Priority is an `int`, lower runs
 * first, inserts are priority-ordered.  Registration is O(chain-length);
 * intended scale is observability / tracing, not data-path fan-out.
 *
 * Unregister-during-dispatch is safe: we mark-then-sweep.  If any hook is
 * unregistered while a dispatch is walking the chain, the node stays in
 * place until the outer-most dispatch returns, then it's unlinked.  Hooks
 * may therefore unregister themselves or each other without breaking the
 * iteration.
 */

/* ---------- Hook points --------------------------------------------------- */

enum nx_hook_point {
    NX_HOOK_IPC_SEND = 0,           /* pre-route, before the edge lookup */
    NX_HOOK_IPC_RECV,               /* pre-handler, inside nx_ipc_dispatch */
    NX_HOOK_COMPONENT_ENABLE,       /* around nx_component_enable */
    NX_HOOK_COMPONENT_DISABLE,      /* around nx_component_disable */
    NX_HOOK_COMPONENT_PAUSE,        /* around nx_component_pause */
    NX_HOOK_COMPONENT_RESUME,       /* around nx_component_resume */
    NX_HOOK_SLOT_SWAPPED,           /* piggy-back on NX_EV_SLOT_SWAPPED */

    NX_HOOK_POINT_COUNT,            /* sentinel — keep last */
};

enum nx_hook_action {
    NX_HOOK_CONTINUE = 0,
    NX_HOOK_ABORT,
};

/* ---------- Hook context -------------------------------------------------- */

/*
 * Typed union per hook point.  A single `void *data` would force every
 * hook author to cast blindly; the union gives a typed view with no
 * runtime cost.  The caller populates exactly one arm — the arm that
 * matches `point`.
 */
struct nx_hook_context {
    enum nx_hook_point point;
    union {
        /* IPC_SEND / IPC_RECV */
        struct {
            struct nx_slot         *src;       /* may be NULL for RECV */
            struct nx_slot         *dst;
            struct nx_ipc_message  *msg;
            struct nx_connection   *edge;      /* NULL until routed */
        } ipc;

        /* COMPONENT_ENABLE / _DISABLE / _PAUSE / _RESUME */
        struct {
            struct nx_component     *comp;
            enum nx_lifecycle_state  from;
            enum nx_lifecycle_state  to;
        } comp;

        /* SLOT_SWAPPED — new impl may be NULL for an unbind. */
        struct {
            struct nx_slot      *slot;
            struct nx_component *old_impl;
            struct nx_component *new_impl;
        } swap;
    } u;
};

/* Forward decl — defined in framework/ipc.h. */
struct nx_ipc_message;

/* ---------- Hook nodes ---------------------------------------------------- */

typedef enum nx_hook_action (*nx_hook_fn)(struct nx_hook_context *ctx,
                                          void                   *user);

struct nx_hook {
    enum nx_hook_point point;
    int                priority;    /* lower runs first */
    nx_hook_fn         fn;
    void              *user;
    const char        *name;        /* diagnostic; may be NULL */

    /* Framework-owned.  Callers must not touch. */
    struct nx_hook    *_next;
    bool               _dead;
};

/* ---------- API ----------------------------------------------------------- */

/*
 * Insert `h` into the chain for `h->point`, ordered by priority.  `h`
 * storage is caller-owned and must outlive the registration.
 *
 * Returns NX_OK, NX_EINVAL (NULL h / bad point / NULL fn), or NX_EEXIST
 * if the same `h` pointer is already registered.
 */
int nx_hook_register(struct nx_hook *h);

/*
 * Remove `h` from its chain.  Safe to call from inside the hook's own
 * callback or another hook's callback — the node is marked dead and
 * swept when the outermost dispatch returns.  No-op if `h` is not
 * currently registered.
 */
void nx_hook_unregister(struct nx_hook *h);

/*
 * Walk the chain for `ctx->point` in priority order, calling each live
 * hook.  Returns NX_HOOK_ABORT if any hook returned ABORT (and stops
 * walking), otherwise NX_HOOK_CONTINUE.  Reentrant: hooks may invoke
 * other framework APIs that themselves dispatch hooks.
 */
enum nx_hook_action nx_hook_dispatch(struct nx_hook_context *ctx);

/* Number of currently-live hooks on a given point.  Test helper. */
size_t nx_hook_chain_length(enum nx_hook_point point);

/* Tear down every chain.  Test-only — real kernels never call this. */
void nx_hook_reset(void);

#endif /* NX_FRAMEWORK_HOOK_H */
