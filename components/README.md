# components/

Pluggable kernel components — the swappable implementations that fill the
slots declared in `kernel.json`. Every component directory contains:

- **`manifest.json`** — declares the component's name, version, required and
  optional slot dependencies, and per-binding config keys. Machine-read by
  `tools/gen-config.py` (emits `gen/<name>_deps.h`) and
  `tools/validate-config.py` (checks schema + version constraints + cycles).
- **`<name>.c`** — the implementation. Exports one
  `NX_COMPONENT_REGISTER(...)` descriptor that lands in the `nx_components`
  linker section and is walked by `framework/bootstrap.c` at boot.
- **`README.md`** (most components) — interface contract, design notes, and
  known limitations.

Slot interfaces are defined in `interfaces/`; generated call/dispatch shims
live in `framework/`.

## Components

### `mm_buddy/`

**Slot:** `memory.page_alloc`

Buddy-system physical page allocator. Implements the `mm` interface
(`interfaces/mm.h`). Provides `alloc_pages(order)` / `free_pages(ptr, order)`
with O(log n) coalescing. Replaces the flat bitmap in `core/pmm/` for
userspace allocation; `core/pmm` is still used for very-early boot allocations
before the component graph is up.

### `posix_shim/`

**Slot:** `posix_shim`

Kernel-side POSIX boundary component. Receives EL0 syscall results routed by
`framework/syscall.c` and translates them into typed slot calls to `vfs`,
`scheduler`, and other components. Owns the per-task `caller_slot` and
`reply_waitq` that make `nx_slot_call_blocking()` work from EL0 context.
Implements `on_dep_swapped` / `STATE_LOST` so handle tables are invalidated
cleanly when a dependency is hot-swapped.

### `procfs/`

**Slot:** `filesystem.proc`

In-memory process filesystem. Implements the `fs` interface
(`interfaces/fs.h`). Synthesises `/proc/<pid>/` entries from the live process
table; `readdir` on `/proc` iterates `nx_process` list. Used by busybox `ps`.
No `README.md` — interface is fully described by `interfaces/fs.h` and
`interfaces/idl/fs.json`.

### `ramfs/`

**Slot:** `filesystem.root`

In-memory read-write filesystem. Implements the `fs` interface. Stores files
and directories as kernel-heap nodes; supports `open`, `read`, `write`,
`seek`, `mkdir`, `readdir` (including `.` and `..` entries), and `fstat`.
Root filesystem for all busybox tests; `initramfs.cpio` is unpacked into it
at boot by `ktest_initramfs`.

### `sched_priority/`

**Slot:** `scheduler`

Eight-bucket priority round-robin scheduler. Priority 0 is the default; 7 is
highest. `set_priority(task, n)` moves a task to a different bucket
immediately. The idle task always runs at priority 0 and yields when any
higher-priority task is runnable. Used as the default scheduler in
`kernel.json`; can be live-swapped with `sched_rr` via the config API.

### `sched_rr/`

**Slot:** `scheduler`

Basic round-robin scheduler (the original, simpler implementation). All tasks
share one FIFO run queue; no priority concept. Useful as a baseline for
benchmarking or as the swap target in `ktest_live_swap`. Listed in
`kernel.json` under `"alternatives": ["sched_rr"]` so it is compiled in
but not active at boot.

### `uart_pl011/`

**Slot:** `char_device.serial`

PL011 UART driver for the QEMU `virt` machine's primary serial port
(`0x09000000`). Implements the `char_device` interface. `write` copies bytes
to the MMIO TX FIFO; `read` is IRQ-driven via the GIC and delivers bytes
through the RX wait-queue. The kernel console (`framework/console.c`) routes
through this component for all UART output.

### `vfs_simple/`

**Slot:** `vfs`

VFS routing layer. Implements the `vfs` interface (`interfaces/vfs.h`).
Dispatches `open`, `read`, `write`, `readdir`, `mkdir`, `fstat`, `close`, and
`seek` to either `filesystem.root` (ramfs) or `filesystem.proc` (procfs)
based on path prefix. Uses `nx_fs_*` sync-dispatcher wrappers
(`framework/fs_call.c`) to call down into the filesystem components.
`resolve_fs()` selects the target slot; absolute path normalisation
(`path_normalize`) is handled in `framework/syscall.c` before the call
reaches vfs_simple.
