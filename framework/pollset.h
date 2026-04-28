#ifndef NONUX_FRAMEWORK_POLLSET_H
#define NONUX_FRAMEWORK_POLLSET_H

#include "core/lib/list.h"
#include "core/sched/waitq.h"

/*
 * Pollset listener — slice 7.8b.
 *
 * Bridges the per-handle-type readiness signals (channel write +
 * close; console RX byte arrival; future timer expiry) into a single
 * waitq that `sys_ppoll`'s caller blocks on.
 *
 * Lifecycle.
 *   sys_ppoll allocates one `nx_pollset_listener` on its kstack per
 *   pollfd, registers it with the matching object (via
 *   nx_channel_endpoint_register_pollset / nx_console_register_pollset),
 *   then blocks on `pollset->waitq`.  Producers (nx_channel_send,
 *   nx_channel_endpoint_close, console RX ISR) walk the per-object
 *   listener list and call `nx_pollset_listener_wake` on each, which
 *   in turn calls nx_waitq_wake_all on the parent waitq.
 *   sys_ppoll unregisters every listener before returning so the
 *   per-object lists are always empty between calls.
 *
 * Storage is owned by the caller (the pollset on sys_ppoll's
 * kstack).  Per-object lists hold borrowed pointers — never freed.
 */
struct nx_pollset_listener {
    struct nx_list_node  node;     /* links into per-object list */
    struct nx_waitq     *waitq;    /* parent pollset waitq to wake */
};

static inline void nx_pollset_listener_init(struct nx_pollset_listener *l,
                                            struct nx_waitq *waitq)
{
    l->node.next = &l->node;
    l->node.prev = &l->node;
    l->waitq     = waitq;
}

/* Walk a per-object listener list and wake every parent waitq.
 * Producers call this from inside their wake-side critical sections
 * (e.g., right after pushing to a ring or after closing an endpoint). */
static inline void nx_pollset_wake_all(struct nx_list_head *list)
{
    if (!list) return;
    struct nx_list_node *n, *tmp;
    nx_list_for_each_safe(n, tmp, list) {
        struct nx_pollset_listener *l =
            nx_list_entry(n, struct nx_pollset_listener, node);
        if (l->waitq) nx_waitq_wake_all(l->waitq);
    }
}

#endif /* NONUX_FRAMEWORK_POLLSET_H */
