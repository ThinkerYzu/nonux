#ifndef NX_FRAMEWORK_IPC_H
#define NX_FRAMEWORK_IPC_H

#include "framework/registry.h"
#include "core/lib/mpsc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * IPC router — Phase 3 slice 3.6.
 *
 * Inter-component communication.  The component code is identical for
 * sync and async modes — it just calls `nx_ipc_send()`.  The router
 * consults the registered connection edge from `src_slot` to `dst_slot`
 * and either dispatches directly (SYNC) or enqueues onto the
 * destination slot's inbox (ASYNC) for later draining.
 *
 * Host-side v1: the async inbox is a plain singly-linked FIFO per
 * slot, and `nx_ipc_dispatch` is an explicit drain helper for tests.
 * Kernel-side (slice 3.9+) replaces the inbox with an MPSC lock-free
 * queue plus a per-CPU dispatcher thread — same routing contract.
 */

/* ---------- Capabilities in messages --------------------------------- */

enum nx_cap_kind {
    NX_CAP_SLOT_REF = 1,
    /* NX_CAP_HANDLE, NX_CAP_VMO_CHUNK — added alongside Phase 5/7 */
};

enum nx_cap_ownership {
    NX_CAP_BORROW = 0,          /* default — valid only during handler */
    NX_CAP_TRANSFER,            /* receiver must claim or drop */
};

/*
 * Typed capability attached to a message.  The router scans every cap
 * on send and receive — see nx_ipc_scan_send_caps / _recv_caps.  Slot
 * refs travel here, NOT inside `payload[]`, so the router can enforce
 * retention and registration generically.
 */
struct nx_ipc_cap {
    enum nx_cap_kind      kind;
    enum nx_cap_ownership ownership;
    union {
        struct nx_slot   *slot_ref;
    } u;
    uint32_t              cap_id;        /* stable id within this msg */
    bool                  claimed;       /* set by nx_slot_ref_retain */
};

/* ---------- Message format ------------------------------------------- */

#define NX_MSG_FLAG_REPLY   (1u << 0)
#define NX_MSG_FLAG_ONEWAY  (1u << 1)

struct nx_ipc_message {
    struct nx_slot        *src_slot;     /* sender; must be registered */
    struct nx_slot        *dst_slot;     /* receiver; active impl dispatches */
    uint32_t               msg_type;
    uint32_t               flags;
    uint32_t               n_caps;
    struct nx_ipc_cap     *caps;         /* may be NULL if n_caps == 0 */
    uint32_t               payload_len;
    const void            *payload;

    /* Framework-owned link fields.  A message is on exactly one list
     * at a time:
     *   - `_next`    — per-slot inbox (host async path) and per-edge
     *                  hold queue (pause-protocol buffering).
     *   - `disp_node` — framework dispatcher's MPSC (slice 3.9b.1,
     *                   kernel async path).
     * Callers must not touch either. */
    struct nx_ipc_message *_next;
    struct nx_mpsc_node    disp_node;
};

/* ---------- Router public API ---------------------------------------- */

/*
 * Send a message.  `msg->src_slot` and `msg->dst_slot` must both be
 * registered and must be connected by a registered edge — the router
 * uses that edge's `mode` to decide sync vs async.
 *
 * Also runs `nx_ipc_scan_send_caps` first: every NX_CAP_SLOT_REF in
 * `msg->caps` must correspond to an outgoing connection the sender
 * actually holds (no forged caps).
 *
 * Returns:
 *   NX_OK        on success (sync: handler already returned; async:
 *                queued for later dispatch)
 *   NX_EINVAL    NULL msg / missing src or dst / forged cap
 *   NX_ENOENT    no registered edge from src to dst, or dst has no
 *                active impl / descriptor / ops->handle_msg (sync)
 *   NX_ENOMEM    queue allocation failed (async)
 */
int nx_ipc_send(struct nx_ipc_message *msg);

/*
 * Drain up to `max` messages from `slot`'s inbox, invoking the active
 * component's `ops->handle_msg(impl, msg)` for each.  After the handler
 * returns, the router runs `nx_ipc_scan_recv_caps` and then frees the
 * queue node (not the `nx_ipc_message` itself — callers own that
 * storage).
 *
 * Returns the number of messages dispatched (0..max).  Stops early if
 * the inbox is empty or `handle_msg` returns nonzero.
 *
 * **Host-only since slice 3.9b.1.**  On the kernel build, async messages
 * are owned by the framework dispatcher kthread (see framework/
 * dispatcher.h) and this helper is a no-op returning 0.  Host tests
 * retain the manual-pump contract.
 */
size_t nx_ipc_dispatch(struct nx_slot *slot, size_t max);

