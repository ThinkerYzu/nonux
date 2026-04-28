/*
 * Slice 7.6d.N.final.b — busybox-as-/init initramfs blob.
 *
 * Same shape as initramfs_blob.S but `.incbin`s the busybox-init cpio
 * (built by `pack-initramfs.py --busybox-init=$(BUSYBOX_BIN)` so
 * `/init` is the busybox binary).  Linked into kernel-busybox.bin
 * INSTEAD of initramfs_blob.S — the symbols collide deliberately;
 * each binary picks one variant.
 */

.section .rodata
.balign 16
.global __ramfs_initramfs_blob_start
.global __ramfs_initramfs_blob_end

__ramfs_initramfs_blob_start:
.incbin "test/kernel/initramfs-busybox.cpio"
__ramfs_initramfs_blob_end:
