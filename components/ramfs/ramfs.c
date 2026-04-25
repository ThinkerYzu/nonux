/*
 * ramfs — in-memory filesystem driver (slice 6.2).
 *
 * First real `filesystem` component for nonux.  Implements the universal
 * `nx_fs_ops` contract from `interfaces/fs.h`; passes the conformance
 * suite from slice 6.1 against the same harness that validated the
 * in-file `fs_stub` fixture.  No dependencies, no worker threads —
 * `spawns_threads: false`, `pause_hook: false`.
 *
 * v1 geometry matches the slice-6.1 stub: a fixed table of at most
 * `RAMFS_MAX_FILES` files, each with up to `RAMFS_FILE_CAP` bytes.
 * Everything lives inside the per-instance state struct — no heap
 * allocations, no `memory.page_alloc` dep.  That keeps slice 6.2
 * focused on "first filesystem component + first VFS component"
 * rather than on "first multi-component composition with runtime
 * storage allocation"; a follow-up can swap the static table for a
 * page-backed layout without touching the interface.
 *
 * Per-open state (`struct ramfs_open`) carries a cursor + flags + a
 * back-pointer to the file.  Allocated from a per-instance pool of
 * `RAMFS_MAX_OPEN` slots so the driver needs no dynamic allocator.
 * An exhausted pool returns NX_ENOMEM from open — the pool is sized
 * generously (4x file count) so a normal consumer mix can't hit it.
 *
 * Binding: kernel.json's "filesystem.root" slot picks ramfs by name.
 * The component's iface_ops (`ramfs_fs_ops`) is published through the
 * descriptor so vfs_simple — resolving the slot at call time — reads
 * it straight off `slot->active->descriptor->iface_ops`.
 */

#include "framework/component.h"
#include "framework/registry.h"
#include "interfaces/fs.h"

#include <stddef.h>
#include <stdint.h>

#if __STDC_HOSTED__
#include <string.h>
#else
#include "core/lib/lib.h"
#endif

#define RAMFS_MAX_FILES   8u
#define RAMFS_NAME_MAX   32u
#define RAMFS_FILE_CAP  4096u   /* slice 7.4c: up from 256 so a minimal
                                 * static ELF (~150 B after -n-pack via the
                                 * linker) fits at a ramfs path, giving
                                 * `NX_SYS_EXEC` something to read. */
#define RAMFS_MAX_OPEN  (4u * RAMFS_MAX_FILES)  /* generous: no dynamic allocator */

struct ramfs_file {
    int      in_use;
    char     name[RAMFS_NAME_MAX];
    uint8_t  data[RAMFS_FILE_CAP];
    size_t   size;
};

struct ramfs_open {
    int                in_use;
    struct ramfs_file *file;
    uint32_t           flags;
    size_t             cursor;
};

struct ramfs_state {
    struct ramfs_file files[RAMFS_MAX_FILES];
    struct ramfs_open opens[RAMFS_MAX_OPEN];

    /* Lifecycle counters for test introspection. */
    unsigned init_called;
    unsigned enable_called;
    unsigned disable_called;
    unsigned destroy_called;
};

/* ---------- File-table helpers --------------------------------------- */

static struct ramfs_file *ramfs_find(struct ramfs_state *s, const char *path)
{
    for (unsigned i = 0; i < RAMFS_MAX_FILES; i++) {
        if (s->files[i].in_use &&
            strncmp(s->files[i].name, path, RAMFS_NAME_MAX - 1) == 0)
            return &s->files[i];
    }
    return NULL;
}

static struct ramfs_file *ramfs_create_file(struct ramfs_state *s,
                                            const char *path)
{
    size_t plen = 0;
    while (path[plen] != '\0') plen++;
    if (plen == 0 || plen >= RAMFS_NAME_MAX) return NULL;

    for (unsigned i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!s->files[i].in_use) {
            struct ramfs_file *f = &s->files[i];
            f->in_use = 1;
            memcpy(f->name, path, plen + 1);  /* include NUL */
            f->size = 0;
            return f;
        }
    }
    return NULL;
}

