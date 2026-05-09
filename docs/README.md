# nonux framework documentation

Reference docs for the framework modules under `framework/`. Each doc
is self-contained — types, functions, return codes, invariants,
example usage — and is kept in sync with the real headers. Headers
are the normative source; docs track them.

## Background

| Doc                                      | Topic                                                                                  |
|------------------------------------------|----------------------------------------------------------------------------------------|
| [Boot and linker](boot-and-linker.md)    | What a bootloader is, how QEMU's `-kernel` mode plays that role, the ARM64 entry contract, what `core/boot/linker.ld` controls, the timeline from `0x40080000` to `boot_main()`. |

## Modules

| Doc                                      | Header                  | Role                                                                    |
|------------------------------------------|-------------------------|-------------------------------------------------------------------------|
| [Registry](framework-registry.md)        | `framework/registry.h`  | Slots, components, connections; events, change log, snapshots, JSON. The source of truth for the running composition. |
| [Component lifecycle](framework-components.md) | `framework/component.h` | Six-verb lifecycle state machine, `struct nx_component_ops`, dependency resolver, `NX_COMPONENT_REGISTER` macro. |
| [IPC router](framework-ipc.md)           | `framework/ipc.h`       | `nx_ipc_send` / dispatch, per-slot inbox, per-edge hold queue, pause-policy routing, capabilities, `slot_ref_retain/release`. |
| [Hook framework](framework-hooks.md)     | `framework/hook.h`      | Per-hook-point chains, priority-sorted insert, typed context union, mark-then-sweep unregister. |
| [Bootstrap](framework-bootstrap.md)      | `framework/bootstrap.h` | `nx_framework_bootstrap()` — walks the `nx_components` linker section at boot, registers slots + components, runs init / enable in topo order, dumps composition. |

## Error codes

All four modules share the codes defined in `framework/registry.h`:

| Code         | Value | Meaning                                                              |
|--------------|------:|----------------------------------------------------------------------|
| `NX_OK`      |  `0`  | Success.                                                             |
| `NX_EINVAL`  | `-1`  | Bad argument (NULL pointer, out-of-range enum, etc.).                |
| `NX_ENOMEM`  | `-2`  | Internal allocation failed.                                          |
| `NX_EEXIST`  | `-3`  | Duplicate registration.                                              |
| `NX_ENOENT`  | `-4`  | Target not found (slot, component, fallback, etc.).                  |
| `NX_EBUSY`   | `-5`  | Operation would leave dangling references, or destination is paused with `NX_PAUSE_REJECT`. |
| `NX_ESTATE`  | `-6`  | Lifecycle state machine violation.                                   |
| `NX_ELOOP`   | `-7`  | Redirect fallback chain exceeded `NX_IPC_REDIRECT_DEPTH_MAX` (= 4).  |
| `NX_EABORT`  | `-8`  | A hook returned `NX_HOOK_ABORT`.                                     |

## Caller-owned storage

Most framework objects (`struct nx_slot`, `struct nx_component`,
`struct nx_hook`) are **caller-owned**: the framework bookkeeps
pointers to them but never allocates or frees the struct itself. This
keeps the kernel boot path allocator-free — descriptors and slot
structs live in static storage emitted into the `nx_components`
linker section.

Every `_register` call simply records the pointer; every `_unregister`
drops the pointer. The caller is responsible for the struct's lifetime
being at least as long as the registration.

Internal bookkeeping nodes (the per-slot / per-component entries the
registry keeps in linked lists) **are** allocated by the framework via
`malloc`, because the registry's own size is a function of
composition, not compile-time. The kernel build resolves `malloc` /
`calloc` / `free` to `core/lib/kheap.c` (a PMM-backed slab allocator);
the host build links against libc's allocator directly.

## Reset helpers (testing)

Each module exposes a reset function used by the test harness:

| Function               | Clears                                                             |
|------------------------|--------------------------------------------------------------------|
| `nx_graph_reset()`     | Every registry node, subscriber, change log entry, generation counter. |
| `nx_ipc_reset()`       | Every per-slot inbox and every per-`(src,dst)` hold queue.         |
| `nx_hook_reset()`      | Every hook chain and the in-dispatch counter.                      |

These are test-only in kernel builds — real kernels never reset the
framework state. On the host build, tests call them at the top of
every case so state from previous tests is gone.

## Thread model

The framework comes up on the boot thread (`nx_framework_bootstrap`
runs composition bring-up inline). Once Phase 4's scheduler is alive,
`nx_dispatcher_init` spawns a dispatcher kthread that drains the
async-IPC inbox via the Vyukov-style MPSC enqueue point
(`nx_ipc_enqueue_from_irq`) — ISRs and arbitrary kernel threads
enqueue messages there but never dispatch. Synchronous `nx_ipc_send`
shortcuts and `nx_ipc_dispatch` calls still execute on the caller's
thread; the slot-resolve-locality rule (see [framework-ipc.md](framework-ipc.md))
keeps that consistent.

`pause_state` on `struct nx_slot` is `_Atomic` so the future SMP
upgrade is a barrier swap, not a restructure.

## See also

- [`../README.md`](../README.md) — project-level overview and quick-start.
- [`../tools/README.md`](../tools/README.md) — build tooling (`gen-config.py`,
  `gen-iface.py`, `validate-config.py`, `verify-registry.py`).
