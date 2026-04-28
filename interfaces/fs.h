#ifndef NONUX_INTERFACE_FS_H
#define NONUX_INTERFACE_FS_H

#include <stddef.h>
#include <stdint.h>

/*
 * Filesystem-driver interface — slice 6.1.
 *
 * Every filesystem driver (ramfs in slice 6.2, a future ext2 / tmpfs / ...)
 * implements a `const struct nx_fs_ops` table and exposes a `void *self`
 * instance carrying its private state.  The upper VFS layer (slice 6.2's
 * `components/vfs_simple/`) consumes this interface once per active mount:
 * it calls through `ops->open / close / read / write` to translate a
 * `(path, flags)` pair on behalf of a userspace syscall into per-open
 * state and byte-stream operations on the underlying driver.
 *
 * Path convention.
 *   Paths passed to `open` are driver-local — the VFS strips any mount
 *   prefix before dispatch.  A single leading '/' is the root.  Slice
 *   6.1 permits any NUL-terminated string; ramfs will define its own
 *   naming rules when it lands.  Implementations must not dereference
 *   the `path` pointer past the call (they may copy the bytes they
 *   care about; the caller's buffer may be reused immediately after).
 *
 * Ownership.
 *   `open` returns an opaque per-open "file state" pointer.  Callers
 *   must pair each successful open with a matching `close` — the driver
 *   owns the state between those two calls and may free it in `close`.
 *   `read` / `write` advance a per-open cursor that lives inside the
 *   file-state object; two distinct opens on the same path have two
 *   distinct cursors.
 *
 * Concurrency.
 *   v1 is single-CPU.  All ops run under the same preemption discipline
 *   as the calling VFS layer (caller holds any serialisation it needs).
 *   Implementations do not need internal locks in slice 6.1.
 *
 * Error convention.
 *   Status-returning ops (`open`) use `NX_OK` / negative `NX_E*` from
 *   `framework/registry.h`.  Byte-count ops (`read` / `write`) return
 *   `>= 0` on success and a negative `NX_E*` on failure, matching the
 *   channel send/recv shape (see `framework/channel.h`).  `close` is
 *   declared `void` — callers cannot recover from a bad close.
 */

/* ---------- Open flags -------------------------------------------------- */
/*
 * Bitmask passed to `open`.  READ and WRITE select the requested access
 * mode (a driver MAY reject write-on-read-only or similar, returning
 * NX_EPERM).  CREATE asks the driver to create the file if absent —
 * absent this flag, `open` on a missing path returns NX_ENOENT.  Opening
 * an existing file WITH `CREATE` is permitted (not an error); truncate
 * semantics are driver-defined in v1 (a future slice adds an explicit
 * TRUNCATE bit when a consumer needs it).
 *
 * APPEND (slice 7.6d.N.11): every `write` advances the cursor to
 * end-of-file *before* writing.  Maps to POSIX `O_APPEND`; ash uses it
 * for `>>`-style stdout redirection.  Independent of WRITE in the bit
 * mask, but a driver MAY return NX_EPERM if a write happens on an
 * APPEND-only open without WRITE set.
 */
#define NX_FS_OPEN_READ     (1U << 0)
#define NX_FS_OPEN_WRITE    (1U << 1)
#define NX_FS_OPEN_CREATE   (1U << 2)
#define NX_FS_OPEN_APPEND   (1U << 3)

/* ---------- Seek whence (slice 6.4) ------------------------------------ */
#define NX_FS_SEEK_SET      0    /* absolute offset */
#define NX_FS_SEEK_CUR      1    /* relative to current cursor */
#define NX_FS_SEEK_END      2    /* relative to current file size */

/* ---------- Readdir entry (slice 6.4) ---------------------------------- */
/*
 * A single directory entry.  Fixed-size `name[]` for v1 so readdir can
 * bulk-copy into user space without per-entry malloc; `name_len` is
 * the byte count excluding the trailing NUL.  Name is always NUL-
 * terminated even when `name_len == NX_FS_DIRENT_NAME_MAX - 1`.
 *
 * Slice 7.7b.1: readdir takes a `dir_path` argument and returns
 * basenames of immediate children of that directory.  Drivers that
 * stored hierarchical paths verbatim (e.g. ramfs's `/bin/busybox`)
 * project them to the segment immediately following `dir_path` and
 * deduplicate within a single iteration so each directory child is
 * yielded exactly once.
 */