static struct ramfs_open *ramfs_alloc_open(struct ramfs_state *s)
{
    for (unsigned i = 0; i < RAMFS_MAX_OPEN; i++) {
        if (!s->opens[i].in_use) {
            s->opens[i].in_use = 1;
            return &s->opens[i];
        }
    }
    return NULL;
}

/* ---------- nx_fs_ops implementations -------------------------------- */

static int ramfs_op_open(void *self, const char *path, uint32_t flags,
                         void **out_file)
{
    const uint32_t known = NX_FS_OPEN_READ | NX_FS_OPEN_WRITE |
                           NX_FS_OPEN_CREATE;
    if (!self || !path || !out_file) return NX_EINVAL;
    if (path[0] == '\0') return NX_EINVAL;
    if (flags & ~known) return NX_EINVAL;

    struct ramfs_state *s = self;

    struct ramfs_file *file = ramfs_find(s, path);
    if (!file) {
        if (!(flags & NX_FS_OPEN_CREATE)) return NX_ENOENT;
        file = ramfs_create_file(s, path);
        if (!file) return NX_ENOMEM;
    }

    struct ramfs_open *op = ramfs_alloc_open(s);
    if (!op) return NX_ENOMEM;
    op->file   = file;
    op->flags  = flags;
    op->cursor = 0;

    *out_file = op;
    return NX_OK;
}

static void ramfs_op_close(void *self, void *file)
{
    (void)self;
    if (!file) return;
    struct ramfs_open *op = file;
    op->in_use = 0;
    op->file   = NULL;
    op->flags  = 0;
    op->cursor = 0;
}

static int64_t ramfs_op_read(void *self, void *file, void *buf, size_t cap)
{
    (void)self;
    if (!file) return NX_EINVAL;
    if (cap == 0) return 0;
    if (!buf) return NX_EINVAL;

    struct ramfs_open *op = file;
    if (!(op->flags & NX_FS_OPEN_READ)) return NX_EPERM;

    size_t remain = (op->cursor < op->file->size)
                    ? op->file->size - op->cursor : 0;
    size_t n = remain < cap ? remain : cap;
    if (n > 0) memcpy(buf, op->file->data + op->cursor, n);
    op->cursor += n;
    return (int64_t)n;
}

static int64_t ramfs_op_write(void *self, void *file, const void *buf,
                              size_t len)
{
    (void)self;
    if (!file) return NX_EINVAL;
    if (len == 0) return 0;
    if (!buf) return NX_EINVAL;

    struct ramfs_open *op = file;
    if (!(op->flags & NX_FS_OPEN_WRITE)) return NX_EPERM;

    size_t room = (op->cursor < RAMFS_FILE_CAP)
                  ? RAMFS_FILE_CAP - op->cursor : 0;
    if (room == 0) return NX_ENOMEM;
    size_t n = len < room ? len : room;
    memcpy(op->file->data + op->cursor, buf, n);
    op->cursor += n;
    if (op->cursor > op->file->size) op->file->size = op->cursor;
    return (int64_t)n;
}

static int64_t ramfs_op_seek(void *self, void *file,
                             int64_t offset, int whence)
{
    (void)self;
    if (!file) return NX_EINVAL;
    struct ramfs_open *op = file;

    int64_t base;
    switch (whence) {
    case NX_FS_SEEK_SET: base = 0;                        break;
    case NX_FS_SEEK_CUR: base = (int64_t)op->cursor;      break;
    case NX_FS_SEEK_END: base = (int64_t)op->file->size;  break;
    default:             return NX_EINVAL;
    }
    int64_t new_pos = base + offset;
    if (new_pos < 0 || (uint64_t)new_pos > op->file->size) return NX_EINVAL;
    op->cursor = (size_t)new_pos;
    return new_pos;
}

static int ramfs_op_readdir(void *self, uint32_t *cookie,
                            struct nx_fs_dirent *out)
{
    if (!self || !cookie || !out) return NX_EINVAL;
    struct ramfs_state *s = self;

    /* Advance past empty slots from the cookie forward.  Cookie is the
     * zero-based file-table index; past RAMFS_MAX_FILES means done. */
    for (uint32_t i = *cookie; i < RAMFS_MAX_FILES; i++) {
        if (!s->files[i].in_use) continue;

        size_t nlen = 0;
        while (s->files[i].name[nlen] != '\0' &&
               nlen < NX_FS_DIRENT_NAME_MAX - 1) nlen++;
        out->name_len = (uint32_t)nlen;
        memcpy(out->name, s->files[i].name, nlen);
        out->name[nlen] = '\0';
        *cookie = i + 1;
        return NX_OK;
    }
    *cookie = RAMFS_MAX_FILES;
    return NX_ENOENT;
}

