/*
 * posix_shim — kernel-side syscall-entry boundary component.
 *
 * Slice 8.0a.4 landed the skeleton: manifest, init/enable/disable/
 * destroy stubs, singleton accessor.  Slice 8.0a.6 fills in
 * `posix_shim_handle_msg` so that reply messages dispatched back to a
 * per-task `caller_slot` (slice 8.0a.5) are routed into the originating
 * task's wrapper-allocated reply buffer and the task's `reply_waitq`
 * is woken — closing the round-trip for `nx_slot_call_blocking`
 * (slice 8.0a.6).
 *
 * Background.  DESIGN.md §"Every Component Occupies a Slot" mandates
 * a graph-resident component at the userspace/kernel boundary so that
 * syscall-driven cross-component calls have a well-defined `src_slot`.
 * Today's `framework/syscall.c` reaches into `slot->active->descriptor->
 * iface_ops` directly (~184 sites across 19 files) — this slot lives
 * outside the registry, so caller-side IPC discipline (cap-scan,
 * pause/drain, hooks) bypasses it.
 *
 * Slice 8.0a routes every cross-component call through
 * `nx_slot_call_blocking`; the per-task `caller_slot` (slice 8.0a.5)
 * is the actual sender, and *every* task slot binds to this single
 * `posix_shim` component instance (registry's N→1 binding —
 * concurrency: shared).  The component itself is the receiver of reply
 * messages dispatched back from service slots; `posix_shim_handle_msg`
 * decodes the reply payload (which begins with `struct nx_reply_header`
 * carrying `rc`) into the originating task's kstack reply buffer and
 * wakes its `reply_waitq`.
 *
 * Why mode: async on every dep edge — DESIGN.md §"Sync-mode caller must
 * be on a dispatcher".  Syscall callers run on their own task kstack,
 * not a dispatcher thread; sync-mode would let `slot->active` be read
 * off-dispatcher (R8 violation).  All four edges (vfs, scheduler,
 * memory.page_alloc, char_device.serial) are async; the blocking-call
 * wrapper enqueues the request and parks the caller on
 * `task->reply_waitq` until the dispatcher posts the reply.
 *
 * State.  `g_posix_shim` is the singleton accessor that
 * `framework/syscall.c` will use (post-8.0c migration) to reach deps
 * via `g_posix_shim->deps.<dep>` instead of `nx_slot_lookup(...)`.
 *
 * See SLOT-CALL-API.md §"The posix_shim Component" for the full spec.
 */

#include "framework/component.h"
#include "framework/handle.h"
#include "framework/ipc.h"
#include "framework/registry.h"
#include "framework/slot_call.h"
#include "core/sched/task.h"
#include "core/sched/waitq.h"
#include "gen/posix_shim_deps.h"

#if __STDC_HOSTED__
#include <string.h>
#else
#include "core/lib/lib.h"
#endif

#include <stdbool.h>
#include <stddef.h>

struct posix_shim_state {
    struct posix_shim_deps deps;

    /* Counters observable by host/kernel tests; mirrors the pattern in
     * mm_buddy / uart_pl011 / vfs_simple. */
    unsigned init_called;
    unsigned enable_called;
    unsigned disable_called;
    unsigned destroy_called;
    unsigned messages_handled;
    unsigned replies_routed;
    unsigned reply_truncations;
    unsigned reply_unbound_caller;
};

struct posix_shim_state *g_posix_shim = NULL;

/* Test/diagnostic accessors so tests can read the routing counters
 * without poking the struct directly.  Returns 0 when the component
 * isn't bound (e.g. host tests that haven't run framework_bootstrap). */
unsigned nx_posix_shim_replies_routed_for_test(void)
{
    return g_posix_shim ? g_posix_shim->replies_routed : 0;
}
unsigned nx_posix_shim_reply_truncations_for_test(void)
{
    return g_posix_shim ? g_posix_shim->reply_truncations : 0;
}

static int posix_shim_init(void *self)
{
    struct posix_shim_state *s = self;
    g_posix_shim = s;
    s->init_called++;
    return NX_OK;
}

static int posix_shim_enable(void *self)
{
    struct posix_shim_state *s = self;
    s->enable_called++;
    /* Per-task slot wiring is done at task_create-time (slice 8.0a.5),
     * not here.  posix_shim's enable just marks the boundary as live. */
    return NX_OK;
}

