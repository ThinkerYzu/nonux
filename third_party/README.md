# third_party/

Pre-built third-party binaries and libraries used for integration testing.
Nothing in this directory is compiled from source during a normal `make` —
the binaries are checked in as-is.

## `busybox/`

Statically linked busybox binary for ARM64. Used as the userspace shell and
utility set in integration tests.

`tools/build-busybox.sh` documents how to reproduce the build (ARM64 static
musl config). The resulting binary is packed into `initramfs-busybox.cpio` by
`tools/pack-initramfs.py` and embedded in `kernel-busybox.bin` via
`test/kernel/initramfs_busybox_blob.S`.

Tests that run the busybox shell live in `test/kernel/ktest_posix_busybox*.c`
and `test/interactive/`.

## `musl/`

musl libc headers and pre-built static library for ARM64. Used by EL0 test
programs that need a real libc (formatted I/O, math, etc.) rather than the
minimal `lib/libnxlibc/` stubs.

musl-linked programs are compiled with `tools/musl-aarch64-gcc.sh` (a small
wrapper that sets the right sysroot and flags) and embedded as blobs in
`test/kernel/` alongside their crt0-linked counterparts.