#define NX_FS_DIRENT_NAME_MAX  64u

struct nx_fs_dirent {
    uint32_t name_len;
    char     name[NX_FS_DIRENT_NAME_MAX];
};

/* ---------- Stat info (slice 7.7b.1) ----------------------------------- */
/*
 * Minimum metadata the syscall layer needs to distinguish a file from
 * a directory and report a size.  Filesystems may surface richer info
 * later; for v1 the shape is "kind + size" only — the syscall layer
 * synthesises owner/perms/timestamps when packing the Linux struct
 * stat for `sys_fstatat`.
 */
#define NX_FS_KIND_FILE   1u
#define NX_FS_KIND_DIR    2u

struct nx_fs_stat {
    uint32_t kind;     /* NX_FS_KIND_FILE or NX_FS_KIND_DIR */
    int64_t  size;     /* bytes; 0 for directories in v1 */
};

/* ---------- Ops table --------------------------------------------------- */

struct nx_fs_ops {
    /*
     * Open `path` on the driver instance `self`.  `flags` is a bitmask
     * of `NX_FS_OPEN_*`.  On success, `*out_file` is written with an
     * opaque per-open state pointer and NX_OK is returned.
     *
     * Returns:
     *   NX_OK      — *out_file holds the new file-state pointer.
     *   NX_EINVAL  — NULL args / empty path / unknown flag bits.
     *   NX_ENOENT  — path missing and CREATE not set.
     *   NX_EPERM   — driver refuses the requested access mode.
     *   NX_ENOMEM  — driver could not allocate per-open state.
     */
    int (*open)(void *self, const char *path, uint32_t flags,
                void **out_file);

    /*
     * Release per-open state returned by `open`.  Idempotent against
     * NULL.  After close, the `file` pointer is dead — passing it to
     * any other op is a programmer error (driver may assert / trap /
     * silently corrupt state; well-behaved callers don't do it).
     *
     * Slice 7.6d.N.8 introduces refcounted per-open state (see
     * `retain` below) so a `dup3(file_fd, ...)` + `close(file_fd)`
     * sequence keeps the duplicated handle alive.  `close` becomes
     * "decrement the refcount; release when zero".  Callers don't
     * see the refcount: pair every successful open / retain with
     * exactly one close.
     */
    void (*close)(void *self, void *file);

    /*
     * Bump the per-open's reference count (slice 7.6d.N.8).  Used by
     * the syscall layer when the same per-open state is referenced
     * from multiple handle slots (POSIX dup / dup2 / dup3 / fcntl
     * F_DUPFD; future fork-FILE inheritance) — the retained
     * reference must be paired with a matching `close`.
     *
     * Drivers that don't implement refcounting MAY leave this op
     * NULL; the syscall layer falls back to "no retain" in that
     * case.  ramfs implements it as a refcount bump on its
     * per-open struct.
     */
    void (*retain)(void *self, void *file);

    /*
     * Read up to `cap` bytes from `file` into `buf`, starting at the
     * per-open cursor.  Advances the cursor by the number of bytes
     * actually read.  `cap == 0` is permitted and returns 0.
     *
     * Returns:
     *   >= 0       — bytes read (0 = end-of-file at current cursor).
     *   NX_EINVAL  — NULL args (except `buf` when `cap == 0`).
     *   NX_EPERM   — file was opened without NX_FS_OPEN_READ.
     */
    int64_t (*read)(void *self, void *file, void *buf, size_t cap);

    /*
     * Write `len` bytes from `buf` into `file` at the per-open cursor,
     * extending the file if the cursor is at (or past) end-of-file.
     * Advances the cursor by the number of bytes actually written.
     * `len == 0` is permitted and returns 0.
     *
     * Returns:
     *   >= 0       — bytes written (may be < len if the backing store
     *                filled; caller retries with the remaining tail).
     *   NX_EINVAL  — NULL args (except `buf` when `len == 0`).
     *   NX_EPERM   — file was opened without NX_FS_OPEN_WRITE.
     *   NX_ENOMEM  — backing store exhausted before any bytes written.
     */
    int64_t (*write)(void *self, void *file, const void *buf, size_t len);

