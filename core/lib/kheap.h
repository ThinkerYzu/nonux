#ifndef NONUX_KHEAP_H
#define NONUX_KHEAP_H

#include <stddef.h>

/*
 * Minimal kernel heap — provides malloc/calloc/free so freestanding
 * code (e.g. the framework registry and IPC router) can link without
 * pulling in libc.
 *
 * Slice-3.9a design: a slab-on-PMM allocator tuned for boot-time
 * bring-up, not long-lived dynamic workloads.
 *
 *   - Small allocations (<= KHEAP_SLAB_MAX) carve bytes out of a page
 *     carved from the PMM.  Pages are shared across many small allocs
 *     via a bump pointer.
 *   - Larger allocations take a whole page (or contiguous run) from
 *     the PMM directly.
 *   - free() on a small allocation is a no-op — boot-time composition
 *     doesn't churn the heap, so precise tracking isn't worth the
 *     per-alloc header.  free() on a large allocation returns the
 *     page(s) to the PMM.
 *
 * Slice 3.9b will replace this with a real kmalloc backed by buckets
 * once kthreads + dynamic component swap exist.  All the framework's
 * allocation call sites go through malloc/calloc/free, so that swap
 * is a single source file, not a refactor.
 */

void  *malloc(size_t n);
void  *calloc(size_t count, size_t size);
void   free(void *p);

/* Exposed for telemetry / testing. */
size_t kheap_bytes_allocated(void);

#endif /* NONUX_KHEAP_H */
