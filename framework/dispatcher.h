#ifndef NX_FRAMEWORK_DISPATCHER_H
#define NX_FRAMEWORK_DISPATCHER_H

#include "framework/ipc.h"

/*
 * Framework dispatcher — slice 3.9b.1.
 *
 * One framework-owned kthread (on the kernel build) consumes the async
 * MPSC inbox and runs each message's IPC_RECV hook + component
 * handler.  See DESIGN.md §Execution Model — Per-CPU Dispatcher Loop.
 *
 * On the host build, `nx_dispatcher_init()` is a no-op; host tests
 * continue to call `nx_ipc_dispatch(slot, max)` as their manual pump.
 *
 * Wiring:
 *   `nx_framework_bootstrap()` calls `nx_dispatcher_init()` AFTER
 *   `sched_init()` so the kthread-spawn path finds the scheduler ready.
 *
 * Queue shape:
 *   One MPSC queue shared by every async producer — kthreads, ISRs,
 *   the dispatcher feeding itself.  Messages are caller-owned; the
 *   dispatcher consumes via `nx_ipc_message.disp_node` (see ipc.h).
 *
 * Concurrency contract:
 *   - `nx_dispatcher_enqueue` is the only producer entry point and is
 *     safe from any context (including ISRs).  It is not allowed to
 *     dereference `slot->active`.
 *   - `nx_dispatcher_pop` runs on the dispatcher kthread only.
 *   - `nx_dispatcher_dequeue_one_for_test` is host-only, drains one
 *     message synchronously so tests can exercise the queue without
 *     spawning a thread.
 */

/*
 * Bring the dispatcher up.  Kernel: spawns `nx_dispatcher_kthread` via
 * `sched_spawn_kthread`.  Host: initialises the MPSC and returns.
 *
 * Returns NX_OK or an NX_E* code on kthread-spawn failure.  Safe to
 * call multiple times; subsequent calls are no-ops.
 */
int nx_dispatcher_init(void);

/*
 * Push a message onto the dispatcher's MPSC.  Called by
 * `nx_ipc_send` (async path, kernel build) and by
 * `nx_ipc_enqueue_from_irq`.  Never dereferences `msg->dst_slot->
 * active`; R8 safe from ISR context.
 *
 * Returns NX_OK or NX_EINVAL for NULL args.
 */
int nx_dispatcher_enqueue(struct nx_ipc_message *msg);

/*
 * Test/debug hooks.  Not part of the normal runtime surface.
 */

/* Run the dispatcher loop body once (pop + dispatch, if any).
 * Returns 1 if a message was dispatched, 0 if the queue was empty.
 * Used by host tests and kernel tests that want to avoid waiting
 * for the kthread to cycle. */
int nx_dispatcher_pump_once(void);

/* Reset the dispatcher MPSC to empty.  Test-only.  On the kernel
 * build this does not tear down the kthread; it just drains
 * outstanding messages. */
void nx_dispatcher_reset(void);

#endif /* NX_FRAMEWORK_DISPATCHER_H */
