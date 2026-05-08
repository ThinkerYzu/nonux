# framework/

The component framework: the glue between the kernel core and the swappable
components. Every subsystem (scheduler, memory manager, VFS, drivers) plugs
into the framework via a slot; the framework manages their lifecycle, IPC,
hooks, and the runtime component graph.

Reference documentation lives in [`docs/`](../docs/):
[Registry](../docs/framework-registry.md) |
[Component lifecycle](../docs/framework-components.md) |
[IPC router](../docs/framework-ipc.md) |
[Hook framework](../docs/framework-hooks.md) |
[Bootstrap](../docs/framework-bootstrap.md).

## Files

### Composition core

| File | Role |
|------|------|
| `bootstrap.c/.h` | `nx_framework_bootstrap()`. Called once at boot from `core/boot/boot.c`. Walks the `nx_components` linker section, registers all slots and components, resolves dependencies, then runs `init`→`enable` in topological order. Dumps the composition on success. |
| `registry.c/.h` | Component graph registry. Tracks every slot, component, and connection. The single source of truth for "what is the kernel built from right now?" Emits events, maintains an append-only change log, and exposes JSON snapshots. All hot-swap and recomposition operations go through the registry. |
| `component.c/.h` | Six-verb lifecycle state machine (`INIT → READY → ACTIVE ↔ PAUSED → DISABLED → DESTROYED`). The `NX_COMPONENT_REGISTER` macro embeds a component descriptor in the `nx_components` linker section. `nx_resolve_deps()` wires each component's `requires`/`optional` slots at boot. |
| `recompose.c/.h` | Runtime recomposition orchestrator (`nx_recompose()`). Builds a topological pause order from registry edges (Kahn's algorithm), pauses and drains affected components, rewires connections, resumes. Rolls back on any pause failure. Used by `config.c` for live component swaps. |
| `config.c/.h` | Runtime configuration manager. `nx_config_open()`, `nx_config_query_snapshot()`, `nx_config_swap_component()`. Backed by three EL0 syscalls (`NX_SYS_CONFIG_OPEN`, `NX_SYS_CONFIG_QUERY`, `NX_SYS_CONFIG_SWAP`, `NX_SYS_CONFIG_REWIRE`). Userspace drives live scheduler swaps and async↔sync connection-mode switches through this interface. |

### IPC

| File | Role |
|------|------|
| `ipc.c/.h` | Async IPC router. `nx_ipc_send()` enqueues a message into the destination slot's inbox; `nx_ipc_dispatch()` drains one message and calls the component's `handle_msg` op. Per-edge hold queues buffer messages while a slot is paused. Routes capabilities (`slot_ref`) with borrow/transfer ownership tracking. |
| `dispatcher.c/.h` | Async IPC dispatcher kthread. Drains the MPSC inbox (`core/lib/mpsc.h`) on a dedicated kthread. `nx_dispatcher_enqueue()` is safe from any context (IRQ, kthread, EL0 exception); the dispatcher kthread is the only `nx_ipc_dispatch()` caller. Implements pause-drain: waits until `in_flight_calls == 0` before allowing a slot swap. |
| `slot_call.c/.h` | `nx_slot_call_blocking()` — synchronous cross-slot call primitive. Sends a message with `NX_MSG_FLAG_REPLY_REQUESTED`; blocks the caller on `task->reply_waitq` until the callee writes back into `task->in_flight_reply_buf`. Used by all generated `*_call.h` wrappers. |
| `channel.c/.h` | In-kernel IPC channel (bidirectional, blocking). Provides a higher-level pipe-like primitive layered on the wait-queue and used by the pipe syscall implementation. |

### Interface call and dispatch shims (IDL-generated)

Each interface has four generated artefacts; the `_call.h` and `_dispatch.h`
files live here because they are kernel-framework code, not interface
definitions.

| File | Interface | Role |
|------|-----------|------|
| `char_device_call.c/.h` | `char_device` | `nx_char_device_read()` / `nx_char_device_write()` blocking wrappers. Used by `syscall.c` for `sys_read`/`sys_write` on `NX_HANDLE_RESOURCE` console FDs. |
| `char_device_dispatch.h` | `char_device` | Receiver-side `handle_msg` dispatch template included by `uart_pl011`. |
| `char_device_isr.h` | `char_device` | ISR entry-point template for IRQ-driven char devices. |
| `fs_call.c/.h` | `fs` | `nx_fs_*` blocking wrappers. Used by `vfs_simple` when calling down into `ramfs` / `procfs`. |
| `fs_dispatch.h` | `fs` | Receiver-side dispatch template included by `ramfs` and `procfs`. |
| `mm_call.h` | `mm` | Blocking wrappers for the physical-page-allocator interface. |
| `mm_dispatch.h` | `mm` | Receiver-side dispatch template included by `mm_buddy`. |
| `scheduler_call.h` | `scheduler` | Blocking wrappers for the scheduler interface (`next_task`, `set_priority`, etc.). |
| `scheduler_dispatch.h` | `scheduler` | Receiver-side dispatch template included by `sched_rr` and `sched_priority`. |
| `vfs_call.c/.h` | `vfs` | `nx_vfs_*` blocking wrappers. Used by `syscall.c` for `sys_open`, `sys_read`, `sys_write`, etc. |
| `vfs_dispatch.h` | `vfs` | Receiver-side dispatch template included by `vfs_simple`. |

### OS services

| File | Role |
|------|------|
| `syscall.c/.h` | EL0 syscall dispatch table. The exception handler calls `nx_syscall_dispatch(nr, args)` which fans out to the per-syscall handler. All `NX_SYS_*` numbers are defined in `syscall.h`. Handlers call into VFS, process, and config via the typed slot-call wrappers. |
| `process.c/.h` | Process data structures and lifecycle. `struct nx_process` holds the handle table, VMA list (Phase 9), working directory, PID, and child/wait lists. `nx_process_create()`, `nx_process_fork()`, `nx_process_exec()`, `nx_process_destroy()`. |
| `handle.c/.h` | Per-process handle table. Typed handles with per-handle permission bits. `NX_HANDLE_RESOURCE` handles carry a `{ rights, id, target_slot* }` entry; the component that owns the object decodes `id` from its own object table. No global type-switch. |
| `hook.c/.h` | Hook framework. Components register callbacks on named hook points (`NX_HOOK_SYSCALL_ENTER`, `_EXIT`, etc.); hooks fire in priority order. `nx_hook_context` exposes typed fields for each hook point. ENTER hooks can abort the syscall body; EXIT hooks can override the return value. |
| `elf.c/.h` | Minimal ELF64 loader. Reads PT_LOAD segments from an in-memory blob, maps them into the user address space, and returns the entry point. Used by `sys_exec` to load EL0 programs. |
| `console.c/.h` | Kernel console output. Routes `console_putchar()` / `console_write()` through the `char_device.serial` slot. Provides the `nx_console_write()` function used by `uart_pl011` for kernel-path writes. |

### EL0-facing support

| File | Role |
|------|------|
| `pollset.h` | `struct nx_pollset` and related types for `NX_SYS_PPOLL`. Shared between `syscall.c` and `channel.c`. |
