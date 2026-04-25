/*
 * Kernel-side coverage for slice 7.6b — initramfs slurp at ramfs init.
 *
 * `tools/pack-initramfs.py` packs a cpio-newc archive containing
 * `/init` (init_prog.elf, ~800 bytes) and `/banner` ("hello from
 * initramfs\n") into `test/kernel/initramfs.cpio`.  `initramfs_blob.S`
 * embeds the bytes in `kernel-test.bin`'s `.rodata`.  At boot, ramfs's
 * `init` walks the blob and creates the matching files via
 * `ramfs_create_file`.
 *
 * `/banner` is deliberately NOT named `/hello` — `ktest_vfs` already
 * creates a `/hello` file via vfs_simple's create flow, and the
 * seeded version would either collide with that test or get
 * overwritten depending on KTEST registration order.
 *
 * This test goes through vfs_simple (the same path as a syscall) to
 * prove the seeded files are reachable through the live VFS — not
 * just visible in the driver's private state.  Two separate
 * assertions:
 *
 *   1. `/banner` opens, its bytes match "hello from initramfs\n".
 *   2. `/init` opens; the first 4 bytes are the ELF magic
 *      (0x7F, 'E', 'L', 'F').
 *
 * Either failure mode means either the cpio packer is wrong, the
 * parser is wrong, or the `.incbin` hook isn't reaching ramfs (for
 * example a stale ramfs.o left over from a previous build wasn't
 * relinked against the current blob — `make clean` covers this).
 */

#include "ktest.h"

#include "framework/component.h"
#include "framework/registry.h"
#include "interfaces/vfs.h"

KTEST(initramfs_seed_makes_banner_readable_through_vfs)
{
    struct nx_slot *vs = nx_slot_lookup("vfs");
    KASSERT_NOT_NULL(vs);
    KASSERT_NOT_NULL(vs->active);
    KASSERT_NOT_NULL(vs->active->descriptor);
    const struct nx_vfs_ops *vops =
        (const struct nx_vfs_ops *)vs->active->descriptor->iface_ops;
    void *vself = vs->active->impl;
    KASSERT_NOT_NULL(vops);

    void *file = NULL;
    int rc = vops->open(vself, "/banner", NX_VFS_OPEN_READ, &file);
    KASSERT_EQ_U(rc, NX_OK);
    KASSERT_NOT_NULL(file);

    char buf[64] = { 0 };
    int64_t n = vops->read(vself, file, buf, sizeof buf - 1);
    KASSERT(n > 0);
    /* "hello from initramfs\n" — 21 bytes including the newline. */
    KASSERT_EQ_U((uint64_t)n, 21);

    static const char expected[] = "hello from initramfs\n";
    for (int i = 0; i < n; i++) {
        if (buf[i] != expected[i]) { KASSERT(0); }
    }
    vops->close(vself, file);
}

KTEST(initramfs_seed_includes_init_with_elf_magic)
{
    struct nx_slot *vs = nx_slot_lookup("vfs");
    const struct nx_vfs_ops *vops =
        (const struct nx_vfs_ops *)vs->active->descriptor->iface_ops;
    void *vself = vs->active->impl;

    void *file = NULL;
    int rc = vops->open(vself, "/init", NX_VFS_OPEN_READ, &file);
    KASSERT_EQ_U(rc, NX_OK);

    uint8_t hdr[4] = { 0 };
    int64_t n = vops->read(vself, file, hdr, sizeof hdr);
    KASSERT_EQ_U((uint64_t)n, sizeof hdr);
    /* ELF magic: 0x7F 'E' 'L' 'F' */
    KASSERT_EQ_U(hdr[0], 0x7Fu);
    KASSERT_EQ_U(hdr[1], (uint64_t)'E');
    KASSERT_EQ_U(hdr[2], (uint64_t)'L');
    KASSERT_EQ_U(hdr[3], (uint64_t)'F');
    vops->close(vself, file);
}