const struct nx_fs_ops ramfs_fs_ops = {
    .open    = ramfs_op_open,
    .close   = ramfs_op_close,
    .read    = ramfs_op_read,
    .write   = ramfs_op_write,
    .seek    = ramfs_op_seek,
    .readdir = ramfs_op_readdir,
};

/* ---------- Component lifecycle -------------------------------------- */

/* ---------- Initramfs slurp (slice 7.6b) ----------------------------- *
 *
 * Walks a cpio-newc blob (RFC: kernel.org/doc/html/latest/driver-api/
 * early-userspace/buffer-format.html, also `man 5 cpio`) at component
 * init() and seeds the ramfs file table with each entry.  The blob is
 * delivered through a pair of weak symbols (`__ramfs_initramfs_blob_
 * start` / `_end`) so production `kernel.bin` builds — which never
 * provide them — see start == end and skip the slurp entirely; only
 * the test build (which embeds an actual cpio via `.incbin`) does the
 * work.
 *
 * Format reminder (per entry, all hex fields are 8 ASCII characters):
 *
 *   c_magic[6] "070701"
 *   c_ino, c_mode, c_uid, c_gid, c_nlink, c_mtime, c_filesize,
 *   c_devmajor, c_devminor, c_rdevmajor, c_rdevminor,
 *   c_namesize, c_check     ← total 13 × 8 = 104 chars; +6 magic = 110.
 *   name (NUL-terminated)   ← `c_namesize` bytes including the NUL,
 *                              padded to 4-byte boundary.
 *   data                    ← `c_filesize` bytes, padded to 4 bytes.
 *
 * A trailer entry named "TRAILER!!!" with `c_filesize == 0` ends the
 * archive — we stop scanning when we see the name match.
 *
 * v1 supports regular files only.  Directories (mode bits 0o040000)
 * and symlinks (0o120000) are silently skipped; ramfs's flat
 * single-mount layout has no representation for them.  busybox's
 * standard initramfs layout uses dir entries to define the rootfs
 * structure but works fine when they're absent (the files just live
 * in a flat namespace), so this gap is OK until slice 7.6d's full
 * busybox integration explicitly needs hierarchical paths.
 */

extern char __ramfs_initramfs_blob_start[] __attribute__((weak));
extern char __ramfs_initramfs_blob_end[]   __attribute__((weak));

#define CPIO_NEWC_HEADER_SIZE  110u
#define CPIO_NEWC_MAGIC        "070701"

