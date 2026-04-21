#include "core/lib/kheap.h"
#include "core/lib/lib.h"
#include "core/pmm/pmm.h"

#include <stdint.h>

/*
 * Tiny boot-time kheap.  See core/lib/kheap.h for the big picture.
 *
 * Two allocation paths:
 *
 *   - **Slab** (request <= KHEAP_SLAB_MAX): carve 16-aligned bytes out
 *     of a current slab page.  When a page fills, grab another from
 *     the PMM.  Old slab pages stay live; free() is a no-op for small
 *     allocs.
 *   - **Large** (request > KHEAP_SLAB_MAX): pull contiguous pages from
 *     the PMM and record the size in a side list keyed by the
 *     returned pointer.  free() walks the list to find the run size,
 *     then pmm_free_pages.
 *
 * The side list is a small intrusive chain; every `g_large_hdr` node
 * itself goes through the slab path, so we don't recurse.
 */

#define KHEAP_SLAB_MAX 256U

/* ---- Slab state ------------------------------------------------------ */

static uint8_t *g_slab_next;
static uint8_t *g_slab_end;
static size_t   g_bytes_allocated;

static inline size_t align_up(size_t n, size_t a)
{
    return (n + a - 1) & ~(a - 1);
}

/* Slab alloc only — used internally + by the public malloc for small n.
 * Returns NULL if the PMM is out of pages. */
static void *slab_alloc(size_t need)
{
    if (g_slab_next + need > g_slab_end) {
        uint8_t *p = pmm_alloc_page();
        if (!p) return NULL;
        g_slab_next = p;
        g_slab_end  = p + PMM_PAGE_SIZE;
    }
    void *out = g_slab_next;
    g_slab_next += need;
    g_bytes_allocated += need;
    return out;
}

/* ---- Large-alloc side list ------------------------------------------- */

struct large_hdr {
    void             *ptr;      /* pointer returned to the caller */
    size_t            n_pages;
    struct large_hdr *next;
};

static struct large_hdr *g_large_list;

static struct large_hdr *large_find_and_unlink(void *p)
{
    struct large_hdr **pp = &g_large_list;
    while (*pp) {
        if ((*pp)->ptr == p) {
            struct large_hdr *hit = *pp;
            *pp = hit->next;
            return hit;
        }
        pp = &(*pp)->next;
    }
    return NULL;
}

/* ---- Public API ------------------------------------------------------ */

void *malloc(size_t n)
{
    if (n == 0) return NULL;

    /* 16-byte alignment for every allocation — aarch64 expects
     * max_align_t-aligned struct storage. */
    size_t need = align_up(n, 16);

    if (need <= KHEAP_SLAB_MAX) {
        return slab_alloc(need);
    }

    /* Large path. */
    size_t pages = (need + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    void  *region = pmm_alloc_pages(pages);
    if (!region) return NULL;

    /* Record the run so free() can return it.  Header itself is a
     * slab alloc — bounded by one header per live large region. */
    struct large_hdr *hdr = slab_alloc(align_up(sizeof *hdr, 16));
    if (!hdr) {
        pmm_free_pages(region, pages);
        return NULL;
    }
    hdr->ptr     = region;
    hdr->n_pages = pages;
    hdr->next    = g_large_list;
    g_large_list = hdr;
    g_bytes_allocated += pages * PMM_PAGE_SIZE;
    return region;
}

void *calloc(size_t count, size_t size)
{
    if (count != 0 && size > (size_t)-1 / count) return NULL;
    size_t bytes = count * size;
    void  *p     = malloc(bytes);
    if (!p) return NULL;
    memset(p, 0, bytes);
    return p;
}

void free(void *p)
{
    if (!p) return;
    struct large_hdr *hit = large_find_and_unlink(p);
    if (hit) {
        g_bytes_allocated -= hit->n_pages * PMM_PAGE_SIZE;
        pmm_free_pages(hit->ptr, hit->n_pages);
        /* hit itself was slab-allocated; no-op. */
        return;
    }
    /* Small allocation — no-op.  Boot-time composition never
     * free()s enough to matter; slice 3.9b's real allocator
     * replaces this. */
}

size_t kheap_bytes_allocated(void)
{
    return g_bytes_allocated;
}
