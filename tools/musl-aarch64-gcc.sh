#!/bin/bash
#
# tools/musl-aarch64-gcc.sh — wrapper around aarch64-linux-gnu-gcc that
# forces use of our patched musl instead of the cross-toolchain's
# bundled glibc.
#
# Slice 7.6d.3c discovery: passing `--sysroot=$MUSL_SYSROOT` and
# `-static` to busybox's build was *not* enough to get a musl-linked
# binary.  gcc's `-print-search-dirs --sysroot=...` shows the cross-
# toolchain's glibc paths (/usr/aarch64-linux-gnu/lib/...) come BEFORE
# the sysroot paths in the library search order, so `-lc` resolves
# to libc.so/.a from glibc.  The resulting busybox links
# `__libc_setup_tls` + `_IO_*` (glibc internals) and never touches
# musl's syscall translation — so every syscall goes via the raw
# Linux number, returns -ENOSYS, and busybox crashes early in init.
#
# This wrapper biases the search order toward musl by adding
# `-nostdinc -isystem $MUSL_SYSROOT/include` (force only musl
# headers) and `-Wl,-rpath-link=$MUSL_SYSROOT/lib -B$MUSL_SYSROOT/lib`
# (force musl crt1 / crti / crtn / libc.a to be found before glibc).
# Compiler builtins like libgcc.a still come from the cross-
# toolchain's gcc directory, which is what we want.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SYSROOT="$ROOT/third_party/musl/_sysroot"

if [ ! -f "$SYSROOT/lib/libc.a" ]; then
    echo "musl-aarch64-gcc.sh: $SYSROOT/lib/libc.a missing" >&2
    echo "musl-aarch64-gcc.sh: run 'make musl-libc' + 'make busybox' first" >&2
    exit 1
fi

# `-nostdinc` strips out the cross-toolchain's stdio etc. headers
# but ALSO strips its kernel headers (linux/*.h, asm/*.h).  busybox's
# kbd_mode / mount / mtab handling include a few of those.  Add the
# cross-toolchain's include path AFTER musl's so libc-namespace
# headers (stdio.h etc.) come from musl while linux/* still resolves.
KERNEL_INCLUDES="/usr/aarch64-linux-gnu/include"

exec aarch64-linux-gnu-gcc \
    -nostdinc \
    -isystem "$SYSROOT/include" \
    -isystem "$KERNEL_INCLUDES" \
    -B "$SYSROOT/lib/" \
    "$@"
