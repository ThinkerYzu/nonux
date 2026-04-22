/*
 * Kernel-side coverage for slice 5.2.
 *
 * After `boot_main` runs `nx_framework_bootstrap()`, the
 * `memory.page_alloc` slot declared in kernel.json must be bound to
 * the mm_buddy component in NX_LC_ACTIVE state, with init + enable
 * having fired exactly once.  The bound instance's iface_ops must be
 * callable end-to-end (alloc_pages returns a page-aligned non-NULL
 * pointer, free_pages completes without faulting).  Mirrors the
 * slice-4.3 ktest_sched_bootstrap tests.
 */

#include "ktest.h"
#include "framework/bootstrap.h"
#include "framework/component.h"
#include "framework/registry.h"
#include "interfaces/mm.h"

/* Mirror of mm_buddy.c's private state struct — same technique as
 * ktest_bootstrap.c's uart_pl011_state mirror.  If the component
 * reorders fields, the counter read below produces unexpected values
 * and the test fails loudly.  Must track `struct mm_buddy_state` in
 * components/mm_buddy/mm_buddy.c exactly. */
#define MM_BUDDY_POOL_ORDER 4u
struct mm_buddy_state_mirror {
    void    *pool_base;
    void    *free_lists[MM_BUDDY_POOL_ORDER + 1];
    unsigned init_called;
    unsigned enable_called;
    unsigned disable_called;
    unsigned destroy_called;
};

KTEST(bootstrap_registers_memory_page_alloc_slot)
{
    struct nx_slot *s = nx_slot_lookup("memory.page_alloc");
    KASSERT_NOT_NULL(s);
    KASSERT(strcmp(s->iface, "memory") == 0);
}

KTEST(bootstrap_binds_mm_buddy_to_memory_slot)
{
    struct nx_slot *s = nx_slot_lookup("memory.page_alloc");
    KASSERT_NOT_NULL(s);
    KASSERT_NOT_NULL(s->active);
    KASSERT(strcmp(s->active->manifest_id, "mm_buddy") == 0);
    KASSERT_EQ_U(s->active->state, NX_LC_ACTIVE);
}

KTEST(bootstrap_invoked_mm_buddy_init_and_enable)
{
    struct nx_slot *s = nx_slot_lookup("memory.page_alloc");
    KASSERT_NOT_NULL(s);
    KASSERT_NOT_NULL(s->active);
    KASSERT_NOT_NULL(s->active->impl);
    const struct mm_buddy_state_mirror *m = s->active->impl;
    KASSERT_EQ_U(m->init_called,   1);
    KASSERT_EQ_U(m->enable_called, 1);
    KASSERT_NOT_NULL(m->pool_base);
}

KTEST(mm_buddy_alloc_free_roundtrip_on_bound_instance)
{
    /* Exercise the iface_ops end-to-end against the live bootstrap
     * instance.  Allocate a single page, verify page-alignment, write
     * a pattern, read it back, free. */
    struct nx_slot *s = nx_slot_lookup("memory.page_alloc");
    KASSERT_NOT_NULL(s);
    KASSERT_NOT_NULL(s->active);
    KASSERT_NOT_NULL(s->active->impl);
    KASSERT_NOT_NULL(s->active->descriptor);

    const struct nx_mm_ops *ops = s->active->descriptor->iface_ops;
    KASSERT_NOT_NULL(ops);

    void *p = ops->alloc_pages(s->active->impl, 0);
    KASSERT_NOT_NULL(p);
    size_t ps = ops->page_size(s->active->impl);
    KASSERT_EQ_U(ps, 4096);
    KASSERT_EQ_U((uint64_t)p & (ps - 1), 0);

    /* Scribble + read-back to prove the page is actually backed memory
     * the caller can use.  With the MMU on and RAM mapped Normal, this
     * is an ordinary cacheable store/load. */
    volatile unsigned char *bytes = p;
    bytes[0]      = 0xA5;
    bytes[ps - 1] = 0x5A;
    KASSERT_EQ_U(bytes[0],      0xA5);
    KASSERT_EQ_U(bytes[ps - 1], 0x5A);

    ops->free_pages(s->active->impl, p, 0);
}