static unsigned cpio_hex8(const char *p)
{
    unsigned v = 0;
    for (int i = 0; i < 8; i++) {
        char c = p[i];
        v <<= 4;
        if      (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else return 0xFFFFFFFFu;     /* sentinel — caller treats as "stop" */
    }
    return v;
}

static size_t cpio_align4(size_t v) { return (v + 3u) & ~(size_t)3u; }

/*
 * Stuff the blob's contents into the ramfs file table.  Best-effort:
 * malformed entries terminate the walk silently (we can't kprintf
 * here without a kernel-only include path on host builds, and an
 * unparseable blob is the build-system's bug, not a runtime concern).
 * Returns the number of files seeded.
 */
static unsigned ramfs_slurp_initramfs(struct ramfs_state *s,
                                      const char *blob, size_t total)
{
    unsigned count = 0;
    size_t   off   = 0;
    while (off + CPIO_NEWC_HEADER_SIZE <= total) {
        const char *hdr = blob + off;
        for (int i = 0; i < 6; i++)
            if (hdr[i] != CPIO_NEWC_MAGIC[i]) return count;

        unsigned mode      = cpio_hex8(hdr + 6 + 1 * 8);
        unsigned filesize  = cpio_hex8(hdr + 6 + 6 * 8);
        unsigned namesize  = cpio_hex8(hdr + 6 + 11 * 8);
        if (mode == 0xFFFFFFFFu || filesize == 0xFFFFFFFFu ||
            namesize == 0xFFFFFFFFu) return count;

        size_t name_off = off + CPIO_NEWC_HEADER_SIZE;
        if (name_off + namesize > total) return count;
        const char *name = blob + name_off;

        /* TRAILER!!! marks end of archive. */
        if (namesize >= 11 &&
            strncmp(name, "TRAILER!!!", 10) == 0 && name[10] == '\0')
            return count;

        size_t data_off = cpio_align4(name_off + namesize);
        if (data_off + filesize > total) return count;

        /* Only seed regular files (S_IFREG = 0o100000 = 0x8000); skip
         * dirs / symlinks / special files. */
        if ((mode & 0xF000u) == 0x8000u && filesize <= RAMFS_FILE_CAP) {
            /* `name` may include path separators (e.g. "bin/sh");
             * v1 ramfs is flat-namespace, so we just store the name
             * verbatim — vfs_simple's caller addresses files via
             * the same string.  When we add hierarchical paths we'll
             * tweak the parser side rather than the storage side. */
            char path[RAMFS_NAME_MAX + 1];
            path[0] = '/';
            size_t copy = namesize ? namesize - 1 : 0;
            if (copy >= RAMFS_NAME_MAX - 1) copy = RAMFS_NAME_MAX - 2;
            memcpy(path + 1, name, copy);
            path[1 + copy] = '\0';

            struct ramfs_file *f = ramfs_create_file(s, path);
            if (f) {
                memcpy(f->data, blob + data_off, filesize);
                f->size = filesize;
                count++;
            }
        }

        off = cpio_align4(data_off + filesize);
    }
    return count;
}

static int ramfs_init(void *self)
{
    struct ramfs_state *s = self;

    /* Framework zeroes state via calloc before init; this loop documents
     * the invariant that every file / open slot starts free, so a reader
     * doesn't have to know about the calloc. */
    for (unsigned i = 0; i < RAMFS_MAX_FILES; i++) s->files[i].in_use = 0;
    for (unsigned i = 0; i < RAMFS_MAX_OPEN;  i++) s->opens[i].in_use = 0;

    /* Slice 7.6b: if a non-empty initramfs blob was linked in (test
     * builds embed one via `.incbin`; production builds leave the
     * weak symbols at 0), parse it and seed the file table.  The
     * `&__sym[0]` form is to evade gcc's `-Warray-compare` when both
     * sides are array-typed weak externs — we want the address
     * comparison the linker actually resolves, not the array-decay-
     * to-pointer warning. */
    const char *blob_start = &__ramfs_initramfs_blob_start[0];
    const char *blob_end   = &__ramfs_initramfs_blob_end[0];
    if (blob_start && blob_end && blob_end > blob_start) {
        size_t total = (size_t)(blob_end - blob_start);
        ramfs_slurp_initramfs(s, blob_start, total);
    }

    s->init_called++;
    return NX_OK;
}

static int ramfs_enable(void *self)
{
    struct ramfs_state *s = self;
    s->enable_called++;
    return NX_OK;
}

static int ramfs_disable(void *self)
{
    struct ramfs_state *s = self;
    s->disable_called++;
    /* Files survive an enable/disable cycle — open handles are a
     * separate question (the VFS layer owns those, and the lifecycle
     * contract is that disable happens only after all ops have drained).
     * A subsequent enable resumes with the same files present. */
    return NX_OK;
}

static void ramfs_destroy(void *self)
{
    struct ramfs_state *s = self;
    s->destroy_called++;
    /* Storage is inline in the state struct; freeing the state frees
     * everything.  The framework owns state allocation. */
}

const struct nx_component_ops ramfs_component_ops = {
    .init    = ramfs_init,
    .enable  = ramfs_enable,
    .disable = ramfs_disable,
    .destroy = ramfs_destroy,
    /* No pause_hook: spawns_threads is false in the manifest.
     * No handle_msg: ramfs is consumed through iface_ops by vfs_simple,
     * not via the IPC router. */
};

NX_COMPONENT_REGISTER_NO_DEPS_IFACE(ramfs,
                                    struct ramfs_state,
                                    &ramfs_component_ops,
                                    &ramfs_fs_ops);
