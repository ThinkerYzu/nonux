# nonux framework documentation

Reference docs for the framework modules under `framework/`. Each doc
is self-contained — types, functions, return codes, invariants,
example usage — and is kept in sync with the real headers. Headers
are the normative source; docs track them.

## Modules

| Doc                                      | Header             | Role                                                                    |
|------------------------------------------|--------------------|-------------------------------------------------------------------------|
| [Registry](framework-registry.md)        | `framework/registry.h`  | Slots, components, connections; events, change log, snapshots, JSON. The source of truth for the running composition. |
| [Component lifecycle](framework-components.md) | `framework/component.h` | Six-verb lifecycle state machine, `struct nx_component_ops`, dependency resolver, `NX_COMPONENT_REGISTER` macro. |
| [IPC router](framework-ipc.md)           | `framework/ipc.h`       | `nx_ipc_send` / dispatch, per-slot inbox, per-edge hold queue, pause-policy routing, capabilities, `slot_ref_retain/release`. |
| [Hook framework](framework-hooks.md)     | `framework/hook.h`      | Per-hook-point chains, priority-sorted insert, typed context union, mark-then-sweep unregister. |

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
composition, not compile-time. The kernel side will route these
through `kmalloc` / `kfree` in slice 3.9.

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

Slice 3.6–3.8 runs on a single-threaded host build. The kernel boot
path (slice 3.9) upgrades the inbox to an MPSC lock-free queue with
a per-CPU dispatcher thread. The public APIs in the docs below don't
change across that upgrade — the synchronisation primitives inside
`registry.c` / `ipc.c` do.

`pause_state` on `struct nx_slot` is already `_Atomic` so the SMP
upgrade is a barrier swap, not a restructure.

## See also

- [`../README.md`](../README.md) — project-level overview and quick-start.
- [`../tools/README.md`](../tools/README.md) — build tooling (`gen-config.py`,
  `validate-config.py`, `verify-registry.py`).
