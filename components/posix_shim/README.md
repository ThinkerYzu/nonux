# posix_shim

Kernel-side syscall-entry boundary component (slice 8.0a.4 — skeleton).

## Role

DESIGN.md §"Every Component Occupies a Slot" mandates a graph-resident
component at the userspace/kernel boundary so that syscall-driven
cross-component calls have a well-defined `src_slot`. `posix_shim` is
that component.

Slice 8.0a routes every cross-component call through
`nx_slot_call_blocking` (see `framework/slot_call.h`). The per-task
`caller_slot` (slice 8.0a.5) is the actual sender; *every* task slot
binds to this single `posix_shim` component instance via the registry's
N→1 binding (`concurrency: shared`). `posix_shim_handle_msg` is the
receiver of reply messages dispatched back from service slots.

## Dependencies

| dep                  | C field name         | mode   | stateful | policy |
|----------------------|----------------------|--------|----------|--------|
| `vfs`                | `vfs`                | async  | true     | queue  |
| `scheduler`          | `scheduler`          | async  | false    | queue  |
| `memory.page_alloc`  | `memory_page_alloc`  | async  | false    | queue  |
| `char_device.serial` | `char_device_serial` | async  | false    | queue  |

All four edges are `mode: async` — sync mode requires the caller to run
on a dispatcher thread (DESIGN.md §"Sync-mode caller must be on a
dispatcher"), and syscall callers run on their own task kstacks.

## Sub-slice plan (slice 8.0a)

| Sub-slice | What lands here |
|---|---|
| 8.0a.4 (this) | Component skeleton — manifest, init/enable/disable/destroy stubs, `handle_msg` returning `NX_EINVAL`. |
| 8.0a.5 | Per-task `caller_slot` lifecycle — `nx_task_create` registers slot, binds `active = posix_shim_component`, clones outgoing edges; `_destroy` reverses. |
| 8.0a.6 | `posix_shim_handle_msg` decodes reply payload into originating task's reply buffer + wakes `reply_waitq`. |
| 8.0a.7 | `posix_shim_on_dep_swapped` STATE_LOST handler invalidates the kernel handle table. |

See [SLOT-CALL-API.md §"The posix_shim Component"](../../../proj_docs/nonux/SLOT-CALL-API.md).
