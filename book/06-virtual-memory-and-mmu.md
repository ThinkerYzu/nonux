# Virtual memory and the MMU

Up to now we've been a little loose with the word **address**.
The boot code prints "kernel loaded at `0x40080000`", the PMM
hands out pages by their numeric address, and we've been
treating those numbers as if they reached the DRAM chips
directly. That was true on the very first instruction the CPU
ever ran — but it stopped being true two lines later, when
`boot_main` called `mmu_init` and turned the MMU on.

Once the MMU is on, every load, store, and instruction fetch
the CPU performs is *translated* before it leaves the core.
The number you put in a register is a **virtual address**; the
hardware walks a tree of tables in RAM to figure out which
**physical address** that virtual one maps to. Translation
happens billions of times a second. It is one of the two or
three most important pieces of machinery in any modern
operating system.

In this chapter we wire up the MMU, walk through the page-table
shape nonux uses, and pin down what every bit in a descriptor
means. Then we carve out a small region of the address space
that user programs will eventually be allowed to touch, and
build per-process address spaces that hand each process a
private slice of that region. By the end you should be able to
read [`core/mmu/mmu.c`](../core/mmu/mmu.c) line by line and
know exactly what the CPU is doing on every memory access the
kernel makes.

The relevant files in this repo:

- [`core/mmu/mmu.h`](../core/mmu/mmu.h),
  [`core/mmu/mmu.c`](../core/mmu/mmu.c) — the MMU bring-up,
  the descriptor builders, and per-process address-space
  management. About 500 lines including comments.
- [`core/boot/boot.c`](../core/boot/boot.c) — the call to
  `mmu_init` near the top of `boot_main`, before the PMM and
  before the GIC.
- [`core/cpu/el0_entry.S`](../core/cpu/el0_entry.S) —
  `drop_to_el0`, the one-way jump from kernel mode into a user
  program. The MMU is what makes "running at EL0" actually
  enforce anything.
- The ARM Architecture Reference Manual for ARMv8-A (DDI0487),
  chapter "AArch64 memory model" — the canonical reference
  for descriptor formats, MAIR, TCR, and the translation
  process. Linked at the end of the chapter.

---

## Terms you'll see

- **Virtual address (VA).** The address a program puts in a
  register. The CPU translates it into a physical address
  before reaching memory. Same machine instructions, but the
  numbers in pointers no longer name DRAM directly.
- **Physical address (PA).** The address that names an actual
  byte on a DRAM chip (or in QEMU's case, a byte in the host
  memory pretending to be DRAM). The output of address
  translation.
- **MMU (Memory Management Unit).** The hardware inside the
  CPU that translates VAs to PAs on every memory access. On
  ARMv8 it lives on each CPU core and is configured through
  system registers; on a multi-core machine each core has its
  own MMU state.
- **Page table.** The tree of tables in RAM that tells the MMU
  how to translate. Each level is a 4 KiB page containing 512
  entries (on the 4 KiB-granule configuration we use).
- **Descriptor.** One 8-byte entry in a page table. It either
  points at the next level (a *table* descriptor), points at a
  block of memory and ends the walk (a *block* or *page*
  descriptor), or is invalid.
- **Granule.** The leaf page size the MMU is configured to
  use. ARMv8 supports 4 KiB, 16 KiB, and 64 KiB granules. We
  use the 4 KiB granule, which is also what Linux uses by
  default on ARM64.
- **Translation regime.** A complete recipe for translating
  one set of VAs: which root table to start at, how many
  levels to walk, what permissions to enforce, what
  cacheability and ordering rules to apply. EL1 has a
  different translation regime from EL0 sometimes, and a
  hypervisor at EL2 has its own as well. nonux uses the
  EL1&0 regime, which means EL1 and EL0 share translation
  state.
- **TTBR0_EL1.** "Translation Table Base Register 0 for
  EL1". Holds the physical address of the root page table
  for the lower half of the virtual-address space. This is
  what nonux writes when it switches between processes.
- **TTBR1_EL1.** The same idea for the upper half. ARMv8
  splits the address space and gives each half its own root
  so the kernel and the running process can both have their
  mappings live at once. nonux currently has TTBR1 disabled
  (`TCR.EPD1 = 1`); the kernel lives in TTBR0 along with the
  process. A future "kernel in the high half" rework will
  turn it on.
- **MAIR_EL1.** "Memory Attribute Indirection Register". A
  64-bit register that holds eight 8-bit attribute slots. A
  descriptor doesn't carry the full memory type itself —
  it carries a 3-bit *index* into MAIR, and MAIR turns that
  index into the actual attributes (Normal vs Device,
  cacheability, write-back vs write-through, …).
- **TCR_EL1.** "Translation Control Register". A pile of
  flags that tell the MMU how to interpret TTBR0 and TTBR1:
  how many bits of VA to use, what granule, where to start
  walking, whether the table walks themselves are cached,
  and so on.
- **SCTLR_EL1.** "System Control Register". Bit 0 (the `M`
  bit) is what actually turns the MMU on. Other bits in the
  same register control the data cache, the instruction
  cache, alignment checking, and a long list of policy
  knobs.
- **Identity map.** A page table where VA = PA. The MMU
  still translates, but the answer it produces is the same
  number it started with. It's the simplest possible
  mapping and the one nonux uses for the kernel.
- **AP (Access Permission).** Two bits in a descriptor that
  control read-vs-write and EL1-vs-EL0+EL1 access. Combined
  with PXN and UXN, this is how the MMU stops a user program
  from reading kernel memory.
- **PXN (Privileged eXecute-Never).** Bit 53 of a descriptor.
  When set, the page can't be executed at EL1 (the kernel)
  even if its other permissions would allow it. Used to
  mark device-MMIO pages and user-data pages as
  non-executable from kernel mode.
- **UXN (Unprivileged eXecute-Never).** Bit 54. The same
  idea for EL0 (user mode). nonux marks every kernel page
  UXN=1 so a user program can't jump to kernel code by
  abusing a stray pointer.
- **AF (Access Flag).** Bit 10. The architecture lets the
  MMU optionally trap the *first* access to each page so the
  OS can implement page-aging in software. nonux always
  sets AF=1 in its descriptors so accesses never trap on
  this — page-aging is not a feature we're building.
- **TLB (Translation Lookaside Buffer).** A small cache
  inside the MMU that remembers recent VA → PA
  translations. Without the TLB, every memory access would
  require a multi-level table walk. With it, the common
  case is one cycle.