    /*
     * Reposition the per-open cursor (slice 6.4).  `whence` is one of
     * `NX_FS_SEEK_SET` (absolute), `_CUR` (relative to current cursor),
     * or `_END` (relative to current size).  The new position must
     * land in `[0, size]` inclusive — past-EOF seeks return NX_EINVAL
     * (no hole-filling in v1; callers write in sequence or SEEK_END).
     *
     * Returns the new absolute cursor position on success (≥ 0), or a
     * negative NX_E* on failure:
     *   NX_EINVAL  — NULL args, unknown whence, or resulting position
     *                outside [0, size].
     */
    int64_t (*seek)(void *self, void *file, int64_t offset, int whence);

    /*
     * Read the next entry under `dir_path` (slice 7.7b.1).
     *
     * `dir_path` is an absolute path (starts with `/`) naming a
     * directory whose immediate children should be enumerated.  For
     * `dir_path == "/"` the driver yields each top-level child.  For
     * `dir_path == "/bin"` the driver yields each entry whose stored
     * name has `/bin/` as a prefix, projected to the segment between
     * `/bin/` and the next `/` (or end-of-string).
     *
     * `cookie` is caller-owned iterator state: initialise to 0, pass
     * the same pointer on every call; the driver advances it so the
     * next call returns the next entry.  Cookies are driver-defined
     * (v1 ramfs uses the slot index directly) but always monotonically
     * advance — callers may save a cookie and resume later without
     * losing iteration position, but must not tamper with values the
     * driver wrote.
     *
     * `out->name` carries the basename only (no leading slash, no
     * embedded `/`).  Drivers must deduplicate within a single
     * iteration so each immediate child is yielded exactly once even
     * when several stored paths project to the same first segment
     * (e.g. `/bin/sh` and `/bin/cat` both project to `bin` when
     * iterating `/`).
     *
     * Returns:
     *   NX_OK      — `*out` populated; `*cookie` advanced.
     *   NX_ENOENT  — no more entries (iteration complete) or `dir_path`
     *                does not name an existing directory.
     *   NX_EINVAL  — NULL args / `dir_path` not absolute.
     */
    int (*readdir)(void *self, const char *dir_path,
                   uint32_t *cookie, struct nx_fs_dirent *out);

    /*
     * Create a directory at `path` (slice 7.7b.1).  `path` is absolute.
     * Parent directory existence is not enforced in v1 — `mkdir
     * /a/b/c` succeeds even when `/a` and `/a/b` are absent (consistent
     * with how the cpio loader stores file paths verbatim without
     * intermediate dir entries).
     *
     * Returns:
     *   NX_OK      — directory created.
     *   NX_EEXIST  — `path` already names a file or directory.
     *   NX_ENOMEM  — driver storage exhausted.
     *   NX_EINVAL  — NULL args / non-absolute / empty path.
     */
    int (*mkdir)(void *self, const char *path);

    /*
     * Report metadata for `path` (slice 7.7b.1).  Used by the syscall
     * layer to distinguish file from directory before deciding whether
     * to allocate a HANDLE_FILE or a HANDLE_DIR, and to report sizes
     * to `fstatat` callers.
     *
     * Drivers may synthesise directory results for path prefixes that
     * match no stored entry but match as a parent of one (e.g.
     * `stat("/bin")` returns DIR if `/bin/sh` is stored even though
     * `/bin` itself was never explicitly created).  The root path
     * `"/"` always reports DIR.
     *
     * Returns:
     *   NX_OK      — `*out` populated.
     *   NX_ENOENT  — no entry matches `path`.
     *   NX_EINVAL  — NULL args / non-absolute path.
     */
    int (*stat)(void *self, const char *path, struct nx_fs_stat *out);
};

#endif /* NONUX_INTERFACE_FS_H */
