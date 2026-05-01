/*
 * posix_shim — kernel-side syscall-entry boundary component.
 *
 * Slice 8.0a.4 lands the skeleton: manifest, init/enable/disable/destroy
 * stubs, and a placeholder `handle_msg` that returns NX_EINVAL until
 * slice 8.0a.6 fills in the reply-routing body.
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
 * (lands 8.0a.6) routes the reply payload into the originating task's
 * kstack reply buffer and wakes its `reply_waitq`.
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
#include "framework/ipc.h"
#include "framework/registry.h"
#include "gen/posix_shim_deps.h"

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
};

/* Slice 8.0a.5 will populate this at task_create-time when the first
 * caller_slot binds.  For 8.0a.4 the singleton is set in init() so
 * future code (and tests) can `extern` it without a NULL-check
 * landmine. */
struct posix_shim_state *g_posix_shim = NULL;

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

static int posix_shim_handle_msg(void *self, struct nx_ipc_message *msg)
{
    struct posix_shim_state *s = self;
    s->messages_handled++;
    (void)msg;
    /* Slice 8.0a.6 will route reply messages here — decode payload into
     * the originating task's kstack reply buffer and wake its
     * `reply_waitq`.  Until then, refusing the message keeps any stray
     * caller from observing a silent success. */
    return NX_EINVAL;
}

static const struct nx_component_ops posix_shim_component_ops = {
    .init       = posix_shim_init,
    .enable     = posix_shim_enable,
    .disable    = posix_shim_disable,
    .destroy    = posix_shim_destroy,
    .handle_msg = posix_shim_handle_msg,
    /* No pause_hook: spawns_threads is false (manifest default). */
};

NX_COMPONENT_REGISTER(posix_shim,
                      struct posix_shim_state,
                      deps,
                      &posix_shim_component_ops,
                      POSIX_SHIM_DEPS_TABLE);
