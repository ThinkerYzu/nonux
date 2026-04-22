/*
 * mm_buddy — buddy-allocator page pool (slice 5.2).
 *
 * First real component bound to the `memory.page_alloc` slot.  Manages
 * a fixed-size per-instance pool of 16 pages (64 KiB at 4 KiB granule)
 * using the classic power-of-two buddy algorithm:
 *
 *   init()     → allocate pool (malloc on host / kheap on kernel),
 *                seed free_lists so the whole pool is one order-
 *                MM_BUDDY_POOL_ORDER block.
 *   alloc_pages(order)
 *              → find smallest free block of order ≥ target, split
 *                down, return the bottom half.
 *   free_pages(ptr, order)
 *              → push onto free_lists[order]; if the buddy is also
 *                free, unlink it, coalesce, recurse at order+1.
 *   destroy()  → return pool to its backing allocator.
 *
 * Each free block's first bytes hold a `struct free_node` in-place —
 * classic intrusive free list.  This means a free block's storage is
 * temporarily "used" by allocator metadata, but since the caller does
 * not own freed blocks, that's fine.
 *
 * The pool size is small on purpose.  16 pages is enough to exercise
 * splits and merges end-to-end without bloating the per-instance state,
 * and every near-term consumer (page-table pages in slice 5.5, handle
 * tables in 5.3) needs at most a handful of pages.  A future consumer
 * needing more memory can either bind its own mm_buddy instance or
 * drive a larger pool through a component config knob.
 */

#include "framework/component.h"
#include "framework/registry.h"
#include "interfaces/mm.h"
#include "core/pmm/pmm.h"

#include <stddef.h>
#include <stdint.h>

#if __STDC_HOSTED__
#include <stdlib.h>
#else
#include "core/lib/kheap.h"
#include "core/lib/lib.h"
#endif

/* Pool geometry.  MM_BUDDY_POOL_ORDER is the max allocation order and
 * also the pool's order — one block at init spans the entire pool. */
#define MM_BUDDY_POOL_ORDER   4u                      /* 16 pages, 64 KiB */
#define MM_BUDDY_POOL_PAGES   (1u << MM_BUDDY_POOL_ORDER)
#define MM_BUDDY_POOL_BYTES   ((size_t)MM_BUDDY_POOL_PAGES * PMM_PAGE_SIZE)

struct free_node {
    struct free_node *next;
};

struct mm_buddy_state {
    void             *pool_base;
    struct free_node *free_lists[MM_BUDDY_POOL_ORDER + 1];

    /* Counters observable by host/kernel tests. */
    unsigned          init_called;
    unsigned          enable_called;
    unsigned          disable_called;
    unsigned          destroy_called;
};

/* ---------- Pool allocator helper (host vs kernel) -------------------- */

static void *pool_alloc_page_aligned(size_t size)
{
#if __STDC_HOSTED__
    void *p = NULL;
    if (posix_memalign(&p, PMM_PAGE_SIZE, size) != 0) return NULL;
    return p;
#else
    /* kheap's malloc on the kernel side returns page-aligned pointers
     * for whole-page allocations (it delegates straight to PMM). */
    return malloc(size);
#endif
}

static void pool_free(void *p)
{
    free(p);
}

/* ---------- Free-list helpers ----------------------------------------- */

static void free_list_push(struct mm_buddy_state *s, unsigned order,
                           struct free_node *node)
{
    node->next = s->free_lists[order];
    s->free_lists[order] = node;
}

static struct free_node *free_list_pop(struct mm_buddy_state *s, unsigned order)
{
    struct free_node *n = s->free_lists[order];
    if (n) s->free_lists[order] = n->next;
    return n;
}

/* Walk-and-remove.  Pool size is tiny (≤ 16 entries at order 0), so a
 * linear scan is fine — swapping for a doubly-linked list or bitmap
 * only pays off once the pool grows. */
static int free_list_remove(struct mm_buddy_state *s, unsigned order,
                            struct free_node *target)
{
    struct free_node **pp = &s->free_lists[order];
    while (*pp && *pp != target) pp = &(*pp)->next;
    if (!*pp) return 0;
    *pp = target->next;
    return 1;
}

/* ---------- Interface ops (`struct nx_mm_ops`) ------------------------ */

