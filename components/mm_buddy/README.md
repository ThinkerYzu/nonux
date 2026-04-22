# mm_buddy

Buddy-allocator page pool — the first real component bound to the
`memory.page_alloc` slot, introduced in slice 5.2. Classic power-of-two
buddy on top of a fixed-size per-instance pool.

## Interface

- **iface:** `memory`
- **Bound by default:** `kernel.json["components"]["memory.page_alloc"]`.
- **Dependencies:** none.
- **Worker threads:** none (`spawns_threads: false`).
- **Pause hook:** not required (`pause_hook: false`).

## Behaviour

- Each instance owns a 16-page pool (`MM_BUDDY_POOL_PAGES = 16`, 64 KiB
  at the 4 KiB granule). The pool is allocated in `init()` and freed in
  `destroy()` — heap usage on host, `kheap` on the kernel. Both paths
  return page-aligned pointers so the buddy's block-alignment invariant
  holds.
- `alloc_pages(order)` finds the smallest free block with order ≥
  target, splits it down, and returns the low half. Address-aligned to
  `(1 << order) * PAGE_SIZE` within the pool.
- `free_pages(ptr, order)` pushes onto `free_lists[order]`; if the
  buddy (same-order neighbour, address XOR block_size) is also free,
  the two coalesce and the merge recurses up to `MM_BUDDY_POOL_ORDER`.
- `page_size()` returns 4096; `max_order()` returns 4 (pool size).

## State

```c
struct mm_buddy_state {
    void             *pool_base;
    struct free_node *free_lists[MM_BUDDY_POOL_ORDER + 1];
    /* + counters observable by kernel/host tests */
};
```

Free blocks carry an in-place `struct free_node` (one `next` pointer)
at their head — intrusive free-list. Nothing else is stored on the
block while it's free, so caller-owned memory is never touched after
`free_pages` returns.

## Symbols exported

- `extern const struct nx_mm_ops mm_buddy_mm_ops;` — the `interfaces/mm.h`
  surface the kernel's MMU / handle / address-space code consumes.
- `extern const struct nx_component_ops mm_buddy_component_ops;` —
  framework lifecycle (`init` / `enable` / `disable` / `destroy`).
  `handle_msg` is NULL: mm_buddy is driven directly through `mm_ops`,
  not via IPC.
- `extern const struct nx_component_descriptor mm_buddy_descriptor;`
  (auto-emitted by `NX_COMPONENT_REGISTER_NO_DEPS_IFACE`).

## Why this component exists

Slice 5.1 turned the kernel MMU on with hard-coded BSS-resident page
tables. Phase 5 needs a real page allocator for per-process TTBR0
tables, handle-table storage, and userspace stacks. Shipping it as a
swappable component follows the same pattern as `sched_rr` — the
kernel's page-allocation strategy becomes a config choice, not a
compile-time decision.

```
interfaces/mm.h  ─────────────┐
(slice 5.2 defines ops contract) │
                                 ▼
conformance_mm.{h,c}  ───────────┐
(slice 5.2 universal cases)       │
                                  ▼
components/mm_buddy/  ─────────── passes every case
(this slice)                      plus lifecycle cycling
                                  │
                                  ▼
                    kernel.json binds "memory.page_alloc" → mm_buddy
                    nx_framework_bootstrap brings it up in NX_LC_ACTIVE
```

Other plausible `memory` implementations (slab, rbtree-backed VMA
allocator, NUMA-aware pool) can be swapped in by editing `kernel.json`
— the conformance suite ensures they meet the same contract.
