/*
 * Framework dispatcher — slice 3.9b.1.
 *
 * One MPSC queue, one kthread (on the kernel build) that drains it.
 * Matches DESIGN.md §Execution Model — Per-CPU Dispatcher Loop for
 * the v1 single-CPU case.  SMP will multiply the dispatcher kthread
 * and give each CPU its own queue; the producer API stays the same.
 */

#include "framework/dispatcher.h"
#include "framework/component.h"
#include "framework/hook.h"
#include "framework/ipc.h"
#include "framework/registry.h"
#include "core/lib/mpsc.h"

#include <stddef.h>

#if !__STDC_HOSTED__
#include "core/sched/sched.h"
#include "core/lib/lib.h"
#endif

/* Single global MPSC queue.  v1 is single-CPU so one queue suffices;
 * when SMP arrives each CPU gets its own and the producer enqueues
 * to its own local queue (remote-CPU sends go through a cross-CPU
 * send path that SMP will introduce). */
static struct nx_mpsc_queue g_disp_mpsc;
static bool                 g_disp_mpsc_initialized;
#if !__STDC_HOSTED__
static bool                 g_disp_kthread_spawned;
#endif

static inline struct nx_ipc_message *node_to_msg(struct nx_mpsc_node *n)
{
    return (struct nx_ipc_message *)
        ((char *)n - offsetof(struct nx_ipc_message, disp_node));
}

/* ---------- Queue access --------------------------------------------- */

static void ensure_queue_init(void)
{
    if (g_disp_mpsc_initialized) return;
    nx_mpsc_init(&g_disp_mpsc);
    g_disp_mpsc_initialized = true;
}

int nx_dispatcher_enqueue(struct nx_ipc_message *msg)
{
    if (!msg || !msg->dst_slot) return NX_EINVAL;
    ensure_queue_init();
    nx_mpsc_push(&g_disp_mpsc, &msg->disp_node);
    return NX_OK;
}

/* ---------- Local helper: invoke the handler for one msg ------------- */

static int invoke_handler(struct nx_slot *dst, struct nx_ipc_message *msg)
{
    struct nx_component *active = dst->active;
    if (!active || !active->descriptor || !active->descriptor->ops ||
        !active->descriptor->ops->handle_msg)
        return NX_ENOENT;
    int rc = active->descriptor->ops->handle_msg(active->impl, msg);
    /* Post-handler cap sweep — same contract as nx_ipc_dispatch on
     * host: borrowed caps are silently dropped, unclaimed TRANSFERs
     * are observable via the return value of nx_ipc_scan_recv_caps
     * (call it here so caller can see them if they care). */
    nx_ipc_scan_recv_caps(dst, msg);
    return rc;
}

/* Pop + dispatch one message.  Runs the IPC_RECV hook first; an
 * ABORT return drops the message and continues (same ledger as the
 * host nx_ipc_dispatch drain).  Returns 1 if a message was handled,
 * 0 if the queue was empty (or transiently inconsistent per Vyukov
 * semantics — caller should retry on next tick). */
int nx_dispatcher_pump_once(void)
{
    if (!g_disp_mpsc_initialized) return 0;

    struct nx_mpsc_node *node = nx_mpsc_pop(&g_disp_mpsc);
    if (!node) return 0;

    struct nx_ipc_message *msg = node_to_msg(node);
    struct nx_slot *dst = msg->dst_slot;

    /* Pre-handler hook.  edge lookup is optional for the dispatcher
     * path since async messages already ran through nx_ipc_send's
     * pre-route cap scan + pause-policy check; passing NULL for the
     * edge is consistent with the host drain when a message was
     * enqueued before its edge was unregistered. */
    struct nx_hook_context hctx = {
        .point = NX_HOOK_IPC_RECV,
        .u.ipc = { .src = msg->src_slot, .dst = dst,
                   .msg = msg, .edge = NULL },
    };
    if (nx_hook_dispatch(&hctx) == NX_HOOK_ABORT)
        return 1;

    (void)invoke_handler(dst, msg);
    return 1;
}

/* ---------- Kthread body (kernel only) ------------------------------- */

#if !__STDC_HOSTED__
static void nx_dispatcher_kthread_entry(void *arg)
{
    (void)arg;
    for (;;) {
        while (nx_dispatcher_pump_once())
            ;
        /* Queue empty — yield so other kthreads can produce work.
         * When they yield back to us, we drain again.  Slice 3.9b
         * can add a proper wait-queue later (the spec allows the
         * dispatcher to block on an `msg_queue_dequeue_wait`). */
        nx_task_yield();
    }
}
#endif

int nx_dispatcher_init(void)
{
    ensure_queue_init();

#if __STDC_HOSTED__
    /* Host: the queue is the whole dispatcher.  Tests pump via
     * nx_dispatcher_pump_once explicitly. */
    return NX_OK;
#else
    if (g_disp_kthread_spawned) return NX_OK;
    struct nx_task *t = sched_spawn_kthread("nx_disp",
                                            nx_dispatcher_kthread_entry,
                                            NULL, NULL);
    if (!t) return NX_ENOMEM;
    g_disp_kthread_spawned = true;
    return NX_OK;
#endif
}

/* ---------- Test helpers --------------------------------------------- */

void nx_dispatcher_reset(void)
{
    /* Drain and discard any pending messages.  We can't free them
     * (they're caller-owned) — we just detach.  On SMP a full reset
     * would need to pause the dispatcher kthread first; v1 doesn't
     * need that. */
    if (!g_disp_mpsc_initialized) return;
    while (nx_mpsc_pop(&g_disp_mpsc))
        ;
    /* Reinitialise so a subsequent set of pushes sees a clean
     * stub-at-head state.  Tests that set up/tear down the graph
     * between runs expect this. */
    nx_mpsc_init(&g_disp_mpsc);
}