static void *mm_buddy_alloc_pages(void *self, unsigned order)
{
    struct mm_buddy_state *s = self;
    if (order > MM_BUDDY_POOL_ORDER) return NULL;

    /* Find smallest free block with order >= target. */
    unsigned found = order;
    while (found <= MM_BUDDY_POOL_ORDER && !s->free_lists[found])
        found++;
    if (found > MM_BUDDY_POOL_ORDER) return NULL;

    struct free_node *block = free_list_pop(s, found);

    /* Split down to target order.  The upper half of each split goes
     * back on the smaller order's free list; we keep descending on the
     * lower half.  Addresses within the pool use pointer arithmetic
     * in bytes. */
    while (found > order) {
        found--;
        size_t half_size = (size_t)PMM_PAGE_SIZE << found;
        struct free_node *buddy =
            (struct free_node *)((uintptr_t)block + half_size);
        free_list_push(s, found, buddy);
    }

    return block;
}

static void mm_buddy_free_pages(void *self, void *ptr, unsigned order)
{
    if (!ptr) return;
    struct mm_buddy_state *s = self;
    if (order > MM_BUDDY_POOL_ORDER) return;

    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t base = (uintptr_t)s->pool_base;

    /* Coalesce while the buddy is free.  Buddy address is addr XOR
     * block_size, computed relative to the pool base. */
    while (order < MM_BUDDY_POOL_ORDER) {
        size_t block_size = (size_t)PMM_PAGE_SIZE << order;
        uintptr_t buddy_addr = base + (((addr - base) ^ block_size));
        if (!free_list_remove(s, order, (struct free_node *)buddy_addr))
            break;  /* buddy wasn't free — stop coalescing */

        /* Merged block starts at the lower of the two addresses. */
        if (buddy_addr < addr) addr = buddy_addr;
        order++;
    }

    free_list_push(s, order, (struct free_node *)addr);
}

static size_t mm_buddy_page_size(void *self)
{
    (void)self;
    return PMM_PAGE_SIZE;
}

static unsigned mm_buddy_max_order(void *self)
{
    (void)self;
    return MM_BUDDY_POOL_ORDER;
}

const struct nx_mm_ops mm_buddy_mm_ops = {
    .alloc_pages = mm_buddy_alloc_pages,
    .free_pages  = mm_buddy_free_pages,
    .page_size   = mm_buddy_page_size,
    .max_order   = mm_buddy_max_order,
};

/* ---------- Component lifecycle --------------------------------------- */

static int mm_buddy_init(void *self)
{
    struct mm_buddy_state *s = self;

    s->pool_base = pool_alloc_page_aligned(MM_BUDDY_POOL_BYTES);
    if (!s->pool_base) return NX_ENOMEM;

    /* All free_lists empty; seed the top-level one with a single block
     * covering the whole pool. */
    for (unsigned k = 0; k <= MM_BUDDY_POOL_ORDER; k++)
        s->free_lists[k] = NULL;
    free_list_push(s, MM_BUDDY_POOL_ORDER,
                   (struct free_node *)s->pool_base);

    s->init_called++;
    return NX_OK;
}

static int mm_buddy_enable(void *self)
{
    struct mm_buddy_state *s = self;
    s->enable_called++;
    return NX_OK;
}

static int mm_buddy_disable(void *self)
{
    struct mm_buddy_state *s = self;
    s->disable_called++;
    /* Pool stays carved across enable/disable cycles — held allocations
     * remain valid.  Full teardown happens in destroy. */
    return NX_OK;
}

static void mm_buddy_destroy(void *self)
{
    struct mm_buddy_state *s = self;
    s->destroy_called++;
    if (s->pool_base) {
        pool_free(s->pool_base);
        s->pool_base = NULL;
    }
    for (unsigned k = 0; k <= MM_BUDDY_POOL_ORDER; k++)
        s->free_lists[k] = NULL;
}

const struct nx_component_ops mm_buddy_component_ops = {
    .init    = mm_buddy_init,
    .enable  = mm_buddy_enable,
    .disable = mm_buddy_disable,
    .destroy = mm_buddy_destroy,
    /* No pause_hook: spawns_threads is false in the manifest.
     * No handle_msg: mm_buddy is called directly via iface_ops. */
};

NX_COMPONENT_REGISTER_NO_DEPS_IFACE(mm_buddy,
                                    struct mm_buddy_state,
                                    &mm_buddy_component_ops,
                                    &mm_buddy_mm_ops);
