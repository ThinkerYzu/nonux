/*
 * Framework dispatcher — slice 3.9b.1, extended for slice 8.0a.6.
 *
 * One MPSC queue, one kthread (on the kernel build) that drains it.
 * Matches DESIGN.md §Execution Model — Per-CPU Dispatcher Loop for
 * the v1 single-CPU case.  SMP will multiply the dispatcher kthread
 * and give each CPU its own queue; the producer API stays the same.
 *
 * Slice 8.0a.6 layered the blocking-call reply path on top:
 *
 *   - `slot->in_flight_calls` is incremented around every handler
 *     invocation so the pause protocol's drain step can wait for the
 *     counter to reach zero (DESIGN.md §"Slot-side pause + blocking-
 *     call metadata").
 *   - When a request carries `NX_MSG_FLAG_REPLY_REQUESTED`, the
 *     dispatcher allocates a reply message from a per-CPU pool, fills
 *     in `struct nx_reply_header { rc }`, and enqueues it back to its
 *     own MPSC.  Next iteration delivers the reply to the caller's
 *     `caller_slot` (whose `active` is the singleton posix_shim
 *     component); `posix_shim_handle_msg` copies the payload into the
 *     caller task's `in_flight_reply_buf` and wakes its `reply_waitq`.
 *   - ABORT on the IPC_RECV hook synthesizes the same EABORT reply so
 *     the caller wakes cleanly (DESIGN.md §"ABORT on a blocking-call
 *     edge synthesizes a reply").
 *   - Reply messages are pool-owned; the dispatcher frees the entry
 *     after the reply's `handle_msg` returns.
 */

#include "framework/dispatcher.h"
#include "framework/component.h"
#include "framework/hook.h"
#include "framework/ipc.h"
#include "framework/registry.h"
#include "framework/slot_call.h"
#include "core/lib/mpsc.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if __STDC_HOSTED__
#include <stdlib.h>
#include <string.h>
#else
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

/* ---------- Reply-message pool (slice 8.0a.6) ------------------------ *
 *
 * `struct nx_reply_pool_entry` lays out a complete reply: an
 * `nx_ipc_message` header followed by NX_REPLY_PAYLOAD_MAX bytes of
 * payload backing.  The dispatcher allocates one entry per blocking
 * round-trip; the entry is freed after the reply's `handle_msg` runs.
 *
 * Allocation is a simple atomic-bitmap claim — single-CPU v1 has just
 * one producer (the dispatcher kthread / pump_once caller) so the
 * loop's CAS contention is zero in practice; SMP will replace this
 * with per-CPU pools.  Pool exhaustion fires a kpanic / abort: a
 * dropped reply would silently hang a blocked caller, which is worse
 * than failing loudly.
 */

struct nx_reply_pool_entry {
    struct nx_ipc_message msg;     /* MUST be first — node_to_msg-style
                                    * back-conversion via direct cast. */
    char                  payload[NX_REPLY_PAYLOAD_MAX];
};

static struct nx_reply_pool_entry g_reply_pool[NX_REPLY_POOL_SIZE];

#define NX_REPLY_POOL_BITMAP_WORDS ((NX_REPLY_POOL_SIZE + 63u) / 64u)
static _Atomic(uint64_t) g_reply_pool_inuse[NX_REPLY_POOL_BITMAP_WORDS];

static void reply_pool_panic_exhausted(void)
{
#if __STDC_HOSTED__
    /* Host: assert-like abort so test runs surface the bug
     * immediately.  abort() pulls in <stdlib.h> which is already
     * included above. */
    abort();
#else
    /* Kernel: minimal kpanic.  We don't have a generic panic helper
     * yet, so log + spin.  This is unreachable in correctly-sized
     * deployments; if it fires, the pool needs to grow or the
     * concurrency model needs revisiting. */
    kprintf("[dispatcher] reply pool exhausted (size %u)\n",
            NX_REPLY_POOL_SIZE);
    for (;;) asm volatile ("wfe");
#endif
}