/*
 * ISR-safe enqueue entry point (slice 3.9b.1).  Interrupt handlers
 * that need to hand a pre-built `nx_ipc_message` off to a component
 * call this instead of `nx_ipc_send` — the function dereferences no
 * slot, takes no lock, allocates nothing, and returns in a bounded
 * handful of instructions.  The dispatcher kthread picks the
 * message up on its next iteration and runs the IPC_RECV hook +
 * `handle_msg` on a dispatcher-equivalent context where R8's
 * slot-resolve-locality invariant holds.
 *
 * `msg->src_slot` may be NULL for boot/external edges; `msg->dst_slot`
 * must be non-NULL — the dispatcher uses it to resolve the active
 * impl.  Caller owns `msg` storage and must keep it alive until the
 * dispatcher has consumed it (easiest: allocate from a pre-built pool).
 *
 * Returns NX_OK on enqueue or NX_EINVAL for a NULL msg / dst_slot.
 * Not called on the host build (no dispatcher); on host this is a
 * NX_EINVAL-returning stub so tests can't accidentally depend on it.
 */
int nx_ipc_enqueue_from_irq(struct nx_ipc_message *msg);

/* Number of messages currently queued on `slot`'s inbox. */
size_t nx_ipc_inbox_depth(struct nx_slot *slot);

/* Number of messages currently held for the (`src`, `dst`) edge under the
 * NX_PAUSE_QUEUE policy.  `src == NULL` counts held messages where the
 * sender was a boot/external edge.  Zero for non-existent entries. */
size_t nx_ipc_hold_queue_depth(struct nx_slot *src, struct nx_slot *dst);

/*
 * Flush every held message for `dst` back through the router.  Called from
 * the pause protocol's resume path after the slot's pause_state drops to
 * NX_SLOT_PAUSE_NONE — held messages then route normally (sync or async,
 * per the edge's mode).  Safe to call with no pending items (no-op).
 */
void   nx_ipc_flush_hold_queue(struct nx_slot *dst);

/* Drop every queued message for every slot and free every queue head.
 * Test-only — called from nx_graph_reset() equivalents. */
void   nx_ipc_reset(void);

/* ---------- Cap scanning --------------------------------------------- */

/*
 * Validate that the sender legitimately holds every slot_ref cap it
 * passes.  "Holds" = there is a registered connection edge
 * (sender → cap->u.slot_ref) in the graph.
 *
 * Returns NX_OK or NX_EINVAL on the first forged cap.  Safe to call
 * repeatedly; it has no side effects.
 */
int nx_ipc_scan_send_caps(struct nx_slot *sender,
                          struct nx_ipc_message *msg);

/*
 * Post-handler cap sweep.  For each unclaimed cap:
 *   - BORROW   → silently dropped (expected lifecycle)
 *   - TRANSFER → protocol error; count is returned
 *
 * A cap is "claimed" when the receiver called `nx_slot_ref_retain` on
 * it (or otherwise set `claimed = true`).
 *
 * Returns the number of unclaimed TRANSFER caps (0 on a clean sweep).
 */
size_t nx_ipc_scan_recv_caps(struct nx_slot *recv,
                             struct nx_ipc_message *msg);

/* ---------- slot_ref_retain / _release ------------------------------- */

/*
 * Promote a received slot_ref cap into a registered connection edge
 * owned by the receiving slot.  Must be called from inside the handler
 * (before it returns).  Marks `cap->claimed = true` and emits
 * NX_EV_CONNECTION_ADDED.
 *
 * `purpose` is a diagnostic tag — stored only for logs / the registry's
 * change log; may be NULL.
 *
 * Returns NX_OK or:
 *   NX_EINVAL  — cap is NULL, not a SLOT_REF, or already claimed;
 *                `self` or `cap->u.slot_ref` is NULL
 *   NX_ENOENT  — `self` or the target slot is not registered
 *   NX_ENOMEM  — connection edge allocation failed
 *
 * Returns the newly registered connection pointer via `*out_conn`
 * (may be NULL if caller doesn't need it).  Pass that to
 * `nx_slot_ref_release` (or rely on the (self, target) lookup).
 */
int nx_slot_ref_retain(struct nx_slot       *self,
                       struct nx_ipc_cap    *cap,
                       const char           *purpose,
                       struct nx_connection **out_conn);

/*
 * Release a previously retained slot ref.  Walks `self`'s outgoing
 * connections, finds the edge to `target`, and unregisters it.  No-op
 * if the edge is already gone.
 */
void nx_slot_ref_release(struct nx_slot *self, struct nx_slot *target);

#endif /* NX_FRAMEWORK_IPC_H */
