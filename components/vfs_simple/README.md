# vfs_simple

Single-mount VFS layer — the first real component bound to the `vfs`
slot, introduced in slice 6.2.  Implements the universal `nx_vfs_ops`
contract from `interfaces/vfs.h` that slice 6.3's file syscalls
dispatch through.

## Interface

- **iface:** `vfs`
- **Bound by default:** `kernel.json["components"]["vfs"]`.
- **Dependencies:** none in the manifest.  Late-bound to the
  `filesystem.root` slot at call time (see Behaviour below).
- **Worker threads:** none (`spawns_threads: false`).
- **Pause hook:** not required (`pause_hook: false`).

## Behaviour

- **Single mount.**  v1 recognises exactly one mount point, `/`,
  served by whatever component implements the `filesystem.root` slot.
  Multi-mount lands when a second filesystem driver has a reason to
  coexist (Phase 8+).
- **Path handling.**  An absolute path (`/foo`) is passed through to
  the driver unchanged; relative / empty paths return `NX_EINVAL`.
  Slice 6.4's directory support will strip the mount prefix here
  before dispatch.
- **Slot-resolve discipline.**  Every op resolves `filesystem.root`
  fresh at call time via `nx_slot_lookup` — never caches the driver
  pointer.  This matches DESIGN §Slot-Based Indirection: the slot is
  the late-binding primitive, which means future hot-swap of the root
  filesystem lands as a single `nx_slot_swap` with no stale pointers
  inside vfs_simple to invalidate.
- **Flag / error passthrough.**  `NX_VFS_OPEN_*` flags are
  bit-compatible with `NX_FS_OPEN_*` by construction (both headers
  define them as `(1U << 0) .. (1U << 2)`), so no translation happens
  between the two layers.  Driver status codes (NX_OK / NX_ENOENT /
  NX_EPERM / NX_ENOMEM / NX_EINVAL) forward unchanged.

## State

```c
struct vfs_simple_state {
    /* Lifecycle counters for test introspection; no per-instance state
     * otherwise — the layer is stateless by design. */
    unsigned init_called;
    unsigned enable_called;
    unsigned disable_called;
    unsigned destroy_called;
};
```

The layer carries no per-open wrapper in v1: a file handle returned
from `open` is the driver's opaque pointer, round-tripped verbatim
through `close` / `read` / `write`.  When mount points arrive, a
wrapper will remember which mount owns each open.

## Symbols exported

- `extern const struct nx_vfs_ops vfs_simple_vfs_ops;` — the
  `interfaces/vfs.h` surface the syscall layer consumes through the
  `vfs` slot's `iface_ops`.
- `extern const struct nx_component_ops vfs_simple_component_ops;` —
  framework lifecycle (`init` / `enable` / `disable` / `destroy`).
  `handle_msg` is NULL: vfs_simple is driven via iface_ops, not IPC.
- `extern const struct nx_component_descriptor vfs_simple_descriptor;`
  (auto-emitted by `NX_COMPONENT_REGISTER_NO_DEPS_IFACE`).

## Why this component exists

Slice 6.1 landed the fs-driver interface + conformance harness.
Slice 6.2 pairs the first driver (ramfs) with the first VFS layer so
that slice 6.3's file syscalls have a live `vfs` slot to dispatch
into.  Keeping the layer thin (no path-parsing, no mount table) in v1
follows the "walk before running" pattern: real VFS complexity
(mount tables, path canonicalisation, mount-boundary traversal) lands
with the first real consumer in Phase 8+.

```
interfaces/vfs.h  ───────────── nx_vfs_ops contract
                                │
                                ▼
components/vfs_simple/  ──── exports vfs_simple_vfs_ops
(this slice)                    │
                                ▼
                    kernel.json binds "vfs" → vfs_simple
                    nx_framework_bootstrap brings it up in NX_LC_ACTIVE
                                │
                                ▼ (at call time)
                    nx_slot_lookup("filesystem.root") ──► ramfs
                    dispatch through slot->active->descriptor->iface_ops
```
