# interfaces/

Typed interface definitions for every cross-component slot. Each interface
defines the operation set that a slot implementation must provide and the
message structs used to carry arguments and return values.

## Generated vs. Hand-authored

All headers in this directory (except `fs_types.h`) are **IDL-generated** —
they are the deterministic output of `tools/gen-iface.py` reading the JSON
files in `idl/`. Do not edit them by hand; `make verify-iface-fresh` diffs
the in-tree headers against a fresh regeneration and blocks the build on any
drift.

`fs_types.h` is hand-authored shared type definitions (`struct nx_stat`,
`struct nx_dirent64`) included by both the `fs` and `vfs` interface headers.

## Files

### Interface headers (`interfaces/<iface>.h`)

Each `<iface>.h` declares:
- `struct nx_<iface>_ops` — the vtable a component must fill in.
- Constants and forward declarations needed by callers.

| Header | Slot | Description |
|--------|------|-------------|
| `char_device.h` | `char_device.*` | Read/write/ioctl for a character device (UART, console). |
| `fs.h` | `filesystem.*` | File operations: open, read, write, seek, readdir, mkdir, fstat, close. |
| `mm.h` | `memory.*` | Physical page allocation: alloc/free by order. |
| `scheduler.h` | `scheduler` | Task scheduling: next_task, enqueue, dequeue, set_priority, runqueue_size. |
| `vfs.h` | `vfs` | Virtual filesystem routing layer: same operations as `fs`, plus path resolution. |

### Message structs (`interfaces/<iface>_msg.h`)

Each `<iface>_msg.h` declares:
- `enum nx_<iface>_op_id` — numeric operation identifiers carried in IPC messages.
- Per-operation request and reply message structs (`struct nx_<iface>_<op>_req`, `..._reply`).

These structs are the wire format for async IPC messages and the in-band
buffers for synchronous `nx_slot_call_blocking()` calls.

### Shared types

| Header | Role |
|--------|------|
| `fs_types.h` | Hand-authored. Defines `struct nx_stat` (file metadata) and `struct nx_dirent64` (directory entry), shared by `fs.h`, `vfs.h`, and userspace via `lib/libnxlibc/posix.h`. |

## `idl/`

JSON source files from which all interface headers are generated.

| File | Interface |
|------|-----------|
| `char_device.json` | `char_device` interface IDL |
| `fs.json` | `fs` interface IDL |
| `mm.json` | `mm` interface IDL |
| `scheduler.json` | `scheduler` interface IDL |
| `vfs.json` | `vfs` interface IDL |

Each IDL file specifies operations, parameter names and types, and
whether each parameter is in/out/inout. The type system is a closed set of
12 types (`u8`–`u64`, `i8`–`i64`, `usize`, `bool`, `string_in`,
`bytes_in/out`, `struct_in/out/inout`, `slot_ref`, `opaque_self_handle`).
See `tools/idl-meta-schema.json` for the JSON schema and
`tools/README.md` for generator details.
