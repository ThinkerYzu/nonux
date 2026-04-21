# Bootstrap — `framework/bootstrap.{h,c}`

Boot-time composition bring-up. `nx_framework_bootstrap()` is the
single entry point the kernel calls from `boot_main` (after PMM /
GIC / timer / UART are up) to transform the static
`nx_components` linker section and the generated slot table into a
live, running composition.

This module is the bridge between three statically-knowable things
— the descriptor section emitted by `NX_COMPONENT_REGISTER`, the
slot table emitted by `tools/gen-config.py kernel`, and
`core/boot/boot.c` — and the runtime [registry](framework-registry.md)
/ [lifecycle state machine](framework-components.md).

---

## What it does

1. **Register every slot** declared in the generated
   `nx_boot_slots[]` table. Each entry's `struct nx_slot` lands in
   `.data` / `.bss` (in `gen/slot_table.c`) with `name`, `iface`,
   `mutability`, `concurrency` pre-filled. `nx_slot_register`
   initialises the runtime fields (`pause_state`, `fallback`) and
   publishes the slot.
2. **Walk the descriptor section.** Linker-generated
   `__start_nx_components` / `__stop_nx_components` mark the range;
   each element is a `struct nx_component_descriptor` emitted by
   `NX_COMPONENT_REGISTER`. The walker allocates one
   `struct nx_component` per descriptor with `manifest_id =
   descriptor->name`, `instance_id = "0"`, and a fresh
   `calloc(state_size)` for `impl`.
3. **Bind components to slots.** For each descriptor, look up the
   matching entry in `nx_boot_slots[]` by `impl_name ==
   descriptor->name`; if found, `nx_slot_swap` the new component in.
   A descriptor with no matching slot stays registered but unbound —
   fine for components that are selected at runtime or for
   zero-instance compositions.
4. **Topological bring-up.** Repeatedly scan for any un-visited
   descriptor whose required deps are all satisfied (their target
   slots have a visited active impl), then in dep order call:
   - `nx_resolve_deps(descriptor, self_slot, impl)` — writes the
     dep slot pointers into the component's state struct at the
     offsets baked in by `NX_COMPONENT_REGISTER`, and registers
     connection edges from `self_slot` to each target.
   - `nx_component_init(c)` — `UNINIT → READY`, invoking
     `ops->init` if present.
   - `nx_component_enable(c)` — `READY → ACTIVE`, firing
     `NX_HOOK_COMPONENT_ENABLE` and invoking `ops->enable`.
5. **Detect cycles.** If a full scan of unvisited descriptors
   makes no progress, every remaining one has an unsatisfied
   required dep — either a cycle in the graph, or a dep naming a
   slot with no bound component. Bootstrap returns `NX_ELOOP`.

## API

```c
struct nx_boot_slot {
    struct nx_slot *slot;         /* static storage from gen/slot_table.c */
    const char     *impl_name;    /* matches descriptor->name */
};

extern struct nx_boot_slot nx_boot_slots[];
extern const unsigned      nx_boot_slots_count;

int nx_framework_bootstrap(void);
```

**Returns:**