static int posix_shim_disable(void *self)
{
    struct posix_shim_state *s = self;
    s->disable_called++;
    return NX_OK;
}

static void posix_shim_destroy(void *self)
{
    struct posix_shim_state *s = self;
    s->destroy_called++;
    if (g_posix_shim == s) g_posix_shim = NULL;
}

/* Map a per-task `caller_slot *` back to its containing `nx_task *`.
 * The slot is embedded by value in `struct nx_task` (slice 8.0a.5), so
 * the offsetof-relative back-conversion is exact.  We guard the call
 * by checking the slot's iface tag — only slots wired by
 * `wire_caller_slot` carry `iface == "task"`, and that's what makes
 * the container_of safe. */
static struct nx_task *task_from_caller_slot(struct nx_slot *slot)
{
    if (!slot || !slot->iface)              return NULL;
    if (strcmp(slot->iface, "task") != 0)   return NULL;
    return (struct nx_task *)((char *)slot -
        offsetof(struct nx_task, caller_slot));
}

static int posix_shim_handle_msg(void *self, struct nx_ipc_message *msg)
{
    struct posix_shim_state *s = self;
    s->messages_handled++;

    if (!msg) return NX_EINVAL;

    /* Slice 8.0a.6: only reply messages reach this handler.  Per
     * SLOT-CALL-API.md §"Reply Path (Option β)", a request that
     * accidentally arrives at a per-task caller_slot is a contract
     * violation (tasks are senders, not receivers, except for replies)
     * — refuse it with EINVAL.  The dispatcher will not synthesize
     * another reply because reply messages don't carry
     * NX_MSG_FLAG_REPLY_REQUESTED. */
    if (!(msg->flags & NX_MSG_FLAG_REPLY)) return NX_EINVAL;

    struct nx_task *task = task_from_caller_slot(msg->dst_slot);
    if (!task) return NX_EINVAL;
    if (!task->caller_slot_active || !task->in_flight_reply_buf) {
        s->reply_unbound_caller++;
        return NX_EINVAL;
    }

    /* The reply payload begins with `struct nx_reply_header { rc }`;
     * trailing bytes (slice 8.0b's per-op output fields) are also
     * memcpy'd into the caller's wrapper-allocated reply_buf so the
     * wrapper can read them after `nx_slot_call_blocking` returns. */
    bool truncated   = msg->payload_len < sizeof(struct nx_reply_header);
    bool oversized   = msg->payload_len > task->in_flight_reply_buf_len;
    bool null_payload = msg->payload == NULL;

    if (truncated || oversized || null_payload) {
        /* Set rc to NX_EINVAL so the caller's blocking-call returns
         * a meaningful error rather than zero from the calloc'd
         * reply_buf — and still wake the caller so it doesn't hang
         * on its reply_waitq.  Count truncations for tests. */
        task->in_flight_reply_rc = NX_EINVAL;
        s->reply_truncations++;
    } else {
        const struct nx_reply_header *hdr =
            (const struct nx_reply_header *)msg->payload;
        memcpy(task->in_flight_reply_buf, msg->payload, msg->payload_len);
        task->in_flight_reply_rc = (int)hdr->rc;
        s->replies_routed++;
    }

    nx_waitq_wake_one(&task->reply_waitq);
    return NX_OK;
}

static int posix_shim_on_dep_swapped(void *self,
                                     struct nx_slot      *dep_slot,
                                     struct nx_component *old_comp,
                                     struct nx_component *new_comp,
                                     uint32_t             flags)
{
    (void)self; (void)old_comp; (void)new_comp;
    if (flags & NX_SWAP_STATE_LOST)
        nx_handle_table_invalidate_for_slot(dep_slot);
    return NX_OK;
}

static const struct nx_component_ops posix_shim_component_ops = {
    .init            = posix_shim_init,
    .enable          = posix_shim_enable,
    .disable         = posix_shim_disable,
    .destroy         = posix_shim_destroy,
    .handle_msg      = posix_shim_handle_msg,
    .on_dep_swapped  = posix_shim_on_dep_swapped,
    /* No pause_hook: spawns_threads is false (manifest default). */
};

NX_COMPONENT_REGISTER(posix_shim,
                      struct posix_shim_state,
                      deps,
                      &posix_shim_component_ops,
                      POSIX_SHIM_DEPS_TABLE);