- **ISB, DSB.** Two ARM barrier instructions. `dsb`
  ("Data Synchronization Barrier") waits for in-flight memory
  operations to complete; `isb` ("Instruction Synchronization
  Barrier") flushes the CPU's instruction-decode pipeline so
  the next fetch starts fresh. We need both around any
  change to the MMU's state.
- **`eret`.** "Exception Return". The instruction that
  drops back from a higher exception level (EL1) to a
  lower one (EL0), restoring PC, stack pointer, and flags
  from system registers. The MMU is what makes the demotion
  enforceable: the EL0 program literally cannot reach pages
  whose AP forbids EL0 access.

---

## Why turn on the MMU at all

A naïve reaction to all this is "why bother?" If we identity-map
everything anyway, the addresses don't change — the MMU
output equals its input. So why pay for the page-table walks?

Three reasons, in roughly increasing order of importance:

1. **Caching and memory ordering.** When the MMU is off, the
   architecture treats every data access as Device-nGnRnE: no
   caches, strict ordering, every data load and store goes to
   the bus. (Instruction fetches are architecturally
   *cacheable* in this state, but in practice the I-cache is
   also disabled until `mmu_init` turns it on alongside
   `SCTLR.M` and `SCTLR.C`, so nothing actually fills.) That's
   catastrophically slow. Turning the MMU on lets us tag RAM
   as **Normal** memory — cacheable, write-back, free to be
   reordered for performance — while keeping MMIO regions
   (the GIC, the PL011) tagged as **Device** so writes to
   them stay strictly ordered. This is the *single* biggest
   reason real kernels turn the MMU on as early as possible.
2. **Permissions.** Once the MMU is on, every page has bits
   that say "EL0 can read this", "EL1 can execute this",
   "this page is read-only", and so on. Without these bits,
   *every* program would be able to write to kernel code.
   The MMU is the entire mechanism enforcing the kernel/user
   boundary; without it, "EL0" is just a register-file mode
   with no real isolation.
3. **Address-space isolation.** A process's notion of
   "address `0x48000000`" can mean different physical bytes
   in different processes, because each process has its own
   tree of page tables. Two processes hand the same pointer
   to themselves, and the MMU silently routes the access to
   different RAM. *That* is what makes the word
   "process" mean something more than "thread with a
   different stack".

nonux turns the MMU on right after `uart_init`, before
anything that benefits from caching:

```c
mmu_init();
kprintf("[mmu]  enabled (identity map: MMIO 0..1G Device, RAM 1..2G Normal)\n");
```

After that line runs, every address the kernel ever uses goes
through translation. We use the simplest possible
configuration — virtual equals physical — so no kernel symbol
*moves*, but each access is now classified, cached, and
permission-checked.

> **Side note: why not later?** Three reasons.  First, cache
> and TLB state at reset is *architecturally undefined* —
> the cache may hold valid lines with garbage from firmware,
> speculative prefetches, or simulator wackiness — so the
> kernel must invalidate both before relying on them.  Doing
> that early, before there's anything in memory the kernel
> cares about, keeps the dance simple.  (The kernel's own
> pre-MMU stores aren't a problem in the way one might
> expect: with MMU off everything is Device-nGnRnE, so those
> stores commit to memory directly without entering the
> cache.  But a stale cache line left over from reset *could*
> shadow them after the cache is enabled, which is what makes
> the invalidate-then-enable order non-negotiable.)  Second,
> running with MMU off means every load and store is
> Device-typed: slow, strictly ordered, and `LDXR/STXR`
> atomics may not behave as expected.  Third, a bad-pointer
> access at EL1 produces a clean synchronous abort once the
> MMU is on; without it you'd get a bus error at best and
> silent corruption at worst.  Turning the MMU on right at
> the top of `boot_main` collects all three benefits
> immediately, and the only thing the kernel has touched
> before that point is the UART — which stays Device-typed,
> so it doesn't care.

---

## What we're going to build

nonux's MMU configuration uses three knobs:

- **4 KiB granule** — the leaf page size.
- **39-bit virtual addresses** — `T0SZ = 25`, so VAs run from
  `0x0000_0000_0000_0000` to `0x0000_007F_FFFF_FFFF` (512 GiB).
  Anything above that is unmapped.
- **Three levels of page table** — L1, L2, L3. With 4 KiB
  granule and 39-bit VAs, the walk starts at L1.

The three levels carve a 39-bit VA into four pieces:

```
 38                30 29              21 20            12 11             0
+---------------------+-------------------+----------------+----------------+
| L1 index  (9 bits)  | L2 index (9 bits) | L3 idx (9 bits)| offset(12 bits)|
+---------------------+-------------------+----------------+----------------+
       1 GiB                  2 MiB             4 KiB            byte
       per L1 entry         per L2 entry      per L3 entry    within page
```

So:

- An **L1 entry** covers 1 GiB of VAs. It can either
  point at an L2 table (table descriptor — keep walking) or
  point at a 1 GiB block (rare; we don't use that).
- An **L2 entry** covers 2 MiB. It can point at an L3 table
  or be a 2 MiB *block descriptor* and end the walk right
  there.
- An **L3 entry** covers 4 KiB — one page. It can be a *page
  descriptor* (end of walk) or invalid.

nonux's `mmu_init` builds a tiny tree: one L1 with two valid
entries pointing at two L2 tables, and the L2 entries are all
2 MiB block descriptors. The walk reaches an answer in two
steps. We never touch L3 in the kernel-side identity map —
2 MiB blocks are big enough for the whole kernel.

Per-process address spaces use the same shape: one fresh L1,
one fresh L2_ram (the L2 covering the RAM range), and again
2 MiB blocks. A future rework swaps the user-window slots out
for L3 pages so we can do per-page demand paging and
copy-on-write fork; that's flagged in `mmu.h` and the per-process
section below.

---

## Descriptors: the bit-level layout

A descriptor is one 64-bit value.  The bit layout is the
same across L1, L2, and L3 except that bit 1 tells the MMU
"this is a table descriptor; keep walking" (`Tbl` = 1 at
L1/L2) versus "this is a block / page descriptor; the walk
ends here" (`Blk` = 0 at L1/L2; bit 1 must be 1 for valid
page descriptors at L3).  The fields nonux uses, ordered
from low bit to high:

| Bits   | Name      | Meaning                                                    |
|--------|-----------|------------------------------------------------------------|
| 0      | V         | Valid (0 = no mapping; accesses fault)                     |
| 1      | Tbl/Blk   | At L1/L2: 1 = table descriptor, 0 = block. At L3: must be 1 for a valid page. |
| 4..2   | AttrIdx   | 3-bit index into MAIR_EL1 (picks memory type)              |
| 7..6   | AP[2:1]   | Access permission (R/W vs RO; EL1-only vs EL0+EL1)         |
| 9..8   | SH        | Shareability                                               |
| 10     | AF        | Access Flag (always set in nonux)                          |
| 47..12 | PA        | Output address: next-table base for table descriptors; block / page base for block / page descriptors |
| 53     | PXN       | Privileged eXecute-Never (no execution at EL1)             |
| 54     | UXN       | Unprivileged eXecute-Never (no execution at EL0)           |

(Bit 11 nG, bit 52 contiguous-hint, bits 51..48, and bits 63..55
software-use are all left zero in nonux's descriptors.)

In more detail:

- **`V` (bit 0)** — *Valid.* Zero means "no mapping; access
  here faults."
- **`Tbl/Blk` (bit 1)** — At L1 and L2, distinguishes a table
  descriptor (1) from a block descriptor (0). At L3 there
  are no blocks; bit 1 must be 1 for valid page descriptors
  and is just part of the magic value `0b11`.
- **`AttrIdx` (bits 2..4)** — A 3-bit index into MAIR_EL1.
  Picks which 8-bit attribute slot describes this page's
  memory type.
- **`AP` (bits 6..7)** — Two access-permission bits. The
  encoding is non-obvious: bit 6 selects "EL0 access
  allowed" (0 = EL1 only, 1 = EL0+EL1), and bit 7 selects
  read-only (0 = R/W, 1 = read-only). Of the four AP
  encodings, nonux uses two: kernel R/W (`AP = 0b00`) and
  user R/W (`AP = 0b01`). Read-only mappings would be
  encoded by setting bit 7, but no current call site needs
  one.
- **`SH` (bits 8..9)** — *Shareability.* Tells the MMU
  whether other observers (other CPUs, DMA agents) need to
  see writes. We use Inner Shareable for RAM (`SH = 0b11`)
  and unshared for MMIO (`SH = 0b00`).
- **`AF` (bit 10)** — *Access Flag.* If clear, the MMU
  traps the first access to the page. We always set it.
- **`PA`/table base (bits 12..47)** — The high bits of the
  next-level table address (for table descriptors) or the
  block's physical base (for block / page descriptors). The
  low 12 bits are zero because everything is 4 KiB-aligned.
- **`PXN` (bit 53)** — *Privileged eXecute-Never.* No
  execution at EL1 from this page.
- **`UXN` (bit 54)** — *Unprivileged eXecute-Never.* No
  execution at EL0 from this page.

In `core/mmu/mmu.c` these are wrapped in named macros:

```c
#define DESC_VALID        (1UL << 0)
#define DESC_TABLE        (1UL << 1)
#define DESC_ATTR_IDX(n)  ((uint64_t)((n) & 0x7) << 2)
#define DESC_AP_EL1_RW    (0UL << 6)   /* AP[2:1] = 0b00 — EL1-only RW */
#define DESC_AP_USER_RW   (1UL << 6)   /* AP[2:1] = 0b01 — EL0+EL1 RW */
#define DESC_SH_INNER     (3UL << 8)
#define DESC_SH_NONE      (0UL << 8)
#define DESC_AF           (1UL << 10)
#define DESC_PXN          (1UL << 53)
#define DESC_UXN          (1UL << 54)
```

(In `mmu.c` itself, `DESC_AP_USER_RW` is defined later — immediately
above its only consumer, `user_block` — rather than alongside the
rest of the descriptor vocabulary.  We've collected all the macros
here for reading clarity.)

…and three small helpers build the three kinds of block
descriptors we use:

```c
/* Device block: RW at EL1, never executable, no cache. */
static inline uint64_t device_block(uint64_t pa)
{
    return pa | DESC_VALID |
           DESC_ATTR_IDX(ATTR_IDX_DEVICE) |
           DESC_AP_EL1_RW | DESC_SH_NONE |
           DESC_AF | DESC_PXN | DESC_UXN;
}

/* Normal-memory block: RW + executable at EL1, no EL0 exec. */
static inline uint64_t normal_block(uint64_t pa)
{
    return pa | DESC_VALID |
           DESC_ATTR_IDX(ATTR_IDX_NORMAL) |
           DESC_AP_EL1_RW | DESC_SH_INNER |
           DESC_AF | DESC_UXN;
}

/* User-accessible Normal block: RW at both EL0 and EL1. */
static inline uint64_t user_block(uint64_t pa)
{
    return pa | DESC_VALID |
           DESC_ATTR_IDX(ATTR_IDX_NORMAL) |
           DESC_AP_USER_RW | DESC_SH_INNER |
           DESC_AF;
}
```

These three helpers are the *entire* descriptor vocabulary
nonux uses. Every entry in every page table the kernel
builds is one of `device_block`, `normal_block`, or
`user_block` — plus *table* descriptors (`pa | DESC_VALID
| DESC_TABLE`) at L1.

Three things to notice:

- **`device_block` is PXN+UXN.** No code ever runs from MMIO,
  whether you're the kernel or a user program. Returning
  from a function pointer that happened to land in MMIO
  would fault.
- **`normal_block` is UXN-only.** The kernel can execute
  here (PXN unset); EL0 cannot. This is what stops a user
  program from jumping into kernel code via a stray pointer
  even when it has access to the same address space.
- **`user_block` clears UXN.** The user-window pages need to
  be executable from EL0 — that's where the user program's
  `.text` ends up. PXN stays unset too, which means kernel
  code could in principle execute from there as well; we
  rely on never doing that.

---

## MAIR: indirection for memory types

`AttrIdx` is 3 bits — eight possible indices. Each index
points at one slot in MAIR_EL1, an 8-bit field that describes
the actual memory type:

```c
#define MAIR_ATTR_DEVICE_nGnRnE  0x00UL
#define MAIR_ATTR_NORMAL_WBWA    0xFFUL   /* Inner WB-WA + Outer WB-WA */

#define ATTR_IDX_DEVICE  0
#define ATTR_IDX_NORMAL  1

#define MAIR_VALUE  ((MAIR_ATTR_DEVICE_nGnRnE << (ATTR_IDX_DEVICE * 8)) | \
                     (MAIR_ATTR_NORMAL_WBWA   << (ATTR_IDX_NORMAL * 8)))
```

Two indices are populated:

- **Index 0** holds `0x00` — *Device-nGnRnE*: the most
  strictly ordered memory type ARM defines. No gathering,
  no reordering, no early write acknowledgement. Used for
  MMIO so that "write 1 to this register" really does mean
  *write 1, in this order, right now*.
- **Index 1** holds `0xFF` — *Normal Inner WB-WA, Outer
  WB-WA*. Cached, write-back, write-allocate at both
  cache levels. The standard "real RAM" attribute. Reads
  and writes are fast; the cache is free to coalesce and
  reorder them.

The other six MAIR slots stay at zero. We could populate them
to support things like "Normal non-cacheable" (useful for DMA
buffers shared with hardware), but nonux doesn't need them
yet, so they're just unused indices that we never reference.

> **Side note: why an indirection at all?** A descriptor is
> already 64 bits; surely there's room to encode an 8-bit
> attribute directly? The reason ARM uses an indirection is
> that an 8-bit attribute is *complicated* — it expresses
> Device vs Normal, write-back vs write-through, inner vs
> outer cacheability, gather/order/early-ack policies, and
> more. Stuffing all that into every descriptor would
> double the descriptor's "useful" size. Instead, the
> architecture trades 3 descriptor bits for 64 register
> bits and lets the OS choose up to 8 attribute *kinds*.
> The cost is one extra 64-bit register write at boot
> (`msr mair_el1, …`); the benefit is room in every
> descriptor.

---

## TCR: telling the MMU what to expect

`TCR_EL1` is the "translation control register". It's a
pile of flags that configure how the MMU walks the trees
rooted at TTBR0 and TTBR1.

```c
#define TCR_VALUE   ((25UL << 0)  | \
                     (1UL  << 8)  | \
                     (1UL  << 10) | \
                     (3UL  << 12) | \
                     (0UL  << 14) | \
                     (1UL  << 23) | \
                     (1UL  << 32))
```

Bit by bit:

- **`T0SZ = 25` (bits 0..5).** "Use 64 − 25 = 39 bits of VA
  for TTBR0 walks". This is what fixes our address-space
  size at 512 GiB. T0SZ = 16 would give the full 48-bit
  ARMv8 VA range; 25 keeps the page tables small.
- **`IRGN0 = 0b01` (bits 8..9).** "Page-table walks for
  TTBR0 are themselves *Normal Inner WB-WA*." Without
  this, the MMU would do a full uncached fetch on every
  walk — terrible for performance. Setting it to WB-WA
  lets the walker reuse cache lines across walks.
- **`ORGN0 = 0b01` (bits 10..11).** Same as IRGN0 but for
  the outer cacheability policy.
- **`SH0 = 0b11` (bits 12..13).** The walks themselves are
  *Inner Shareable*. This guarantees other CPUs see the
  page-table updates the kernel makes. (Not really
  meaningful on a single-core build like ours, but the
  walker still uses this to decide whether the table reads
  hit a shared cache.)
- **`TG0 = 0b00` (bits 14..15).** *Granule = 4 KiB.* The
  other valid choices are 16 KiB and 64 KiB; we pick the
  smallest one because it gives the finest-grained
  permission granularity.
- **`EPD1 = 1` (bit 23).** "*Disable* TTBR1 walks." We
  don't use the high half yet, so any access to a VA in
  the upper-half range faults rather than walking a
  garbage table.
- **`IPS = 0b001` (bits 32..34).** *Intermediate Physical
  address Size = 36 bits.* This is plenty for QEMU's
  4 GiB-or-less RAM map and matches our identity map's
  needs.

The other bits stay at zero, which gives us the conservative
defaults (no top-byte ignored, no hardware access-flag
management, no granule overrides for TTBR1, …). nonux is one
of the simplest TCR values you'll ever see.

---

## The tables themselves

`mmu_init` builds three statically-allocated tables:

```c
static uint64_t l1_table[512]      __attribute__((aligned(4096)));
static uint64_t l2_mmio_table[512] __attribute__((aligned(4096)));
static uint64_t l2_ram_table[512]  __attribute__((aligned(4096)));
```

All three are `static` (so they live in `.bss` and are zeroed
by `start.S` before `boot_main` runs) and 4 KiB-aligned (so
their physical addresses fit in the descriptor's PA field
with no shift).

The L2 tables are populated identically, slot by slot:

```c
for (uint64_t i = 0; i < 512; i++) {
    l2_mmio_table[i] = device_block(MMIO_BASE + (i << BLOCK2_SHIFT));
    l2_ram_table[i]  = normal_block(RAM_BASE  + (i << BLOCK2_SHIFT));
}
```

`BLOCK2_SHIFT = 21`, so `i << 21` walks through 0, 2 MiB, 4
MiB, …, 1 GiB − 2 MiB. Each entry's `pa` field is exactly the
2 MiB-aligned base of the block it represents. After the
loop, `l2_mmio_table` covers `0x00000000`..`0x40000000` (the
device range — GIC, UART, all MMIO) with Device blocks, and
`l2_ram_table` covers `0x40000000`..`0x80000000` (the RAM
range) with Normal blocks.

Then the L1 wires the two L2s into place:

```c
l1_table[0] = (uint64_t)l2_mmio_table | DESC_VALID | DESC_TABLE;
l1_table[1] = (uint64_t)l2_ram_table  | DESC_VALID | DESC_TABLE;
```

That's the entire L1: two valid table descriptors at slots 0
and 1, everything else invalid. Slot 0 covers VAs
`0x0..1G`, slot 1 covers VAs `1G..2G`, slots 2..511 are
unmapped. Trying to access a VA above 2 GiB faults at the L1
level immediately.

> **Side note: where did the table addresses come from?** We
> wrote the *kernel-virtual* address of `l2_mmio_table` into
> the L1 entry, not its *physical* address. That's only
> correct because we're identity-mapped — the address the C
> code sees is also the physical address of the table. A
> design that ran the kernel out of high-half VAs (with
> TTBR1) would need to translate `&l2_mmio_table` into a PA
> before stuffing it into the L1 entry. nonux gets to skip
> that translation step because identity is identity.

> **Side note: no L3 tables in this configuration.**  Notice
> we never declare an `l3_*_table`.  The L2 entries we built
> above are block descriptors (`Tbl/Blk = 0`), so the walk
> ends at L2 — the MMU never asks for an L3.  The 4 KiB
> granule (TG0 = 0b00) defines a third level, but L3 is
> optional when 2 MiB granularity suffices, and nonux today
> is 2 MiB-aligned everywhere.  Phase 9 introduces L3 tables
> for the user window (`Tbl/Blk = 1` at L2, pointing at an L3
> of 4 KiB page descriptors); until then, the L3 rules in
> §"Descriptors" are prescriptive, not descriptive of today's
> code.

---

## The walk, by example

To make the table layout concrete: what happens when the CPU
issues a load from VA `0x4009_0000` (somewhere in the kernel
text)?

1. **L1 lookup.** Bits 38..30 of the VA are
   `0b001` = 1. The MMU reads `l1_table[1]`, finds a
   valid table descriptor pointing at `l2_ram_table`.
2. **L2 lookup.** Bits 29..21 are
   `0x0` for everything in the first 2 MiB of RAM. So
   `l2_ram_table[0]` is read, which is a *block descriptor*
   `normal_block(0x40000000)`. The walk ends here.
3. **Combining.** The block covers VAs `0x40000000`..
   `0x40200000` (a 2 MiB region). The block's PA base is
   `0x40000000`. The 21-bit offset within the block (bits
   20..0 of the VA, = `0x90000`) is added to the PA base.
   Result: PA `0x40090000`.

VA `0x4009_0000` → PA `0x40090000`. Identity, just as
promised. But the access went through the MMU's translation
machinery, hit the cache (because the descriptor said
"Normal WB-WA"), and was permission-checked
(`AP = 0b00`, EL1-only, fine because we're at EL1).

A load from VA `0x0900_1000` (the PL011's `UARTDR` register)
walks differently:

1. L1[0] → `l2_mmio_table`.
2. L2[72] → `device_block(0x09000000)`.
3. PA `0x09001000`.

Same kind of walk, but the descriptor's MAIR index says
Device-nGnRnE, so the access bypasses the cache and goes
straight to the bus — which is exactly what writing to a
device register requires.

---

## Turning the MMU on

After the tables are built, `mmu_init` programs the system
registers and flips the enable bit. This sequence is
delicate; every line is here for a reason.

```c
asm volatile ("dsb ish" ::: "memory");

asm volatile ("msr mair_el1, %0"  :: "r"(MAIR_VALUE));
asm volatile ("msr tcr_el1,  %0"  :: "r"((uint64_t)TCR_VALUE));
asm volatile ("msr ttbr0_el1, %0" :: "r"((uint64_t)(uintptr_t)l1_table));
asm volatile ("isb");

asm volatile ("tlbi vmalle1"  ::: "memory");
asm volatile ("dsb ish"       ::: "memory");
asm volatile ("ic iallu"      ::: "memory");
asm volatile ("dsb ish"       ::: "memory");
asm volatile ("isb");

uint64_t sctlr;
asm volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));
sctlr |= (1UL << 0);   /* M  — MMU enable              */
sctlr |= (1UL << 2);   /* C  — D-cache enable          */
sctlr |= (1UL << 12);  /* I  — I-cache enable          */
asm volatile ("msr sctlr_el1, %0" :: "r"(sctlr) : "memory");
asm volatile ("isb");
```

Walking the steps:

1. **`dsb ish`** — make sure all the table writes we just
   did have actually drained out to memory before we tell
   the MMU about them. The "ish" variant means "wait
   until the inner-shareable domain agrees".
2. **`msr mair_el1`** — install the attribute table.
3. **`msr tcr_el1`** — install the translation-control flags.
4. **`msr ttbr0_el1`** — install the L1 root pointer. At
   this point the MMU *could* translate, but `SCTLR.M` is
   still 0, so it doesn't.
5. **`isb`** — flush the CPU's instruction-decode pipeline
   so that any subsequent instruction is fetched after the
   register writes have taken effect. Without this, an
   instruction fetched *just before* the `msr` could
   speculatively use the old MMU state when it executes.
6. **`tlbi vmalle1`** — invalidate every entry in the EL1
   TLB. Whatever stale translations it might have cached
   from before — even though "before" means "before the MMU
   was on", which sounds like there's nothing to invalidate
   — are blown away. Better safe than wrong.
7. **`dsb ish`** — wait for the TLB invalidation to take
   effect.
8. **`ic iallu`** — invalidate every entry in the
   instruction cache. Same reasoning; previously fetched
   instructions might have been cached based on an "MMU off"
   regime.
9. **`dsb ish` + `isb`** — drain the I-cache invalidation
   and flush the pipeline again.
10. **Read SCTLR, set M+C+I, write back.** Three bits flip
    in one register write: MMU on, D-cache on, I-cache on.
    The `isb` after this is the magic moment — the *next
    instruction fetched* goes through the translation
    table, hits the I-cache, and lands in PA = VA. Because
    we mapped both the kernel's code and the kernel's stack
    identity-mapped, execution continues without skipping a
    beat.

> **Side note: why does the kernel survive its own MMU enable?**
> This is the question that makes people the most nervous
> the first time they bring up an MMU. The answer: the
> kernel is identity-mapped, so the VA `0x40090000` (the
> next instruction after the `isb`) is mapped to PA
> `0x40090000` (the actual byte the I-cache will fetch).
> The MMU is on, but the *answer* it produces is the same
> answer it would produce if it were off. If the kernel
> were *not* identity-mapped — if `mmu_init` were trying to
> rebase itself to a high-half VA — the `isb` after the
> SCTLR write would crash, because the next fetch would
> happen at the old PC value but be translated through a
> table that doesn't have a mapping there. Some kernels
> handle that with a small trampoline that does the SCTLR
> write under a temporary identity map and then jumps to
> the real high-half symbol. nonux skips all that by
> staying identity-mapped forever.

After `mmu_init` returns, `mmu_is_enabled` (a one-line helper
that just reads `SCTLR.M`) returns 1, and every memory
access for the rest of the kernel's lifetime goes through the
MMU.

---

## One more boot-time tweak: FP/SIMD

Tucked at the end of `mmu_init` is a small unrelated change
to `CPACR_EL1`:

```c
uint64_t cpacr;
asm volatile ("mrs %0, cpacr_el1" : "=r"(cpacr));
cpacr |= (3UL << 20);  /* FPEN = 0b11 — no trap on FP/SIMD */
asm volatile ("msr cpacr_el1, %0" :: "r"(cpacr) : "memory");
asm volatile ("isb");
```

`CPACR_EL1.FPEN` controls whether floating-point and SIMD
instructions trap. The default after reset is "trap from
both EL0 and EL1", which would make the very first `fmov`
in any user program take an exception. We set it to "don't
trap from any EL", which means EL0 user code can use FP/SIMD
freely (musl's `memset` and `memcpy` do, for instance, on
ARM64). The kernel itself is built with `-mgeneral-regs-only`
so it never touches FP, but we still have to allow EL0 to.

This lives in `mmu_init` because the same boot phase wraps up
"per-core CPU configuration before bring-up". Future code
that adds a real FP context-save on context switch will
still toggle this on, just with a richer save/restore around
each thread switch.

---

## The user window

So far the kernel's address space contains the kernel itself
and MMIO. Where do user programs go?

Look closely at what `mmu_init` does after populating the L2
tables:

```c
l2_ram_table[USER_WINDOW_INDEX] =
    user_block(RAM_BASE + ((uint64_t)USER_WINDOW_INDEX << BLOCK2_SHIFT));
```

`USER_WINDOW_INDEX` is `64`. `64 << 21` = `0x0800_0000`, so
the PA is `RAM_BASE + 0x0800_0000` = `0x4800_0000`. The slot
that previously held a `normal_block` at that PA is overwritten
with a `user_block` — same PA, same Normal-memory typing, but
EL0 now has access (via `DESC_AP_USER_RW`) and EL0 execution
is allowed (UXN cleared in `user_block`).

That single descriptor change is enough to make a 2 MiB
window — VA `0x4800_0000`..`0x4820_0000` — accessible from
user code. A user program whose `.text` and stack live in
that 2 MiB region runs unmodified.

(Two helpers in `mmu.h` expose the window's bounds:
`mmu_user_window_base()` returns `0x4800_0000`,
`mmu_user_window_size()` returns 8 MiB. The 8 MiB comes from
*per-process* address spaces, which we'll see next; the
kernel's own L2_ram opens just a single 2 MiB block as a
shared launchpad.)

This is enough machinery to support the simplest kind of
"user mode" — a kthread that copies a small EL0 program into
the window, then calls `drop_to_el0` to run it. The
[`core/cpu/el0_entry.S`](../core/cpu/el0_entry.S)
implementation of `drop_to_el0` is short:

```
drop_to_el0:
    msr     sp_el0, x1               /* initial user stack */
    msr     elr_el1, x0              /* entry PC on eret */
    mov     x2, #0
    msr     spsr_el1, x2             /* SPSR for EL0t, IRQ unmasked */
    /* …zero the GP registers… */
    eret
```

`eret` reads `SPSR_EL1` (which we set to "EL0t, IRQ unmasked"),
restores PC from `ELR_EL1`, restores SP from `SP_EL0`, and
demotes the CPU to EL0. From this point on:

- The CPU is at EL0.
- The MMU is still on (turning it off requires an `msr
  SCTLR_EL1`, which EL0 cannot execute — the AP bits would
  fault any attempt).
- Any access to a non-`user_block` page faults
  immediately, because `AP = 0b00` denies EL0.
- The only way back to EL1 is through an exception — an
  `svc` (system call), an IRQ, or a fault. That's chapter 16
  territory.

So the user window is the *only* part of physical RAM a user
program can read or write. The MMU does the enforcing; the
kernel just sets the bits.

---

## Per-process address spaces

A single shared user window is fine for one user program at a
time, but a real kernel needs every process to *think* it
owns its own copy of memory at the same virtual address.
That's what `mmu_create_address_space` does.

The trick is that two processes can have different page tables
even though they share the same kernel. Specifically: if
process A's TTBR0 points at one L1, and process B's TTBR0
points at a *different* L1, and both L1s have the kernel
mappings (slot 0 → L2_mmio, slot 1 → L2_ram-with-RAM-but-
different-user-window), then:

- Both processes see the kernel at the same VAs.
- Both processes see the user window at VA
  `0x4800_0000`..`0x4880_0000` (8 MiB).
- The user window resolves to *different* PAs in each
  process — different physical bytes, different content.

`mmu_create_address_space` builds one of these L1s per
process:

```c
struct mmu_address_space {
    uint64_t  l1_root_pa;     /* == (uintptr_t)l1_page */
    void     *l1_page;        /* 4 KiB L1 table */
    void     *l2_ram_page;    /* 4 KiB L2_ram */
    void     *user_backing;   /* 8 MiB user window (4 × 2 MiB) */
    void     *user_backing_raw;  /* unaligned malloc result */
};
```

Three fresh allocations per process:

1. **An L1 page.** 4 KiB, populated with two entries: slot
   0 reuses the *kernel's* `l2_mmio_table` (so MMIO mappings
   are identical across all processes — the kernel needs to
   talk to the GIC and UART regardless of which process is
   running), slot 1 points at the per-process L2_ram.
2. **A per-process L2_ram page.** 4 KiB, copied from the
   kernel's `l2_ram_table` (so the kernel's RAM mappings
   stay identical too), then four slots overwritten:

   ```c
   for (int i = 0; i < 512; i++) l2r[i] = l2_ram_table[i];
   for (uint64_t i = 0; i < USER_WINDOW_BLOCKS; i++) {
       l2r[USER_WINDOW_INDEX + i] =
           user_block(user_pa + (i << BLOCK2_SHIFT));
   }
   ```

   `USER_WINDOW_BLOCKS = 4`, so slots 64..67 (a contiguous
   8 MiB chunk of VAs starting at `0x4800_0000`) get
   `user_block` descriptors pointing at the per-process
   backing.
3. **An 8 MiB user backing.** A contiguous physical chunk
   pulled from the kernel heap. Each L2 block in the user
   window points at one 2 MiB slice of it.

Allocations 1 and 2 come from `kheap`'s `malloc`, which for
≥ 4 KiB requests pulls full pages from the PMM. Allocation
3 comes from the same source but needs 2 MiB alignment of
its first byte (since the L2 block descriptor's PA bits are
2 MiB-aligned), so the code over-allocates by one block and
rounds up:

```c
void *raw = malloc(USER_WINDOW_SIZE + USER_BLOCK_SIZE);
uint64_t user_pa = ((uint64_t)(uintptr_t)raw + USER_BLOCK_SIZE - 1) &
                   ~(USER_BLOCK_SIZE - 1);
```

The user backing is then zeroed (so a freshly-created process
sees a clean 8 MiB), and the per-process `mmu_address_space`
struct holds onto every pointer for later cleanup.

The fundamental property — kernel mappings *shared*, user
window *private* — falls out of the way the L2_ram is built:
512 entries copied identically from the kernel, four entries
overwritten. Slots 0..63 and 68..511 still point at the same
PAs the kernel sees, with the same `normal_block`
permissions; only slots 64..67 differ.

```
   per-process L1 (4 KiB)
   ├── [0] → kernel l2_mmio_table  (shared MMIO map)
   ├── [1] → per-process L2_ram     (private)
   └── [2..511] invalid

   per-process L2_ram (4 KiB)
   ├── [0..63]   = kernel l2_ram_table[0..63]   (shared RAM)
   ├── [64..67]  = user_block(user_backing + i*2 MiB)  (private)
   └── [68..511] = kernel l2_ram_table[68..511] (shared RAM)
```

> **Side note: a Phase 9 rework is on the calendar.** The
> 8 MiB-per-process model is rigid: every process reserves
> 8 MiB of contiguous physical RAM up front, even if its
> code is 800 bytes. A future per-process MM rework
> replaces the L2 block descriptors with L3 4 KiB pages,
> plus a list of *VMAs* describing what each VA range is
> for, plus demand paging (allocate on first fault) and
> copy-on-write fork. The shape of `mmu_create_address_space`
> changes a lot when that lands; the *concept* of "L1 with
> shared kernel mappings, private bottom 2 GiB" stays. See
> [`mmu.h`](../core/mmu/mmu.h) for the current contract.

---

## Switching address spaces

Switching from one process to another is one register write
plus a TLB flush:

```c
void mmu_switch_address_space(uint64_t l1_root)
{
    asm volatile ("msr ttbr0_el1, %0" :: "r"(l1_root) : "memory");
    asm volatile ("isb");
    asm volatile ("tlbi vmalle1" ::: "memory");
    asm volatile ("dsb ish"      ::: "memory");
    asm volatile ("isb");
}
```

The `msr ttbr0_el1, …` writes the new L1 root. The `isb` then
ensures subsequent instruction fetches use it. `tlbi vmalle1`
invalidates every TLB entry for the EL1&0 regime, because
TLB entries cached under the *old* TTBR0 are no longer
correct (they could resolve a user-window VA to the *previous*
process's PA). `dsb ish` waits for the invalidation; the
final `isb` is the standard "next instruction is now safe to
execute".

This works from kernel code because the kernel's own
mappings (which include the page containing the
instructions doing the switch) are *identical across all
roots*. The kernel code at PA `0x40090000` is reachable via
the same VA `0x40090000` no matter whose TTBR0 is current —
the L1 entry for slot 0 (MMIO) and slots 0..63 + 68..511 of
slot 1's L2 (RAM) are byte-for-byte identical in every
process's L1. Only the user window differs.

> **Side note: why not just write the new TTBR0 and skip the
> TLB flush?** Because the TLB caches translations *by VA*,
> and the same VA (e.g., `0x4800_0000`) translates to
> different PAs in different address spaces. Without the
> flush, the new process's first access to its user window
> could hit a stale cached translation belonging to the old
> process — and silently read or write someone else's data.
> A future optimisation called ASIDs (*Address Space
> IDentifiers*) lets the TLB tag entries with a small
> per-process identifier so that switches don't have to flush
> at all; nonux doesn't use ASIDs yet, hence the brute-force
> `tlbi vmalle1`.

---

## Fork's user-window memcpy

`fork` is the POSIX system call that creates a new process
that's an *exact copy* of the parent. In nonux's current
model, "exact copy" means *byte-for-byte memcpy of the user
window*. The function that does it is
`mmu_copy_user_backing(src_root, dst_root)`.

The interesting part isn't the memcpy — it's the address-space
juggling around the memcpy:

```c
void mmu_copy_user_backing(uint64_t src_root, uint64_t dst_root)
{
    void *src = mmu_address_space_user_backing(src_root);
    void *dst = mmu_address_space_user_backing(dst_root);
    if (!src || !dst || src == dst) return;

    uint64_t saved_ttbr0;
    asm volatile ("mrs %0, ttbr0_el1" : "=r"(saved_ttbr0));
    asm volatile ("msr ttbr0_el1, %0" :: "r"(mmu_kernel_address_space()) : "memory");
    asm volatile ("isb");

    memcpy(dst, src, USER_WINDOW_SIZE);

    asm volatile ("msr ttbr0_el1, %0" :: "r"(saved_ttbr0) : "memory");
    asm volatile ("isb");
    /* …cache-coherence sequence for freshly-copied code… */
}
```

Why does this need to switch TTBR0 to the kernel's root for
the copy?

Look at the situation. We arrive here from a syscall while
the *caller's* (parent's) TTBR0 is current. The parent's
L2_ram has slots 64..67 redirected to the *parent's* user
backing. So accessing VA `0x4800_0000` under the parent's
TTBR0 gives you the parent's PA.

Now we want to memcpy `dst` bytes — bytes that physically
live in the *child's* user backing — into a region whose VA
range may overlap the user window. If `dst`'s PA happens to
land somewhere between `0x4800_0000` and `0x4880_0000`,
writing through the kernel's identity-mapped pointer to it
would *under the parent's TTBR0* write to the parent's user
backing instead. The kernel's PA-equals-VA assumption
silently breaks.

The kernel's TTBR0, in contrast, has the user window pointing
at the kernel-only `0x4800_0000`..`0x4880_0000` PA range
(non-aliased). Switching to the kernel root for the duration
of the memcpy makes the kernel's identity-map assumption true
again. The save-and-restore around the memcpy keeps the
caller's TTBR0 intact on return, so the syscall finishes in
the same address space it started in.

The cache-coherence sequence at the end:

```c
asm volatile ("dsb ish"  ::: "memory");
asm volatile ("ic iallu" ::: "memory");
asm volatile ("dsb ish"  ::: "memory");
asm volatile ("isb");
```

…makes freshly-copied bytes safe to execute. The `memcpy`
wrote them into the data cache; the I-cache hasn't seen the
update. `ic iallu` invalidates the I-cache so the *next*
instruction fetch from those bytes (when the child runs its
copy of the user code) sees the just-written content. Without
this, the child could try to execute stale instructions — a
bug that wouldn't show up in a quick test but would surface
the moment the child's behaviour diverged.

> **Side note: belt and suspenders with the PMM reservation.**
> The PMM reserves the user-window PA range up front (we'll
> see why below in §"A boot-time consequence: PMM
> reservation"), so `dst`'s 8 MiB chunk can never *overlap*
> the user window — strictly speaking, the aliasing bug above
> is already prevented at the source. The TTBR0 switch in
> `mmu_copy_user_backing` is defense-in-depth: it keeps the
> fork-time copy correct even if a future change moved the
> user backing out from under the reservation, and it makes
> the safety property local to one function instead of a
> distant invariant maintained by `boot.c`.

> **Side note: this is exactly what COW fork removes.** In a
> copy-on-write fork, the child's L1 starts out pointing at
> the *same* PAs as the parent's, with every page marked
> read-only. The first write in either process faults; the
> kernel allocates one fresh page, copies just that page,
> updates the faulting process's L3 entry to point at it
> with read-write permission, and resumes. The 8 MiB
> memcpy disappears, replaced by zero memcpy in the common
> case. The Phase 9 rework will land COW alongside L3
> pages.

---

## A boot-time consequence: PMM reservation

Chapter 5 ended with one PMM call we glossed over:

```c
pmm_reserve_range((uintptr_t)mmu_user_window_base(),
                  (size_t)mmu_user_window_size());
```

We promised the full reason would make sense after the MMU
chapter. Here it is.

The PMM hands out pages anywhere in `[__free_mem_start,
RAM_END)`. That includes PAs in the range
`0x4800_0000`..`0x4880_0000` — the user-window's PA range.
If the PMM happens to give a kernel data structure (a kstack,
a page table, a slab page) a PA somewhere in that range,
trouble starts the moment a process is *running*.

Why? Because while a process is running, its TTBR0 is
current. That TTBR0's L2_ram has slots 64..67 overridden to
point at the *process's* user backing. So a kernel
pointer whose PA is, say, `0x4810_0000` (somewhere in the
window's VA range), under the kernel's identity-map
expectation, would translate via the *process's* user-window
override to the process's user_pa instead of the kernel's
intended PA.

The bug is intermittent: depending on which PAs the PMM
hands out, the kernel might or might not write something
sensitive into "whichever process happens to be current."
Stack corruption, memory-mapped IPC garbage, scheduler state
bit-rot — all of it possible.

Reserving the user window's PA range up front, before any
PMM client allocates anything, makes the bug impossible. The
reservation is idempotent (re-reserving an already-reserved
page is a no-op via `try_claim`) and pulls maybe a few
hundred frames out of the pool — a small price for never
having to think about the aliasing case again.

This too is a place a Phase 9 rework changes the picture:
once user-window mappings move to L3 4 KiB pages with
demand paging, the "every process reserves 8 MiB up front"
model is gone, and the reservation becomes a much smaller
sliding affair (or vanishes entirely if the user window
moves to a high-half VA range that doesn't overlap the
identity map).

---

## How it all fits together at runtime

Pulling the threads together:

```
   physical RAM                   kernel-VA identity map
   ┌─────────────────┐            (TTBR0_EL1 = l1_table)
   │  0x40000000     │            ┌────────────────────┐
   │  RAM_BASE       │            │ L1[0] → l2_mmio    │
   │                 │ ◄──────────┤ L1[1] → l2_ram     │
   │   kernel image  │            └────────────────────┘
   │                 │
   │   PMM pool      │
   │                 │            per-process address space
   │   ┌── user      │            (TTBR0_EL1 = process L1)
   │   │   window    │            ┌────────────────────┐
   │   │   PA range  │ ◄┐         │ L1[0] → l2_mmio    │ (shared)
   │   │   reserved  │  │         │ L1[1] → L2_priv    │
   │   └── (unused)  │  │         └────────────────────┘
   │                 │  │                    │
   │                 │  │         L2_priv: slots 0..63, 68..511
   │   per-process   │  └─────────────────── = kernel mapping
   │   user_backing  │ ◄─────────────────── slots 64..67
   │   (8 MiB chunk) │                       = user_block
   │                 │                         pointing here
   │  0x80000000     │
   └─────────────────┘
```

- The kernel's TTBR0 has the kernel mappings *and* one
  EL0-accessible 2 MiB block at the user-window VA. That's
  enough for "kthread copies bytes in, then drops to EL0",
  which is how ktests run an EL0 program before the first
  process is created.
- A real process gets its own L1 with its own L2_ram. The
  kernel mappings are byte-identical to the kernel's own
  L2_ram. The four user-window slots are overridden.
- `mmu_switch_address_space` flips TTBR0 between roots and
  flushes the TLB. The kernel's instructions stay reachable
  across the switch because every root maps the kernel's
  pages identically.
- `mmu_copy_user_backing` (used by fork) temporarily
  switches to the kernel's root so that VAs in the user
  window resolve to kernel-controlled PAs, does the byte
  copy, and switches back.
- The PMM has the user-window PA range reserved so kernel
  data never lands at a PA the process's TTBR0 will alias.

Every syscall, every context switch, and every fork involves
some piece of this picture.

---

## A few extra things to know

- **No high-half kernel.** ARM splits the address space into
  TTBR0 (low half) and TTBR1 (high half) and the typical
  Linux convention is "kernel in the high half, user in
  the low half." nonux currently disables TTBR1 entirely
  (`TCR.EPD1 = 1`) and keeps both kernel and user in
  TTBR0. The advantage is simplicity — no high-half symbol
  rebasing, no separate translation regimes for user code
  vs kernel code. The disadvantage is the user window has
  to live in the kernel's address space too, hence the
  PMM reservation. A future rework will move the kernel to
  the high half and free the entire low half for user
  programs.

- **No ASIDs yet.** The TLB is flushed wholesale on every
  address-space switch (`tlbi vmalle1`). ARMv8 supports
  Address Space IDentifiers — a small per-process tag
  attached to each TLB entry — that let switches skip the
  flush, but the gain only shows up at high context-switch
  rates. nonux's switch rate is dominated by the timer at
  10 Hz and the occasional fork; the brute-force flush
  hasn't shown up as a bottleneck yet. Phase 10
  benchmarks will quantify the gap.

- **No demand paging today.** Every page in the user
  window is mapped at process-create time, with the full
  8 MiB of physical backing allocated up front. The bigger
  the binary, the bigger the waste; but we're running
  busybox, which fits comfortably. The Phase 9 rework
  introduces demand paging (allocate on first access) and
  copy-on-write fork (share PAs across forked processes
  until written). Both are more work than they sound;
  drafting them as a coherent slice was deliberately
  postponed past the "make busybox run" milestone.

- **Page tables are owned by the kernel heap.** The L1
  and L2 pages a process needs are allocated via
  `kheap`'s `malloc`, which falls through to the PMM for
  whole-page requests. The PMM reservation we just talked
  about does *not* exclude the page-table pages
  themselves from the user-window PA range — a process's
  L1 *could* legally land at a PA in `0x4800_0000`..
  `0x4880_0000` were it not for the reservation. This is
  exactly the bug the reservation prevents.

- **One MMU per core.** All the bring-up here happens
  per-core. nonux is single-core today, so we run the
  bring-up exactly once in `mmu_init`. A future SMP
  build needs to call equivalent code on every secondary
  core as it comes up, with the *same* `mair_el1` /
  `tcr_el1` / `ttbr0_el1` values so all cores translate
  consistently.

- **The MMU also handles unaligned access.** With the MMU
  off and Device-typed memory, unaligned accesses fault
  immediately. With the MMU on and Normal-typed memory,
  the architecture allows unaligned accesses (subject to
  `SCTLR.A`, which we leave at 0 — "permit"). That's why
  the kernel can use `memcpy` happily after `mmu_init`
  but couldn't before it.

- **We never read the descriptor flags back.** Once
  `mmu_init` writes the L1, L2, and L2_ram tables, no
  other kernel code reads from them. The MMU does, but
  the C code's interaction with the tables is
  write-only. That's why the tables are plain `uint64_t`
  arrays — no atomics, no `_Atomic`. A real demand-paging
  kernel would update L3 entries from the page-fault
  handler under a lock, with the appropriate barriers
  before issuing the next access; we'll need that
  machinery in Phase 9.

- **The MMU is also why `kprintf("%s", ptr)` doesn't
  crash.** A bad pointer with the MMU off would produce a
  bus error at best and silent corruption at worst. With
  the MMU on, accessing an unmapped VA produces a clean
  synchronous abort that the kernel can catch and report.
  Chapter 3's `on_sync` handler is what receives that
  abort.

---

## Where to read more

- [`core/mmu/mmu.c`](../core/mmu/mmu.c) — the full MMU
  bring-up plus per-process address-space allocator; about
  500 lines.
- [`core/mmu/mmu.h`](../core/mmu/mmu.h) — the public API and
  the long comment explaining the per-process layout in
  prose.
- [`core/cpu/el0_entry.S`](../core/cpu/el0_entry.S) —
  `drop_to_el0`, the one-way demotion from EL1 to EL0. The
  MMU's permission machinery is what makes this jump
  meaningful.
- [Chapter 1 §"Privilege levels: EL0, EL1, EL2, and EL3"](01-boot-and-linker.md#privilege-levels-el0-el1-el2-and-el3) —
  introduces EL0/EL1 in passing; this chapter is where they
  start to bite.
- [Chapter 5 §"Reserving a range"](05-physical-memory-and-pmm.md#reserving-a-range)
  — the PMM call that excludes the user-window PA range from
  general allocation. The motivation is what we covered
  here.
- ARM Architecture Reference Manual for ARMv8-A (DDI0487),
  chapter "AArch64 memory model" — the canonical reference
  for descriptor formats, MAIR, TCR, SCTLR, the translation
  process, TLB management, and barrier semantics. Long, but
  the only authoritative source.
- ARM ARM appendix "VMSAv8-64 translation table format" —
  the full bit-by-bit layout of every kind of descriptor at
  every level. The pictures in this chapter are simplified;
  the ARM ARM has the unabridged version.
- Linux's [`arch/arm64/mm/proc.S`](https://elixir.bootlin.com/linux/latest/source/arch/arm64/mm/proc.S)
  and [`arch/arm64/include/asm/pgtable.h`](https://elixir.bootlin.com/linux/latest/source/arch/arm64/include/asm/pgtable.h)
  — production-grade versions of the same ideas. Read after
  this chapter to see what 4-level translation, ASIDs, KPTI,
  and demand paging look like in a kernel that takes them
  seriously.
- [QEMU `hw/arm/virt.c`](https://gitlab.com/qemu-project/qemu)
  — defines the device-vs-RAM split (`virt_memmap[]`) that
  we hardcoded as "0..1G is MMIO, 1G..2G is RAM" in
  `mmu_init`. The constants in our `mmu.c` come from this
  file.
