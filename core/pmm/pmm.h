#ifndef NONUX_PMM_H
#define NONUX_PMM_H

#include <stddef.h>
#include <stdint.h>

/*
 * Physical page allocator.  Bitmap-backed, 4 KiB pages.
 *
 * The PMM manages a single contiguous region passed in at init.  The bitmap
 * lives at the start of the region and is self-accounting: the pages that
 * hold the bitmap are marked allocated so subsequent allocations never hand
 * them out.
 *
 * Concurrency: bit flips use __atomic_fetch_{or,and}, so a future preemptive
 * interrupt handler or second CPU can alloc/free without additional locking.
 * (See the "Preemption requires atomics" rule in project memory.)
 */

#define PMM_PAGE_SIZE  4096U
#define PMM_PAGE_SHIFT 12

/* Initialize the PMM over the region [base, base + size).  The region is
 * rounded inward to page boundaries.  Must be called exactly once before any
 * alloc/free. */
void pmm_init(uintptr_t base, size_t size);

/* Allocate one page.  Returns a page-aligned kernel-virtual pointer, or NULL
 * if the pool is empty. */
void *pmm_alloc_page(void);

/* Allocate `count` physically contiguous pages.  Returns the pointer to the
 * first page, or NULL.  `count` must be >= 1. */
void *pmm_alloc_pages(size_t count);

/* Release one page previously returned by pmm_alloc_page (or the first page
 * of a single-page pmm_alloc_pages call). */
void pmm_free_page(void *page);

/* Release `count` contiguous pages previously returned by pmm_alloc_pages. */
void pmm_free_pages(void *page, size_t count);

/* Pool statistics.  Safe to call at any time after pmm_init. */
size_t pmm_free_count(void);
size_t pmm_total_count(void);

#endif /* NONUX_PMM_H */
