#ifndef NONUX_INTERFACE_MM_H
#define NONUX_INTERFACE_MM_H

#include <stddef.h>

/*
 * Memory page allocator interface — slice 5.2.
 *
 * Every `memory.page_alloc` component implements a
 * `const struct nx_mm_ops` table and exposes a `void *self` instance
 * that carries its private state.  The kernel's MMU subsystem, handle
 * framework, and per-process address-space code consume this interface
 * through the component bound to the `memory.page_alloc` slot.
 *
 * Allocation granularity is always a power-of-two number of pages.
 * `alloc_pages(self, order)` returns a contiguous run of
 * `1 << order` pages, each `page_size(self)` bytes.  A free must name
 * the SAME order as the matching alloc — the allocator is not required
 * to remember per-block sizes and will typically corrupt its free
 * lists if this is violated.
 *
 * Ownership.
 *   The returned pointer is caller-owned until passed back to
 *   `free_pages`.  The allocator never reads or writes the backing
 *   storage while it is owned by the caller (so caller memory is not
 *   clobbered by other allocations).
 *
 * Concurrency.
 *   v1 is single-CPU.  Implementations do not need internal locks in
 *   slice 5.2.  Future SMP support will wrap the ops in a per-component
 *   spinlock or swap the free-list primitives for lock-free variants;
 *   consumers will not need to change.
 *
 * Error convention.
 *   Out-of-memory + invalid-order on alloc returns NULL.  Free is
 *   declared `void` — callers cannot recover from a bad free, so the
 *   allocator is free to assert / trap / silently corrupt its state
 *   on misuse.  Well-behaved callers pair every alloc with exactly one
 *   matching free (by pointer AND order).
 */

struct nx_mm_ops {
    /*
     * Allocate 2^order contiguous pages.  Returns a pointer aligned to
     * at least `page_size(self)` bytes on success, or NULL if the
     * request cannot be satisfied (pool exhausted, or `order` exceeds
     * `max_order(self)`).
     *
     * The implementation MAY return pointers aligned to higher
     * boundaries (up to the block size), but callers must not rely on
     * alignment beyond `page_size`.  A dedicated `alloc_pages_aligned`
     * op will land when an in-tree consumer needs it.
     */
    void *(*alloc_pages)(void *self, unsigned order);

    /*
     * Release a block previously returned by `alloc_pages`.  `order`
     * MUST match the order passed to the matching `alloc_pages` call;
     * mismatches corrupt the free lists.  `ptr == NULL` is a no-op.
     */
    void  (*free_pages)(void *self, void *ptr, unsigned order);

    /*
     * Page size in bytes.  For the kernel's MMU this is 4096 (matches
     * `PMM_PAGE_SIZE`); an implementation that supports multiple
     * granules chooses one at init and returns that value here.
     */
    size_t (*page_size)(void *self);

    /*
     * Largest supported order (inclusive).  `alloc_pages` with an
     * order > this bound returns NULL.  The value is static for the
     * lifetime of the component instance.
     */
    unsigned (*max_order)(void *self);
};

#endif /* NONUX_INTERFACE_MM_H */
