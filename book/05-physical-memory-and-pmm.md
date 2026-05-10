# Physical memory and the page allocator

So far the kernel's idea of "memory I can use" has been tiny.
The boot code has a fixed-size kernel stack laid out by the
linker. The console has a small statically-sized RX ring. The
scheduler's idle task and any kthreads we've talked about all use
storage compiled into the kernel image. There's been no way to
ask, at runtime, for a fresh chunk of RAM and hand it back when
we're done.

That's about to change. In this chapter we wire up the kernel's
**physical memory manager** — the piece of code that keeps track
of which 4 KiB chunks of RAM are free and which are in use, and
hands them out one (or several) at a time. We'll then put a
component on top of it that knows how to allocate and free
*power-of-two-sized* runs of pages, which is the granularity
every later subsystem in the kernel actually wants.

By the end of the chapter you should be able to look at a kernel
boot log line like:

```
[pmm]  total=245760 free=245247 pages (980988 KiB) [user-window reserved]
```

…and know exactly what the numbers mean, where they come from,
how the kernel calculated them, and what an allocation against
that pool actually does inside the CPU.

The relevant files in this repo:

- [`core/pmm/pmm.h`](../core/pmm/pmm.h),
  [`core/pmm/pmm.c`](../core/pmm/pmm.c) — the physical memory
  manager itself: a bitmap-backed page allocator with atomic bit
  flips. About 200 lines of C in total.
- [`core/boot/boot.c`](../core/boot/boot.c) — the call to
  `pmm_init` in `boot_main`, plus the `__free_mem_start`/`RAM_END`
  bounds it passes in.
- [`core/boot/linker.ld`](../core/boot/linker.ld) — where
  `__free_mem_start` is defined and why it's where the kernel
  image ends.
- [`components/mm_buddy/mm_buddy.c`](../components/mm_buddy/mm_buddy.c),
  [`components/mm_buddy/README.md`](../components/mm_buddy/README.md)
  — the buddy allocator, our first non-trivial component bound to
  the `memory.page_alloc` slot.
- [`interfaces/mm.h`](../interfaces/mm.h) — the contract every
  page-allocator component implements.
- [`core/lib/kheap.h`](../core/lib/kheap.h) — the kernel heap
  (`malloc`/`free`) that lives one layer above the PMM and pulls
  pages from it.

---

## Terms you'll see