static struct nx_reply_pool_entry *reply_pool_alloc(void)
{
    for (size_t w = 0; w < NX_REPLY_POOL_BITMAP_WORDS; w++) {
        uint64_t cur = atomic_load_explicit(&g_reply_pool_inuse[w],
                                            memory_order_relaxed);
        while (cur != ~(uint64_t)0) {
            /* Find the lowest zero bit. */
            uint64_t free_bit = ~cur & (cur + 1);
            uint64_t next     = cur | free_bit;
            if (atomic_compare_exchange_weak_explicit(
                    &g_reply_pool_inuse[w], &cur, next,
                    memory_order_acquire, memory_order_relaxed)) {
                /* free_bit is a power-of-two; recover its index. */
                size_t b = 0;
                uint64_t v = free_bit;
                while ((v & 1u) == 0) { v >>= 1; b++; }
                size_t idx = w * 64u + b;
                if (idx >= NX_REPLY_POOL_SIZE) {
                    /* Bitmap word covers tail beyond pool — release and
                     * fall through to next word (or exhaustion). */
                    atomic_fetch_and_explicit(&g_reply_pool_inuse[w],
                                              ~free_bit,
                                              memory_order_release);
                    break;
                }
                return &g_reply_pool[idx];
            }
            /* CAS fail: cur was reloaded with the latest value; retry. */
        }
    }
    reply_pool_panic_exhausted();
    return NULL;   /* unreachable */
}

static bool reply_pool_owns(const struct nx_ipc_message *m)
{
    if (!m) return false;
    uintptr_t addr = (uintptr_t)m;
    uintptr_t lo   = (uintptr_t)&g_reply_pool[0];
    uintptr_t hi   = (uintptr_t)&g_reply_pool[NX_REPLY_POOL_SIZE];
    return addr >= lo && addr < hi;
}

static void reply_pool_free(struct nx_reply_pool_entry *e)
{
    if (!e) return;
    size_t idx = (size_t)(e - g_reply_pool);
    if (idx >= NX_REPLY_POOL_SIZE) return;
    size_t w = idx / 64u;
    size_t b = idx % 64u;
    uint64_t bit = (uint64_t)1u << b;
    atomic_fetch_and_explicit(&g_reply_pool_inuse[w], ~bit,
                              memory_order_release);
}

/* Test-only helpers (declared in `framework/dispatcher.h`'s test
 * surface).  Kept compiled in on every build for the in-kernel
 * `ktest_*` consumers. */
size_t nx_dispatcher_reply_pool_capacity_for_test(void)
{
    return NX_REPLY_POOL_SIZE;
}

size_t nx_dispatcher_reply_pool_in_use_for_test(void)
{
    size_t n = 0;
    for (size_t w = 0; w < NX_REPLY_POOL_BITMAP_WORDS; w++) {
        uint64_t v = atomic_load_explicit(&g_reply_pool_inuse[w],
                                          memory_order_relaxed);
        for (size_t b = 0; b < 64u; b++) {
            if ((w * 64u + b) >= NX_REPLY_POOL_SIZE) break;
            if (v & ((uint64_t)1u << b)) n++;
        }
    }
    return n;
}

void nx_dispatcher_reply_pool_reset_for_test(void)
{
    for (size_t w = 0; w < NX_REPLY_POOL_BITMAP_WORDS; w++)
        atomic_store_explicit(&g_reply_pool_inuse[w], 0u,
                              memory_order_release);
}

bool nx_dispatcher_reply_pool_owns_for_test(const struct nx_ipc_message *m)
{
    return reply_pool_owns(m);
}

/* Build a reply message addressed to the original sender's
 * `caller_slot`.  Slice 8.0b: if the handler's nx_<iface>_dispatch set
 * req->reply_payload_len > 0, the per-op reply struct was written
 * in-place at req->payload; copy it into the pool entry.  Otherwise
 * (legacy handlers that don't use the generated dispatch), fall back to
 * a header-only reply carrying just rc. */
static struct nx_ipc_message *build_reply(struct nx_ipc_message *req, int rc)
{
    struct nx_reply_pool_entry *e = reply_pool_alloc();
    memset(&e->msg, 0, sizeof e->msg);
    memset(e->payload, 0, NX_REPLY_PAYLOAD_MAX);

    uint32_t plen = req->reply_payload_len;
    if (plen > 0) {
        /* Dispatch wrote a per-op reply struct at req->payload. */
        if (plen > NX_REPLY_PAYLOAD_MAX) plen = NX_REPLY_PAYLOAD_MAX;
        memcpy(e->payload, req->payload, plen);
    } else {
        /* Legacy path: header-only reply. */
        struct nx_reply_header *hdr = (struct nx_reply_header *)e->payload;
        hdr->rc = (int32_t)rc;
        plen = (uint32_t)sizeof *hdr;
    }