| Code          | Meaning                                                         |
|---------------|-----------------------------------------------------------------|
| `NX_OK`       | Every component reached `ACTIVE`.                               |
| `NX_ENOMEM`   | `calloc` for a component record, state struct, or visited array failed. |
| `NX_EEXIST`   | Duplicate slot name across the boot table (caller's bug).       |
| `NX_ELOOP`    | Cycle or missing required dep — no forward progress possible.   |
| other `NX_E*` | Whatever `nx_slot_swap` / `nx_resolve_deps` / `nx_component_init` / `nx_component_enable` returned first. |

On non-OK return, the composition is partially up. Slice 3.9a
treats any failure as fatal — `core/boot/boot.c` logs the error
and falls through to the idle loop. Slice 3.9b will add a real
rollback path once dispatcher kthreads exist.

## How the section is populated

The linker script (`core/boot/linker.ld`) puts `nx_components` into
`.rodata`:

```ld
.rodata : ALIGN(4096) {
    *(.rodata .rodata.*)
    ...
    . = ALIGN(8);
    __start_nx_components = .;
    KEEP(*(nx_components))
    __stop_nx_components = .;
} > RAM
```

`NX_COMPONENT_REGISTER(NAME, CONTAINER, DEPS_FIELD, OPS, DEPS_TABLE)`
(in `framework/component.h`) emits one `const struct
nx_component_descriptor NAME##_descriptor` with
`__attribute__((section("nx_components"), used))`. `KEEP(*(...))`
prevents `--gc-sections` from dropping the descriptor when nothing
references it by symbol.

## How the slot table is populated

`tools/gen-config.py kernel` reads `kernel.json` and emits
`gen/slot_table.c`:

```c
static struct nx_slot nx_slot_char_device_serial = {
    .name        = "char_device.serial",
    .iface       = "char_device",
    .mutability  = NX_MUT_HOT,
    .concurrency = NX_CONC_SHARED,
};

struct nx_boot_slot nx_boot_slots[] = {
    { .slot = &nx_slot_char_device_serial, .impl_name = "uart_pl011" },
};
const unsigned nx_boot_slots_count =
    sizeof nx_boot_slots / sizeof nx_boot_slots[0];
```

The slot identifier is derived from the JSON slot name with `.` →
`_`. The `iface` field takes the part of the slot name before the
first `.` (so `char_device.serial` binds to iface `char_device`);
plain names become their own iface.

A weak fallback in `framework/bootstrap.c` provides an empty
`nx_boot_slots[]` / `nx_boot_slots_count = 0` for unit tests that
don't generate a slot table — the bootstrap function then degenerates
to "walk the section, register components, leave them all unbound."

## Memory model

Bootstrap allocations go through the kernel heap (`core/lib/kheap.c`,
`malloc` / `calloc` / `free`). For slice 3.9a the heap is a simple
PMM-backed slab allocator — small allocations (≤ 256 B) bump out of
a page, large allocations take contiguous PMM runs. `free` returns
pages to the PMM for large allocations and is a no-op for slab
chunks (boot-time composition doesn't churn the heap).

Host builds link against libc's `malloc` / `calloc` / `free`
directly. The framework's `#if __STDC_HOSTED__` guards pick the
right path automatically.

## Boot sequence

```
start.S (EL1 entry, stack, BSS zero)
   ↓
boot_main (core/boot/boot.c)
   ↓  uart_init → pmm_init → vectors_install → gic_init → timer_init
   ↓
nx_framework_bootstrap()          ← this module
   ↓  register slots (from gen/slot_table.c)
   ↓  walk nx_components section, register components, bind to slots
   ↓  topo-sort, init → enable each
   ↓
nx_graph_snapshot_to_json()       ← logged to UART
   ↓
irq_enable_local();  wfi          ← or ktest_main under -DNX_KTEST
```

## Invariants

1. **Slot table is authoritative.** Components whose `descriptor->name`
   isn't referenced by any `nx_boot_slots[].impl_name` stay unbound —
   the boot table drives composition, not the descriptor list.
2. **`"0"` is the default `instance_id`.** Multi-instance composition
   needs explicit support in `kernel.json` / gen-config (Phase 6+).
3. **Init / enable run in topo order.** A component's `ops->init` and
   `ops->enable` can safely read `struct <name>_deps` fields — all
   required deps have been resolved and brought to `ACTIVE` by the
   time your ops fire.
4. **Bootstrap is a one-shot.** Calling it twice is undefined — the
   second call would see slots already registered (`NX_EEXIST`) and
   components already allocated. Slice 3.9b's hot-swap story goes
   through recomposition, not re-bootstrap.
5. **Partial failure is fatal.** Components already brought up stay
   up; the caller is expected to panic / log / halt. Rollback lands
   with 3.9b.

## Testing

`test/kernel/ktest_bootstrap.c` asserts after `boot_main`:

- The expected slot is registered (`nx_slot_lookup("char_device.serial")`).
- The component is bound (`slot->active != NULL`, state
  `NX_LC_ACTIVE`).
- `ops->init` and `ops->enable` actually fired (observable via the
  `struct uart_pl011_state` counters).
- `nx_graph_snapshot_to_json` emits a non-empty JSON body containing
  the slot name and the bound manifest.

## See also

- [Registry](framework-registry.md) — `nx_slot_register`,
  `nx_slot_swap`, `nx_component_register`, and the change-log plumbing
  bootstrap drives.
- [Component lifecycle](framework-components.md) — lifecycle state
  machine + `nx_resolve_deps` + `NX_COMPONENT_REGISTER`.
- [`../components/uart_pl011/README.md`](../components/uart_pl011/README.md)
  — the first real component and the bootstrap's end-to-end proof.
- [`../tools/README.md`](../tools/README.md) — `gen-config.py`
  emits the slot table + sources list.