- **Physical memory.** The actual DRAM chips on the board (or in
  QEMU's case, a slab of host memory pretending to be DRAM). It
  has *physical addresses* — fixed numbers that name byte
  positions in the chip. On QEMU's `virt` machine, RAM starts at
  physical address `0x40000000`.
- **Virtual memory.** A made-up address space that the CPU
  translates into physical addresses on every access via the
  **MMU** (memory management unit). nonux turns the MMU on early
  in `boot_main` with an *identity map* — virtual address `X`
  translates to physical address `X` for every address in RAM.
  We'll explore the MMU properly in the next chapter; for this
  chapter, "virtual" and "physical" mean the same thing
  numerically.
- **Page.** A fixed-size, page-aligned chunk of physical (or
  virtual) memory. The size is a hardware parameter; on ARMv8
  with its smallest granule it's **4096 bytes** (4 KiB), and that
  is what nonux uses. All physical-memory bookkeeping happens at
  page granularity — the kernel never gives out 1234 bytes, it
  gives out a whole page.
- **Page frame.** A specific 4 KiB chunk of physical memory at a
  specific physical address. "Frame" emphasises the *slot* the
  page lives in, as opposed to the *contents* you put in it.
  When we say "page 7 of the pool" we mean "the frame whose
  index is 7", i.e. the bytes at `g_base + 7 * 4096`.
- **PMM (Physical Memory Manager).** The kernel's lowest-level
  page allocator: it owns one big array of page frames, knows
  which are free and which are taken, and hands them out one or
  several at a time. nonux's PMM is a single source file,
  `core/pmm/pmm.c`.
- **Bitmap.** An array of bits where each bit answers a yes/no
  question about one item. Here, the question is "is page
  frame N allocated?" — bit `N` of the bitmap is `1` if so, `0`
  if free. One bit per page, so a 1 GiB pool needs 32 KiB of
  bitmap.
- **First-fit allocation.** The simplest search strategy: walk
  the pool from a starting position, take the first free entry
  you find. The opposite is *best-fit* (search the whole pool
  for the slot whose size most closely matches the request) and
  *next-fit* (start where the last allocation succeeded).
  nonux's PMM uses first-fit with a search hint.
- **Atomic operation.** A read-modify-write on a memory location
  that the hardware promises *cannot* be interrupted between the
  read and the write — even by another CPU or an IRQ on the same
  CPU. nonux uses GCC's `__atomic_*` builtins for this. Without
  them, two interrupting allocators could both see "this bit is
  free" and both claim it, handing out the same page twice.
- **Order.** A power-of-two exponent describing how many pages
  an allocation request wants. Order 0 = 1 page, order 1 = 2
  pages, order 2 = 4 pages, …, order N = 2ᴺ pages. Used by the
  buddy allocator and inherited by the `memory` interface.
- **Buddy allocator.** A page-allocation algorithm that
  organises free pages into "buddies" of every supported order.
  Allocating splits big blocks into smaller ones; freeing
  re-merges them when the neighbour is also free. This is what
  Linux's main page allocator does; mm_buddy is a small,
  pedagogical version of it.
- **Slot, component.** Concepts from the framework: a *slot* is
  a named hole in the kernel ("the thing that hands out
  pages"), and a *component* is a swappable implementation that
  fills it. We've been mentioning these in passing; chapter 9
  treats them properly. For this chapter the only thing to know
  is that `mm_buddy` is the *component* bound to the
  `memory.page_alloc` *slot* in `kernel.json`.

---

## Two questions, two layers

The kernel needs to answer two related but separate questions
about physical memory:

1. **Bookkeeping.** Of all the page frames on this machine,
   which are free and which are in use? How do I find a free
   one when I need one, and how do I record it as free again
   when I'm done?
2. **Sizing.** When code asks for memory, how much does it
   want? Sometimes one page (a kthread's TCB). Sometimes a
   small contiguous run (a 16 KiB page table). Sometimes a
   bigger run still. The bookkeeping layer doesn't care about
   *why*; it just hands out pages.

nonux splits these into two layers, written as two separate
pieces of code:

- **`core/pmm/`** — the **physical memory manager**. Owns the
  bitmap. Hands out individual pages or contiguous runs. It's
  the piece of the kernel that *literally knows* which frames
  exist. Built into the kernel's core; not a component.
- **`components/mm_buddy/`** — the **buddy allocator
  component**. Sits on top of the PMM. Asks the PMM for a
  fixed-size pool at startup, then carves and re-merges that
  pool to satisfy power-of-two requests from kernel subsystems
  through the `memory.page_alloc` slot.

The split looks like overkill for a small kernel (it kind of
*is*), but it pays off later. The PMM is the simplest possible
allocator that can correctly lay claim to all of RAM. The
component layer is where allocator *strategy* lives — and
strategy is exactly the kind of thing you want swappable. A
slab allocator, a NUMA-aware pool, a region-based allocator for
real-time work — they can all be different components bound to
the same slot, sharing the PMM underneath.

This chapter walks the PMM first, top to bottom; then the buddy
component on top.

---

## Where memory comes from

Before we can manage a pool of free pages, we need to know
which pages exist and which are *not* already spoken for. On a
real machine this is the firmware's job to tell us — the
bootloader hands the kernel a list of memory regions and
their roles. On QEMU's `virt` machine the layout is fixed and
public, and nonux exploits that to keep the boot code small.

Three numbers matter:

- **Where RAM starts.** On QEMU `virt`, physical RAM begins at
  `0x40000000`. Below that is MMIO (memory-mapped device
  registers — the PL011 lives at `0x09000000`, the GIC sits
  around `0x08000000`, and so on). The kernel never allocates
  pages below `0x40000000`; that region is for talking to
  hardware.
- **Where the kernel image sits.** The linker script
  ([`core/boot/linker.ld`](../core/boot/linker.ld)) places
  everything starting at `0x40080000`. The first section is
  `.text` (compiled instructions); after that come `.rodata`,
  `.data`, `.bss`, and the kernel's static stack. After
  *all of that*, the linker emits a symbol called
  `__free_mem_start`. Everything from that address up to the
  end of RAM is fair game for the PMM.
- **Where RAM ends.** On QEMU's `virt` machine with `-m 1G`,
  RAM ends at `0x80000000`. nonux's boot code has this
  hardcoded:

  ```c
  #define RAM_END 0x80000000UL
  ```

  …with a comment marking it as the placeholder it is. (A real
  driver would parse the device tree the bootloader handed us
  and read the size from there. nonux will do that in a later
  phase; for now the constant matches QEMU's default and that's
  good enough.)

So `boot_main` does this, after turning the MMU on:

```c
extern char __free_mem_start[];
#define RAM_END 0x80000000UL
...
uintptr_t pmm_base = (uintptr_t)__free_mem_start;
pmm_init(pmm_base, (size_t)(RAM_END - pmm_base));
```

`pmm_init` is told: "here's the base address, here's the size,
go manage that range." It's free to put its own bookkeeping
data inside the range — the kernel doesn't promise to preserve
the byte at `__free_mem_start` for any other reason.

> **Side note: why an `extern char []` and not a number?**
> `__free_mem_start` is a *symbol* defined by the linker, not a
> variable in any C file. The linker fills it in at link time
> based on where everything else ended up. We declare it as
> `extern char []` (with no size) and take its address —
> because the value of `__free_mem_start[]` itself isn't
> meaningful (there's no real array there), only the address
> is. This is the standard idiom for "give me the linker's
> notion of where some boundary lives."

A 1 GiB QEMU instance gives us roughly 1 GiB minus a couple
of megabytes (kernel image + stack) for the PMM to manage.
That's about 245 760 pages in the boot log shown at the top
of the chapter.

---

## The bitmap

The PMM needs to record, for every page frame in its pool,
whether the frame is free or allocated. The most space-efficient
way to do that is **one bit per page**: an array of bits where
bit N tells you about page frame N.

That's what `core/pmm/pmm.c` does. The bitmap is a `uint8_t`
array (eight bits per byte) sized to cover every page in the
pool. To test or flip the bit for page index `i`, the code
splits `i` into a byte index and a bit index within that byte:

```c
static inline size_t  bit_byte(size_t index) { return index >> 3; }
static inline uint8_t bit_mask(size_t index) { return (uint8_t)(1U << (index & 7U)); }
```

So bit 0 is the LSB of byte 0; bit 7 is the MSB of byte 0;
bit 8 is the LSB of byte 1; and so on. The convention is "0 =
free, 1 = allocated" — set means *somebody owns this page*.

How big is the bitmap? One bit per page, 4 KiB per page, so
the bitmap is `pool_size / 4096 / 8` bytes. For a 1 GiB pool
that's 32 768 bytes — eight pages worth of bookkeeping for a
quarter-million pages of pool. That's 0.003% overhead, which
is one of the reasons to use a bitmap instead of, say, a free
list with a linked-list node per page.

### Where the bitmap lives

The PMM has to put the bitmap *somewhere*. There's a chicken-
and-egg problem here: the PMM is the thing that hands out
pages, but it needs storage for the bitmap before it can hand
out anything. A simpler kernel might reserve some `.bss` for
the bitmap, but its size depends on how much RAM the machine
has, which the kernel doesn't know at compile time.

nonux's solution is to put the bitmap *inside the pool itself*,
at the very beginning, then mark the pages it occupies as
allocated. That's done in `pmm_init`:

```c
g_bitmap = (uint8_t *)aligned;            /* base of the pool */
size_t bitmap_bytes = (g_total + 7) / 8;
size_t bitmap_pages = (bitmap_bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;

memset(g_bitmap, 0, bitmap_bytes);        /* every page free, for now */
for (size_t i = 0; i < bitmap_pages; i++) {
    g_bitmap[bit_byte(i)] |= bit_mask(i); /* …except the bitmap's own */
}
```

After that loop, the first `bitmap_pages` pages are marked
allocated. They aren't really "in use" by anyone in the usual
sense — nobody called `alloc` for them — but the *effect* is
the same: subsequent allocations will skip past them, so the
bitmap's storage is safe.

The variable `g_reserved = bitmap_pages` records how many
front-of-pool pages are off-limits; the alloc routines never
look at indices below it. Same with `g_hint`, the search hint
we'll see next — it starts at `g_reserved` so the first
allocation begins right after the bitmap.

> **Side note: this is sometimes called "self-accounting".**
> The data structure that tracks "which pages are taken" is
> itself stored in pages, and those pages are the first to be
> marked taken. It's the same trick as a filesystem putting
> its own metadata blocks at the start of the partition: the
> moment it's mounted, those blocks are already accounted for
> in the metadata they contain.

### Two helpers for testing and claiming bits

Reading a bit is one line:

```c
static int bit_test(size_t index)
{
    return (__atomic_load_n(&g_bitmap[bit_byte(index)], __ATOMIC_RELAXED)
            & bit_mask(index)) != 0;
}
```

`__atomic_load_n` is GCC's "load this byte atomically" builtin.
The `__ATOMIC_RELAXED` argument means "I don't care about
ordering, just don't tear the load." We use it as a cheap
prefilter — *if the bit looks free, try to claim it; if not,
skip*. Even if a racing allocator on another CPU just claimed
the same bit, `bit_test` returning `0` is fine: the
*subsequent* claim will fail and we'll move on.

Claiming is the trickier half. We need to "atomically set bit
`i` from 0 to 1" and find out whether it was already 1. A
plain `g_bitmap[byte] |= mask` won't do — that's a read-modify-
write that another CPU or interrupting handler can step on
between the read and the write, with both sides ending up
thinking they own the page.

The fix is `__atomic_fetch_or`:

```c
static int try_claim(size_t index)
{
    uint8_t mask = bit_mask(index);
    uint8_t old  = __atomic_fetch_or(&g_bitmap[bit_byte(index)], mask,
                                     __ATOMIC_ACQUIRE);
    return (old & mask) == 0;
}
```

`__atomic_fetch_or(&byte, mask, ACQUIRE)` does, atomically,
"set every bit in `byte` that's set in `mask`, and return the
*old* value of `byte`". If the old value's masked bit was 0,
we just won — the bit transitioned from 0 to 1 because of *us*.
If it was already 1, somebody else got there first; we return
0 and try the next slot.

The `ACQUIRE` ordering pairs with a `RELEASE` on the matching
free path, ensuring loads after a claim can't be reordered to
happen before the claim. (We'll come back to memory ordering
when we write the scheduler properly; for now the takeaway is
"the operation is uninterruptible, and it tells you whether
you raced".)

> **Side note: do we really need atomics for a single-CPU
> kernel?** Yes — and the reason is interrupts, not other
> CPUs. If `pmm_alloc_page` is running on the only CPU we
> have and a tick IRQ fires partway through a non-atomic
> read-modify-write, the IRQ handler runs `pmm_alloc_page`
> too (say, to allocate a page for a kthread the scheduler is
> spawning). Both end up handing out the same page, with no
> SMP needed. Atomics make the bit flip uninterruptible —
> the IRQ either sees "before" or "after", never a half-flip.

Releasing a bit — the inverse of claiming — uses
`__atomic_fetch_and` with the *complement* of the mask, so
just the named bit is cleared:

```c
static void release(size_t index)
{
    uint8_t mask = bit_mask(index);
    __atomic_fetch_and(&g_bitmap[bit_byte(index)], (uint8_t)~mask,
                       __ATOMIC_RELEASE);
}
```

That's the whole low-level bookkeeping. Three primitives —
`bit_test`, `try_claim`, `release` — sit underneath every
public PMM call.

---

## Allocating one page

Now we can read `pmm_alloc_page`:

```c
void *pmm_alloc_page(void)
{
    size_t start = __atomic_load_n(&g_hint, __ATOMIC_RELAXED);
    if (start >= g_total) start = g_reserved;

    for (size_t step = 0; step < g_total; step++) {
        size_t i = start + step;
        if (i >= g_total) i -= g_total;
        if (i < g_reserved) continue;

        if (!bit_test(i) && try_claim(i)) {
            __atomic_store_n(&g_hint, i + 1, __ATOMIC_RELAXED);
            __atomic_fetch_sub(&g_free, 1, __ATOMIC_RELAXED);
            return page_addr(i);
        }
    }
    return NULL;
}
```

It's the simplest first-fit search you could write, plus a
hint:

1. **Start where the last allocation succeeded.** `g_hint` is
   set after every successful claim to "the index right after
   the one we just took." That way back-to-back allocs walk
   forward through the pool instead of restarting from the
   beginning every time. This is *next-fit* in disguise — the
   hint is the kernel's bookmark.
2. **Walk the pool, wrapping at the end.** The `for` loop runs
   at most `g_total` times. The wrap (`i -= g_total`) lets us
   keep going past the high end if we started in the middle.
3. **Skip the reserved range.** The bitmap pages live at
   indices `0..g_reserved-1` and are always off-limits.
4. **Cheap test, then expensive claim.** `bit_test` reads the
   byte once with no atomic intent. If the bit looks free, we
   try the atomic claim. If the bit looks taken, we don't even
   try — the cost of `try_claim` only gets paid for plausibly
   free pages.
5. **Update the hint and the free counter on success.** Both
   are atomic stores with relaxed ordering — they're advisory
   numbers, not invariants other code depends on.
6. **Return the address of the claimed page.** `page_addr`
   computes `g_base + index * 4096`. That's the kernel-virtual
   pointer the caller can read from and write to. (Recall that
   our identity map means the virtual and physical addresses
   are equal.)

### Returning a page

`pmm_free_page` defers to the multi-page free path:

```c
void pmm_free_page(void *page) { pmm_free_pages(page, 1); }

void pmm_free_pages(void *page, size_t count)
{
    size_t idx = index_of(page);
    for (size_t i = 0; i < count; i++)
        release(idx + i);
    __atomic_fetch_add(&g_free, count, __ATOMIC_RELAXED);
    __atomic_store_n(&g_hint, idx, __ATOMIC_RELAXED);
}
```

`index_of` does the inverse of `page_addr`:
`(page - g_base) / 4096`. The free path is short because the
hard part already happened in `release` — atomic clear, no
search, constant time.

The last line is a small optimisation: when something gets
freed, set the hint *back* to the freed index. That way the
next allocation will likely reuse the just-freed page,
keeping the *working set* tight. This matters for D-cache
behaviour — re-allocating a recently-freed page often hits a
warm cacheline; allocating a fresh page never does.

---

## Multi-page contiguous allocation

`pmm_alloc_page` hands out one page. Some callers need a
**contiguous run** — N consecutive page frames whose physical
addresses are next to each other. Page tables are the
canonical example: the ARMv8 architecture wants each level of
the table to be 4 KiB and aligned, which is exactly one page,
but a future SMP enhancement that uses larger granules would
want bigger contiguous runs.

`pmm_alloc_pages(count)` is the multi-page form. It's
significantly trickier than the single-page version because
**the claims are not independent**: if we successfully claim
pages 100, 101, 102 and then fail on page 103, we need to roll
back the first three. Otherwise pages 100–102 are leaked.

```c
void *pmm_alloc_pages(size_t count)
{
    if (count == 0) return NULL;
    if (count == 1) return pmm_alloc_page();
    if (count > g_total - g_reserved) return NULL;

    for (size_t i = g_reserved; i + count <= g_total; ) {
        /* Find a run of `count` apparently-free pages. */
        size_t run = 0;
        while (run < count && !bit_test(i + run)) run++;
        if (run < count) {
            i += run + 1;
            continue;
        }

        /* Try to claim all of them. */
        size_t claimed = 0;
        for (; claimed < count; claimed++) {
            if (!try_claim(i + claimed))
                break;
        }
        if (claimed == count) {
            __atomic_store_n(&g_hint, i + count, __ATOMIC_RELAXED);
            __atomic_fetch_sub(&g_free, count, __ATOMIC_RELAXED);
            return page_addr(i);
        }

        /* Roll back and retry after the failure. */
        for (size_t j = 0; j < claimed; j++)
            release(i + j);
        i += claimed + 1;
    }
    return NULL;
}
```

Two passes per candidate run:

1. **Cheap scan.** `bit_test` until either the loop counter
   reaches `count` (good — every page in the run *looks* free)
   or a bit comes back set (skip past it).
2. **Atomic claim.** Walk the same run with `try_claim`. If
   somebody raced us and snatched a page mid-run, we stop
   claiming, **release every claim we made so far**, and
   resume the search past the fail point.

The rollback step is the important detail. Without it, a
single race would permanently leak pages at every collision.
With it, the worst-case behaviour is "we did some atomic work
and ended up where we started" — wasted CPU but no lost
memory.

Note that there's no separate "try-claim a run" primitive —
runs are claimed page-by-page, with rollback. We could
imagine a fancier datastructure (a tree of free runs, say)
that gave us atomic-run-claim in one operation, but for
nonux's pool sizes the linear scan is fast enough and the
code stays small.

> **Side note: real kernels do a lot more here.** Linux's
> page allocator (mm/page_alloc.c) has *hundreds* of
> page-allocation tweaks: per-CPU caches, NUMA awareness,
> watermarks, OOM scoring, fragmentation avoidance, atomic
> reservations for IRQ context, anti-fragmentation
> migrations. nonux's PMM is what's left when you strip all
> of that down to "the bare minimum that's correct under
> preemption." The point of keeping it small is that you can
> read every line.

---

## Reserving a range

There's one more PMM API: `pmm_reserve_range(pa, bytes)`. It
takes a physical address range and marks every page in it as
allocated, so subsequent allocs never return any page that
overlaps the range.

`boot_main` calls it once, right after `pmm_init`:

```c
pmm_reserve_range((uintptr_t)mmu_user_window_base(),
                  (size_t)mmu_user_window_size());
```

The full reason will make more sense after the next chapter on
the MMU, but the short version is: nonux carves out a slice of
virtual-address space called the **user window**, and overlays
it with each running process's pages. If kernel code happened
to allocate a kstack or a page table whose *physical* address
fell into that window, EL1 access to those bytes through their
identity-mapped virtual address would go to the *wrong process's*
data — because the process's TTBR0 redirects that VA. The fix
is to keep all pages in that PA range unavailable for kernel-
internal use. Hence `pmm_reserve_range`.

Implementation is straightforward:

```c
size_t i_start = (start - g_base) >> PMM_PAGE_SHIFT;
size_t i_end   = (end   - g_base) >> PMM_PAGE_SHIFT;

for (size_t i = i_start; i < i_end; i++) {
    if (try_claim(i)) {
        __atomic_fetch_sub(&g_free, 1, __ATOMIC_RELAXED);
    }
}
```

…but with care: the function rounds the requested range
*outward* to page boundaries (so the entire requested region
is covered), clips it to the pool's bounds (so a reservation
outside the pool is silently a no-op), and uses `try_claim`
rather than blind set so that re-reserving an already-reserved
page is harmless. That last property is what the comment in
the header calls *idempotent*.

---

## What `boot_main` actually prints

We've now covered every line behind the boot log entry that
opens the chapter:

```
[boot] kernel loaded at 0x40080000 (QEMU's -kernel offset)
[boot] BSS:  0x40098000 — 0x401a3000
[boot] kernel end:    0x401a3000
[boot] free memory:   0x401e3000 — 0x80000000
[pmm]  total=245760 free=245247 pages (980988 KiB) [user-window reserved]
```

- `kernel loaded at 0x40080000` — the linker script's `ORIGIN`,
  matching QEMU's `-kernel` load offset.
- `BSS: …` — the bounds the linker calculated; everything
  zeroed before `boot_main` runs.
- `kernel end` — `__kernel_end`, end of the `.bss` and stack
  sections.
- `free memory: __free_mem_start — RAM_END` — what `pmm_init`
  was given.
- `[pmm] total=…` — `pmm_total_count()`, which is
  `g_total - g_reserved` (everything except the bitmap pages).
- `free=…` — `pmm_free_count()`, which decrements with every
  alloc and increments with every free.
- `[user-window reserved]` — `pmm_reserve_range` was called,
  consuming a few hundred frames before any subsystem got to
  ask for one.

If you boot the kernel and the numbers shift around a bit, it's
because the kernel image has grown or shrunk slightly — the
bitmap size is computed from the pool size, which depends on
where `__free_mem_start` lands.

---

## The buddy component

The PMM is enough to hand out pages. So why is there a whole
component sitting on top of it?

Three reasons:

1. **Power-of-two requests are the common case.** Page tables
   want one or four contiguous pages. Buffer caches want
   8 or 16 pages at a time. A single-page-or-N-pages PMM API
   forces every caller to think about contiguity, alignment,
   and rollback. A buddy on top lets callers say "give me
   order-3" and get 8 pages aligned to a 32 KiB boundary, no
   contiguity reasoning required.
2. **Coalescing lowers fragmentation.** If we hand out a 16-
   page block, then the user frees the upper 8, then they
   free the lower 8, the buddy puts the original 16-page
   block back together. The PMM by itself would just see 16
   freed pages and have no notion of "they used to be a
   block." Eventually the buddy's strategy reduces external
   fragmentation, especially for short-lived allocations.
3. **Strategy is the kind of thing you swap.** Today's
   buddy could be tomorrow's slab, or a region-based
   allocator for real-time deadlines, or a NUMA-aware pool.
   Making the strategy a *component* — with a typed
   interface, a slot binding, and a config knob — is in
   keeping with nonux's story.

`mm_buddy` is the in-tree implementation today. It's small —
about 250 lines, including comments — and uses the textbook
buddy algorithm. Reading it is a good way to see how a
component is structured before chapter 9 covers the framework
in depth.

### What the component implements

A component is described by two ops tables: a *lifecycle* table
(`init`/`enable`/`disable`/`destroy`) that the framework calls
to bring it up and tear it down, and an *interface* table that
exposes its real behaviour. For mm_buddy the interface is
`struct nx_mm_ops`:

```c
struct nx_mm_ops {
    void *(*alloc_pages)(void *self, uint32_t order);
    void   (*free_pages)(void *self, void *ptr, uint32_t order);
    size_t (*page_size)(void *self);
    uint32_t (*max_order)(void *self);
};
```

`self` is the per-instance state — every component instance has
its own `void *self` it's invoked with. (`mm_buddy_state` is
the type for ours.)

This is the contract every "page allocator" component has to
honour. It comes from
[`interfaces/idl/mm.json`](../interfaces/idl/mm.json) and the
header is auto-generated by `tools/gen-iface.py`. We'll see how
that pipeline works in chapter 10; for this chapter, treat
`interfaces/mm.h` as a contract carved in stone and read the
buddy's implementation against it.

### Pool geometry

The buddy operates on a fixed-size pool, sized at compile time:

```c
#define MM_BUDDY_POOL_ORDER   4u                 /* 16 pages, 64 KiB */
#define MM_BUDDY_POOL_PAGES   (1u << MM_BUDDY_POOL_ORDER)
#define MM_BUDDY_POOL_BYTES   ((size_t)MM_BUDDY_POOL_PAGES * PMM_PAGE_SIZE)
```

That's 16 pages — 64 KiB — per instance. Tiny, intentionally:
the framework's near-term consumers (kernel-side page tables,
handle tables) need at most a handful of pages, and a small
pool exercises the split/merge logic frequently in tests
without hiding bugs behind enormous unused pools.

The maximum allocation is one block of order 4 (the whole
pool, 16 pages). The minimum is one block of order 0 (a single
page).

### State per instance

```c
struct mm_buddy_state {
    void             *pool_base;
    struct free_node *free_lists[MM_BUDDY_POOL_ORDER + 1];
    /* + counters observed by tests */
};

struct free_node {
    struct free_node *next;
};
```

Two interesting parts:

- **`pool_base`** — address of the 64 KiB chunk the component
  asked for at `init`. On the kernel side this comes from
  `kheap`'s `malloc`, which in turn pulls pages from the PMM
  for any whole-page-sized allocation. (See
  [`core/lib/kheap.h`](../core/lib/kheap.h) for the contract.)
  That's how the buddy ends up "on top of" the PMM in the
  layering we mentioned earlier.
- **`free_lists[]`** — one head pointer per supported order.
  `free_lists[0]` chains together free order-0 blocks (single
  pages); `free_lists[4]` chains together free order-4 blocks
  (the whole pool). At `init` only `free_lists[4]` is non-NULL,
  and it holds exactly one node — `pool_base` itself.

The free-list nodes are **intrusive** — they live *inside* the
free blocks themselves. When a block is free, its first 8 bytes
hold a `struct free_node` pointer; the rest of the block is
unused. As soon as the block is allocated, the caller owns
those 8 bytes too and can write whatever they want there. This
saves us from needing a separate per-page metadata array,
which would defeat the point of keeping a small pool.

### Allocation: split until you fit

Allocating order `k` works like this:

1. Look for a free block of *exactly* order `k`. If there is
   one, pop it and return it.
2. Otherwise look for the *smallest* free block of order >
   `k` — call it order `f`. Pop one.
3. Split it down: while `f > k`, halve it. The upper half goes
   onto `free_lists[f-1]`; the lower half stays in your hand
   and you decrement `f`.
4. When `f == k`, return the lower half.
5. If no block of order >= `k` exists anywhere, return NULL.

In code:

```c
static void *mm_buddy_alloc_pages(void *self, unsigned order)
{
    struct mm_buddy_state *s = self;
    if (order > MM_BUDDY_POOL_ORDER) return NULL;

    unsigned found = order;
    while (found <= MM_BUDDY_POOL_ORDER && !s->free_lists[found])
        found++;
    if (found > MM_BUDDY_POOL_ORDER) return NULL;

    struct free_node *block = free_list_pop(s, found);

    while (found > order) {
        found--;
        size_t half_size = (size_t)PMM_PAGE_SIZE << found;
        struct free_node *buddy =
            (struct free_node *)((uintptr_t)block + half_size);
        free_list_push(s, found, buddy);
    }

    return block;
}
```

A worked example helps. Pool starts as one order-4 block at
address `B`. A caller asks for order 2 (4 pages, 16 KiB):

```
Start:  free_lists[4] = [B]            free_lists[≤3] = []

Pop the order-4 block: block = B, found = 4
Split (found = 4 → 3): half_size = 8 pages = 32 KiB
                       buddy = B + 32 KiB → push to free_lists[3]
Split (found = 3 → 2): half_size = 4 pages = 16 KiB
                       buddy = B + 16 KiB → push to free_lists[2]
Stop (found == order). Return block = B.

End:    free_lists[4] = []
        free_lists[3] = [B + 32 KiB]
        free_lists[2] = [B + 16 KiB]
        free_lists[≤1] = []
```

The caller gets pages `B`..`B + 16 KiB`. Two leftovers sit in
the free lists, ready to satisfy future allocations.

### Free: merge while the buddy is free

Freeing order `k` at address `addr`:

1. Compute `addr`'s **buddy address** for order `k`. That's the
   address of the block that this block is paired with at the
   current order.
2. If that buddy is currently free at order `k`, remove it
   from the free list, merge the two blocks (the merged block
   starts at the lower address), and recurse at order `k+1`.
3. If the buddy is not free, push our block onto
   `free_lists[k]` and stop.

The buddy address falls out of one of those neat XOR tricks:
within the pool, blocks of order `k` are aligned to
`block_size = 4096 << k` bytes. Adjacent blocks of the same
order live next to each other, so flipping the bit at position
`block_size` in `addr - pool_base` toggles between the two:

```c
uintptr_t buddy_addr = base + (((addr - base) ^ block_size));
```

Worked example. The pool is again one order-4 block at `B`.
Three allocations and three frees:

```
1) alloc order 0 → returns B, leaves
   free_lists = [B+4K] [B+8K] [B+16K] [B+32K] []

2) alloc order 0 → returns B+4K, leaves
   free_lists = [] [B+8K] [B+16K] [B+32K] []

3) free B+4K, order 0
   buddy of (B+4K) at order 0 is (B+4K) ^ 4K = B
   B is allocated, not free → push B+4K onto free_lists[0]
   free_lists = [B+4K] [B+8K] [B+16K] [B+32K] []

4) free B, order 0
   buddy of B at order 0 is B ^ 4K = B+4K
   B+4K is free → remove it, merge into (B, order 1)
   buddy of B at order 1 is B ^ 8K = B+8K
   B+8K is free → remove it, merge into (B, order 2)
   buddy of B at order 2 is B ^ 16K = B+16K
   B+16K is free → remove it, merge into (B, order 3)
   buddy of B at order 3 is B ^ 32K = B+32K
   B+32K is free → remove it, merge into (B, order 4)
   order 4 is the maximum → push onto free_lists[4]
   free_lists = [] [] [] [] [B]
```

Three allocs and three frees that exactly cancel out: the pool
is back to one order-4 block, exactly the way it started. Pure
by construction.

Reading the implementation:

```c
static void mm_buddy_free_pages(void *self, void *ptr, unsigned order)
{
    if (!ptr) return;
    struct mm_buddy_state *s = self;
    if (order > MM_BUDDY_POOL_ORDER) return;

    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t base = (uintptr_t)s->pool_base;

    while (order < MM_BUDDY_POOL_ORDER) {
        size_t block_size = (size_t)PMM_PAGE_SIZE << order;
        uintptr_t buddy_addr = base + (((addr - base) ^ block_size));
        if (!free_list_remove(s, order, (struct free_node *)buddy_addr))
            break;

        if (buddy_addr < addr) addr = buddy_addr;
        order++;
    }

    free_list_push(s, order, (struct free_node *)addr);
}
```

…which is the algorithm above, line by line.

### Why an XOR works

Within a buddy pool, a block of order `k` always starts at an
offset divisible by `block_size = 4096 << k`. Any two blocks
that *can* coalesce into a single order-`(k+1)` block are
adjacent in memory — one starts at offset `2 * block_size * j`,
the other at offset `2 * block_size * j + block_size`. Their
offsets differ in *exactly* the bit at position `log2(block_size)`,
and are otherwise identical. XORing that bit into the offset
flips you from one to the other.

Restating that: for any free block, the address of the block
it could merge with is just "your address with one specific
bit flipped." No traversal, no metadata, no "where's my
sibling" lookup. It's the property of how blocks are placed
that makes the buddy algorithm so much simpler than it
sounds.

### Lifecycle

Lifecycle code is the part the framework calls during
bring-up and tear-down. For mm_buddy:

```c
static int mm_buddy_init(void *self)
{
    struct mm_buddy_state *s = self;
    s->pool_base = pool_alloc_page_aligned(MM_BUDDY_POOL_BYTES);
    if (!s->pool_base) return NX_ENOMEM;

    for (unsigned k = 0; k <= MM_BUDDY_POOL_ORDER; k++)
        s->free_lists[k] = NULL;
    free_list_push(s, MM_BUDDY_POOL_ORDER,
                   (struct free_node *)s->pool_base);
    return NX_OK;
}
```

- Ask the underlying allocator for 64 KiB. On the kernel side
  that's `malloc` from `kheap`, which whole-page-allocates from
  the PMM (so we end up holding 16 PMM pages). On the host side
  it's `posix_memalign`, used by host tests.
- Initialise the free lists empty.
- Push one order-4 block — the whole pool — onto `free_lists[4]`.

`destroy` is the inverse: free the pool back, NULL the lists.

`enable` and `disable` are no-ops in mm_buddy: the pool stays
carved across enable/disable cycles, since "disable" can be
followed by another "enable" (the framework distinguishes these
from "destroy" precisely so a component's resources can survive
a brief deactivation). In a future allocator with caches or
worker threads, those would be tied off in `disable` and
re-armed in `enable`.

Putting it together, `kernel.json`'s entry is one block:

```json
"memory.page_alloc": {
  "impl": "mm_buddy",
  "config": {}
}
```

…and the framework, which we'll meet properly in chapter 9,
takes care of finding `mm_buddy_descriptor`, allocating its
`struct mm_buddy_state`, calling `init`, calling `enable`, and
plumbing pointers so anyone who reaches for the
`memory.page_alloc` slot ends up calling
`mm_buddy_alloc_pages` underneath.

---

## How the layers stack at runtime

Here's the full picture, from "physical RAM is a slab" up to
"someone called `malloc` for a 50-byte buffer":

```
                    physical RAM (0x40000000 .. 0x80000000)
                                          │
                                          ▼
                   ┌────────────── kernel image .text/.rodata/.data/.bss ┐
                                          │
                                          ▼
                   ┌─────────── core/pmm  (bitmap-backed page allocator) ┐
                   │ pmm_alloc_page  / pmm_alloc_pages  / pmm_free_pages
                   │ pmm_reserve_range,  pmm_free_count / pmm_total_count
                                          │
                          ┌───────────────┼───────────────┐
                          ▼                               ▼
                   core/lib/kheap                  components/mm_buddy
                  (slab/whole-page                 (per-instance buddy
                   malloc / free)                  pool, 64 KiB each)
                          │                               │
                          ▼                               ▼
              kheap callers in framework        memory.page_alloc slot
              and IPC (no PMM directly)         consumers — MMU,
                                                handle tables, …
```

Each layer is small and one-purpose. The PMM tracks the whole
machine. Above it, two consumers carve their own pools. The
buddy is the one that's *swappable* — strategy is its job, and
the slot makes it a config decision.

---

## A few extra things to know

- **Per-CPU caches don't exist in nonux yet.** A real kernel
  hands every CPU a small per-CPU stash of free pages so
  hot-path allocations don't need to touch the global bitmap
  at all. nonux's PMM goes straight to the bitmap on every
  call. That's fine at our scale — the test suite measures
  microsecond-range path lengths — but it's the obvious
  scaling next-step once SMP support lands.

- **The PMM does not zero pages.** A page handed back from
  `pmm_alloc_page` contains whatever the previous owner wrote
  in it. Layers above the PMM that need zero-initialised
  memory have to do their own `memset`. The reason is policy:
  the PMM doesn't know whether the caller wants zero, garbage,
  or some specific pattern (filesystems sometimes prefill new
  pages with magic bytes for debugging). Pushing the choice
  one layer up keeps the PMM fast and predictable.

- **The buddy's pool is small on purpose.** 16 pages is a tiny
  amount of memory, but it's enough to exercise every code
  path: split-on-alloc, coalesce-on-free, refuse-when-full.
  Tests that drive the buddy can deliberately exhaust it and
  observe the failure mode in microseconds. A production
  allocator would size its pool much larger or grow on demand,
  but the algorithm wouldn't change — and that's the part the
  small pool teaches.

- **`pmm_alloc_pages` does not guarantee alignment beyond
  page granularity.** A request for 4 contiguous pages can
  return *any* 16 KiB-aligned address — well, any
  4 KiB-aligned address, since first-fit will hand you the
  first run of 4 free pages it finds. If you need 16 KiB
  alignment for a page table, you have to either use the
  buddy (which gives you order-2 alignment by construction)
  or ask for a single block whose contiguity buys you the
  alignment. The PMM is intentionally stupid here.

- **`g_free` is `_Atomic`, but the bitmap bytes are not.**
  The bitmap bytes are accessed only through `__atomic_*`
  builtins, which work on plain bytes; declaring them
  `_Atomic` would force the compiler to generate atomic
  loads for *every* read, even ones that don't need them
  (like the `bit_test` prefilter). `g_free`, by contrast, is
  read and written from many places that *don't* go through
  atomic builtins, so the type-level `_Atomic` is what
  guarantees a non-tearing access there.

- **There's no "kernel free list".** The PMM doesn't keep a
  linked list of free pages, only a bitmap of allocations.
  Many pedagogical kernels (and earlier-era Linux) used a
  free list — each free page held a `next` pointer in its
  first word. The bitmap design has the trade-off:
  allocations have to scan to find a free bit (more work),
  but freeing is constant-time and the bookkeeping never
  touches caller-owned memory (a freed page is *intact* —
  bit-for-bit unchanged from when it was given back, so the
  PMM can hand it out again with no surprise reads). nonux's
  slab heap uses the same property to keep its slab-fill
  invariants simple.

- **Memory is the simplest of the four core resources.** The
  others — CPU time, IRQs, I/O bandwidth — all need
  scheduling. Memory needs only bookkeeping: it's stateless
  outside the bitmap, and a freed page is identical to a
  never-allocated one. That's why the PMM is the smallest
  subsystem in the kernel and is one of the first ones to
  come up.

---

## Where to read more

- [`core/pmm/pmm.c`](../core/pmm/pmm.c) — the full PMM, ~200
  lines.
- [`components/mm_buddy/mm_buddy.c`](../components/mm_buddy/mm_buddy.c)
  — the buddy component, ~250 lines.
- [`components/mm_buddy/README.md`](../components/mm_buddy/README.md)
  — component-level overview, including how it's bound to
  `memory.page_alloc` in `kernel.json`.
- [`interfaces/idl/mm.json`](../interfaces/idl/mm.json) — the
  IDL source the `nx_mm_ops` contract is generated from. We'll
  walk the IDL toolchain in chapter 10.
- [`core/lib/kheap.h`](../core/lib/kheap.h),
  [`core/lib/kheap.c`](../core/lib/kheap.c) — how
  `malloc`/`free` for kernel code is implemented on top of the
  PMM. The slab story is small and lives entirely in those two
  files.
- [Chapter 1 §"The linker script"](01-boot-and-linker.md) —
  introduces `__free_mem_start`, `__kernel_end`, and the
  linker's role in laying out the kernel image.
- ARM Architecture Reference Manual for ARMv8-A (DDI0487),
  chapter "AArch64 memory model" — defines what 4 KiB granule
  means architecturally and the alignment rules each level of
  page table requires.
- Linux's [`mm/page_alloc.c`](https://elixir.bootlin.com/linux/latest/source/mm/page_alloc.c)
  — the production buddy allocator. Read after this chapter to
  see what the same idea looks like at industrial scale: per-
  CPU caches, watermarks, anti-fragmentation, NUMA, and
  half a dozen other knobs.
- [QEMU `hw/arm/virt.c`](https://gitlab.com/qemu-project/qemu)
  — where the RAM-base / RAM-size constants the boot code
  hardcodes come from. Eventually the kernel should read these
  from the device tree the bootloader hands it; for now the
  source code is the reference.