    e->msg.src_slot    = req->dst_slot;
    e->msg.dst_slot    = req->src_slot;
    e->msg.msg_type    = req->msg_type;
    e->msg.flags       = NX_MSG_FLAG_REPLY;
    e->msg.payload     = e->payload;
    e->msg.payload_len = plen;
    e->msg.n_caps      = 0;
    e->msg.caps        = NULL;
    return &e->msg;
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
#if !__STDC_HOSTED__
    /* Track message lifetime for the pause drain step: in_flight_calls
     * spans from enqueue through handler completion, so a zero count
     * on a DRAINING slot guarantees the MPSC has no pending messages
     * and no handler is running.  Slice 8.1 makes this the canonical
     * drain signal; the host build keeps the old pump-side semantics. */
    atomic_fetch_add_explicit(&msg->dst_slot->in_flight_calls, 1u,
                              memory_order_acq_rel);
#endif
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
    bool reply_requested = (msg->flags & NX_MSG_FLAG_REPLY_REQUESTED) != 0;
    bool is_reply        = (msg->flags & NX_MSG_FLAG_REPLY)           != 0;
    bool pool_owned      = reply_pool_owns(msg);

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
    if (nx_hook_dispatch(&hctx) == NX_HOOK_ABORT) {
        /* DESIGN.md §"ABORT on a blocking-call edge synthesizes a
         * reply": if the aborted message was a request that carries
         * REPLY_REQUESTED, the caller is parked on its reply_waitq
         * and would hang forever without a synthetic reply.  Build
         * an EABORT reply and enqueue it; the next pump iteration
         * delivers it to the caller via posix_shim. */
#if !__STDC_HOSTED__
        /* Balance the enqueue-side increment from nx_dispatcher_enqueue. */
        atomic_fetch_sub_explicit(&dst->in_flight_calls, 1u,
                                  memory_order_acq_rel);
#endif
        if (reply_requested && msg->src_slot) {
            struct nx_ipc_message *abort_reply = build_reply(msg, NX_EABORT);
            (void)nx_dispatcher_enqueue(abort_reply);
        }
        if (is_reply && pool_owned)
            reply_pool_free((struct nx_reply_pool_entry *)msg);
        return 1;
    }

    /* In-flight tracking.  On the host build the add/sub pair brackets
     * handler execution (handler-side semantics).  On the kernel build
     * the add moved to nx_dispatcher_enqueue (enqueue-side semantics),
     * so only the sub remains here — it completes the lifetime tracking
     * started at enqueue, satisfying the pause drain step. */
#if __STDC_HOSTED__
    atomic_fetch_add_explicit(&dst->in_flight_calls, 1u,
                              memory_order_acq_rel);
#endif
    int rc = invoke_handler(dst, msg);
    atomic_fetch_sub_explicit(&dst->in_flight_calls, 1u,
                              memory_order_acq_rel);

    /* Post-handler RECV hook ABORT case is not modelled here: the
     * existing code structure runs RECV pre-handler only.  When an
     * authentic post-handler RECV hook surface lands, the same
     * synthesize-reply rule applies (per DESIGN.md). */

    /* Reply leg, if the request asked for one.  Only requests synthesise
     * replies — replies themselves never carry REPLY_REQUESTED, and a
     * synthetic reply has its src/dst inverted relative to the original
     * request so the caller's caller_slot is the destination. */
    if (reply_requested && msg->src_slot) {
        struct nx_ipc_message *reply = build_reply(msg, rc);
        (void)nx_dispatcher_enqueue(reply);
    }

    /* Reply messages live in the pool — once their handler has run
     * (posix_shim_handle_msg writing the caller's reply_buf + waking
     * the waitq), the entry can be returned. */
    if (is_reply && pool_owned)
        reply_pool_free((struct nx_reply_pool_entry *)msg);

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

    /* Slice 8.0a.6: reset the reply pool too — host tests interleave
     * fixtures and a leaked entry from one test would skew the next
     * test's "in_use == 0" pre-condition. */
    nx_dispatcher_reply_pool_reset_for_test();
}
