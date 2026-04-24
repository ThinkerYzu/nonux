# ramfs

In-memory filesystem driver — the first real component bound to the
`filesystem.root` slot, introduced in slice 6.2.  Implements the
universal `nx_fs_ops` contract from `interfaces/fs.h`.

## Interface

- **iface:** `filesystem`
- **Bound by default:** `kernel.json["components"]["filesystem.root"]`.
- **Dependencies:** none.
- **Worker threads:** none (`spawns_threads: false`).
- **Pause hook:** not required (`pause_hook: false`).

## Behaviour

- **Fixed-capacity inode table.**  Each instance owns a static array of
  `RAMFS_MAX_FILES = 8` file slots, each with `RAMFS_FILE_CAP = 256`
  bytes of inline storage and a `RAMFS_NAME_MAX = 32` byte path (a
  single flat namespace — no directories yet; they land in slice 6.4).
  Everything lives inside the per-instance state struct, so teardown
  is implicit: when the framework frees the state, the whole
  filesystem goes with it.
- **Per-open cursor.**  `open(path, flags)` allocates a per-open slot
  out of a pool of `RAMFS_MAX_OPEN = 32` records.  Each open carries
  its own cursor; two opens on the same path have two cursors (the
  universal `two_opens_have_independent_cursors` conformance case).
- **Create semantics.**  Missing path + `NX_FS_OPEN_CREATE` → fresh
  zero-length file.  Missing path without CREATE → `NX_ENOENT`.
  Existing path + CREATE is not an error (matches POSIX `O_CREAT`);
  truncate semantics land with `NX_FS_OPEN_TRUNCATE` in slice 6.4.
- **Rights enforcement.**  `read` on a handle opened without
  `NX_FS_OPEN_READ` returns `NX_EPERM`; ditto for `write` without
  `NX_FS_OPEN_WRITE`.
- **Backing-store exhaustion.**  Write past the per-file `FILE_CAP`
  returns `NX_ENOMEM`; the partial write is applied and the cursor
  advanced (bytes written count is returned).

## State

```c
struct ramfs_state {
    struct ramfs_file files[RAMFS_MAX_FILES];
    struct ramfs_open opens[RAMFS_MAX_OPEN];
    /* + lifecycle counters observable by tests */
};
```

No heap allocation, no external dep on `memory.page_alloc`.  Storage
is inline in the state struct; an instance weighs ~3 KiB and the
framework allocates it via `calloc` (host) / `kheap` (kernel).

## Symbols exported

- `extern const struct nx_fs_ops ramfs_fs_ops;` — the
  `interfaces/fs.h` surface that vfs_simple consumes through the
  `filesystem.root` slot's `iface_ops`.
- `extern const struct nx_component_ops ramfs_component_ops;` —
  framework lifecycle (`init` / `enable` / `disable` / `destroy`).
  `handle_msg` is NULL: ramfs is driven via iface_ops, not IPC.
- `extern const struct nx_component_descriptor ramfs_descriptor;`
  (auto-emitted by `NX_COMPONENT_REGISTER_NO_DEPS_IFACE`).

## Why this component exists

Slice 6.1 defined the fs-driver interface + universal conformance
harness; slice 6.2 lands the first driver that implements it.
Keeping the v1 implementation static (no `memory.page_alloc` dep)
puts the focus on the VFS layer wiring rather than on runtime
allocation — a follow-up can swap the static tables for a
page-backed layout without touching the interface.

```
interfaces/fs.h  ─────────────┐
(slice 6.1 defines ops contract) │
                                 ▼
conformance_fs.{h,c}  ───────────┐
(slice 6.1 universal cases)       │
                                  ▼
components/ramfs/  ────────────── passes every case
(this slice)                      plus lifecycle cycling
                                  │
                                  ▼
                    kernel.json binds "filesystem.root" → ramfs
                    nx_framework_bootstrap brings it up in NX_LC_ACTIVE
                    vfs_simple resolves the slot at call time
```

Other plausible `filesystem` implementations (tmpfs, ext2, future
on-disk formats via a `block_device` dep) slot in by editing
`kernel.json`; the conformance suite ensures they meet the same
contract.
