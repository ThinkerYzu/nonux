#ifndef NONUX_LIST_H
#define NONUX_LIST_H

#include <stddef.h>

/*
 * Intrusive doubly-linked list.  Members embed `struct nx_list_node` in their
 * own struct; a `struct nx_list_head` is a sentinel node whose `next` / `prev`
 * point at the first / last member respectively.  Empty list: head points at
 * itself.
 *
 * Used by core/sched/ for the scheduler's runqueue and any future per-cpu /
 * per-component queues.  No allocations, no ownership — the embedder owns the
 * storage of every node.
 */

struct nx_list_node {
    struct nx_list_node *next;
    struct nx_list_node *prev;
};

struct nx_list_head {
    struct nx_list_node n;
};

static inline void nx_list_init(struct nx_list_head *h)
{
    h->n.next = &h->n;
    h->n.prev = &h->n;
}

static inline int nx_list_empty(const struct nx_list_head *h)
{
    return h->n.next == &h->n;
}

/* Insert `node` after `pos`. */
static inline void nx_list_add_after(struct nx_list_node *pos,
                                     struct nx_list_node *node)
{
    node->prev = pos;
    node->next = pos->next;
    pos->next->prev = node;
    pos->next = node;
}

/* Append at tail (just before the sentinel). */
static inline void nx_list_add_tail(struct nx_list_head *h,
                                    struct nx_list_node *node)
{
    nx_list_add_after(h->n.prev, node);
}

/* Remove `node` from whichever list it's on.  Leaves `node`'s pointers
 * dangling — caller should re-init if the node may be inspected later. */
static inline void nx_list_remove(struct nx_list_node *node)
{
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->next = node;
    node->prev = node;
}

/* Pop the first node, or NULL if empty. */
static inline struct nx_list_node *nx_list_pop_front(struct nx_list_head *h)
{
    if (nx_list_empty(h)) return NULL;
    struct nx_list_node *node = h->n.next;
    nx_list_remove(node);
    return node;
}

/* Iterate every node in the list; `var` is a `struct nx_list_node *`. */
#define nx_list_for_each(var, head) \
    for ((var) = (head)->n.next; (var) != &(head)->n; (var) = (var)->next)

/* Like nx_list_for_each but safe against removing `var` during the body. */
#define nx_list_for_each_safe(var, tmp, head)                          \
    for ((var) = (head)->n.next, (tmp) = (var)->next;                  \
         (var) != &(head)->n;                                          \
         (var) = (tmp), (tmp) = (var)->next)

#define nx_list_entry(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#endif /* NONUX_LIST_H */
