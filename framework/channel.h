#ifndef NX_FRAMEWORK_CHANNEL_H
#define NX_FRAMEWORK_CHANNEL_H

#include "framework/registry.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Channel framework — Phase 5 slice 5.6.
 *
 * A channel is a pair of endpoints.  A message sent on endpoint A
 * lands in endpoint B's inbox ring (and vice versa).  Each endpoint
 * is typically wrapped in a handle (HANDLE_CHANNEL) so EL0 can hold
 * a reference; syscalls look the handle up in the caller's table,
 * verify the right, and dispatch to `nx_channel_send` / `_recv`.
 *
 * v1 design choices:
 *
 *   - **Fixed-size messages.**  Each message payload is at most
 *     NX_CHANNEL_MSG_MAX bytes.  Larger payloads get NX_EINVAL from
 *     send; callers pack multi-message streams themselves.  Slice 6+
 *     grows this to variable-length with a size-prefixed layout.
 *   - **Bounded ring of 8 messages per direction.**  Send to a full
 *     ring returns NX_EBUSY — non-blocking.  Slice 5.6 relies on
 *     recv-non-blocking too (`NX_EAGAIN` when empty) so no scheduler
 *     wake-queue is needed yet.  A later slice adds blocking + wake
 *     coordination when a real consumer needs it.
 *   - **Per-endpoint handle refcount.**  Each endpoint has its own
 *     `_Atomic int handle_refs` counter that tracks how many handles
 *     in any handle table point at it.  `nx_channel_create` returns
 *     each endpoint with refs=1 (one handle each on the caller side).
 *     `nx_channel_endpoint_retain` bumps the count when fork inherits
 *     a CHANNEL handle into the child's table — the same endpoint is
 *     now reachable from two handle tables, and the second close
 *     mustn't close the endpoint until the second handle is also
 *     released.  `nx_channel_endpoint_close` decrements; when an
 *     endpoint's refs hit zero it's marked closed.  When both
 *     endpoints are closed the whole channel allocation is freed.
 *   - **Single-CPU v1.**  Ring pointer updates are plain loads/stores
 *     with a release/acquire-ordered tail write so a future SMP
 *     upgrade is a per-site barrier audit, not a restructure.
 */

#define NX_CHANNEL_MSG_MAX   256u    /* bytes per message, fixed */
#define NX_CHANNEL_RING_LEN  8u      /* messages per inbox ring */

/*
 * Opaque to callers.  Layout is private to framework/channel.c so a
 * future slice can change the ring shape (SPSC → MPMC, grow the
 * max-message-size, etc.) without touching the public surface.  The
 * syscall bodies and the handle table hold this pointer as the
 * backing object for a HANDLE_CHANNEL.
 */
struct nx_channel_endpoint;

/*
 * Allocate a new channel and write its two endpoints into `*e0` and
 * `*e1`.  On success each endpoint holds exactly one reference to the
 * shared allocation; callers must pair each with a matching
 * `nx_channel_endpoint_close` when done.
 *
 * Returns NX_OK / NX_ENOMEM / NX_EINVAL (NULL args).
 */
int nx_channel_create(struct nx_channel_endpoint **e0,
                      struct nx_channel_endpoint **e1);

/*
 * Send `len` bytes from `data` over `e`.  The bytes land in `e`'s
 * peer's inbox ring.  Returns:
 *
 *   >= 0       — number of bytes queued (always == len on success).
 *   NX_EINVAL  — NULL args / `len > NX_CHANNEL_MSG_MAX`.
 *   NX_EBUSY   — this endpoint is closed, the peer is closed, or the
 *                peer's ring is full (non-blocking: caller retries).
 */
int nx_channel_send(struct nx_channel_endpoint *e,
                    const void *data, size_t len);

/*
 * Dequeue the next message on `e`'s own ring into `buf` (capacity
 * `cap`).  Returns:
 *
 *   >= 0       — bytes actually received.
 *   NX_EINVAL  — NULL args / `cap == 0`.
 *   NX_EAGAIN  — ring is empty (non-blocking).
 *   NX_ENOMEM  — message doesn't fit in `cap` (message stays queued;
 *                caller retries with a bigger buffer).
 *   NX_EBUSY   — endpoint is closed.
 */
int nx_channel_recv(struct nx_channel_endpoint *e,
                    void *buf, size_t cap);

/*
 * Drop one handle reference to this endpoint.  When the endpoint's
 * `handle_refs` falls to zero the endpoint is marked closed; when
 * both endpoints in the channel pair are closed the whole channel
 * allocation is freed.  Idempotent against NULL.  Multiple handle
 * tables (e.g., parent + fork-child) can hold references to the
 * same endpoint — each table's close call drops one ref; only the
 * final close actually shuts the endpoint down.
 */
void nx_channel_endpoint_close(struct nx_channel_endpoint *e);

/*
 * Bump an endpoint's `handle_refs`.  Used by `sys_fork`'s handle-
 * table inheritance path: the child's table allocates a new
 * `HANDLE_CHANNEL` entry pointing at the same endpoint object as
 * the parent's, so the endpoint must learn about the additional
 * reference before either side calls close.  Caller must already
 * be holding a reference (i.e., the parent's handle is live);
 * retaining a closed endpoint is undefined.
 */
void nx_channel_endpoint_retain(struct nx_channel_endpoint *e);

/*
 * Test helpers — inspect the depth of an endpoint's inbox ring
 * without draining it.  Production code has no reason to call these.
 */
size_t nx_channel_endpoint_depth(const struct nx_channel_endpoint *e);
bool   nx_channel_endpoint_is_closed(const struct nx_channel_endpoint *e);
bool   nx_channel_endpoint_peer_closed(const struct nx_channel_endpoint *e);

#endif /* NX_FRAMEWORK_CHANNEL_H */
