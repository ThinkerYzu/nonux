#!/bin/bash
#
# tools/build-busybox.sh — cross-compile busybox against our patched musl.
#
# Slice 7.6d.1 build infrastructure.  Vendored busybox lives under
# third_party/busybox/.  Our minimal config snapshot is at
# third_party/busybox/configs/nonux_defconfig (committed to the tree;
# this script copies it into .config and runs olddefconfig before the
# real build, mirroring the standard busybox per-config workflow).
#
# musl is consumed via a private sysroot (third_party/musl/_sysroot/)
# populated lazily: include/ comes from `make install-headers` against
# our patched musl tree; lib/ symlinks back to third_party/musl/lib/
# so libc.a + the crt set stay coherent with whatever the top-level
# `make musl-libc` last produced.
#
# Output: third_party/busybox/busybox (static aarch64 ELF).

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUSYBOX_DIR="$ROOT/third_party/busybox"
MUSL_DIR="$ROOT/third_party/musl"
SYSROOT="$MUSL_DIR/_sysroot"
CONFIG="$BUSYBOX_DIR/configs/nonux_defconfig"
CROSS="${CROSS:-aarch64-linux-gnu-}"
JOBS="${JOBS:-$(nproc)}"

# 1. musl libc.a must already be built (top-level `make musl-libc`).
if [ ! -f "$MUSL_DIR/lib/libc.a" ]; then
    echo "build-busybox.sh: $MUSL_DIR/lib/libc.a missing" >&2
    echo "build-busybox.sh: run 'make musl-libc' first" >&2
    exit 1
fi

# 2. Populate the private musl sysroot (idempotent).
if [ ! -f "$SYSROOT/include/stdio.h" ]; then
    echo "build-busybox.sh: installing musl headers into $SYSROOT"
    make -C "$MUSL_DIR" install-headers prefix="$SYSROOT" >/dev/null
fi
mkdir -p "$SYSROOT/lib"
for f in libc.a crt1.o crti.o crtn.o; do
    [ -L "$SYSROOT/lib/$f" ] || ln -sf "../../lib/$f" "$SYSROOT/lib/$f"
done

# 3. Stage our config + let busybox auto-fill anything new.  busybox
# 1.36.1's kconfig predates `olddefconfig`, so feed `oldconfig` empty
# answers via `yes ''` to take whichever default the symbol's prompt
# carries (matches the behaviour of olddefconfig in newer kernels).
# `yes` exits with SIGPIPE once oldconfig closes its stdin; that's
# expected, so we trap pipefail off for this one invocation.
cp "$CONFIG" "$BUSYBOX_DIR/.config"
set +o pipefail
yes "" | make -C "$BUSYBOX_DIR" \
    ARCH=arm64 \
    CROSS_COMPILE="$CROSS" \
    oldconfig >/dev/null
set -o pipefail

# 4. Real build.  --sysroot points at our private musl tree; -static
# pulls libc.a in completely.  SKIP_STRIP=y leaves debug info in place
# so we can readelf the output.
#
# -Ttext-segment=0x48000000 (slice 7.6d.2a): the kbuild default for
# `-static -no-pie` aarch64 is text at VA 0x400000.  Our ELF loader
# (framework/elf.c) honours PT_LOAD.p_vaddr verbatim — a load to
# 0x400000 lands in MMIO space.  Pin text at the user-window base so
# segments end up where the loader can actually copy them.  The value
# is hardcoded here; it has to track mmu_user_window_base() in
# core/mmu/mmu.c (and init_prog.ld's `. = 0x48000000;`).  If the user
# window ever moves, this script + every other linker script that
# spells out 0x48000000 has to move with it.
USER_WINDOW_BASE="0x48000000"

make -C "$BUSYBOX_DIR" \
    -j "$JOBS" \
    ARCH=arm64 \
    CROSS_COMPILE="$CROSS" \
    EXTRA_CFLAGS="--sysroot=$SYSROOT" \
    EXTRA_LDFLAGS="--sysroot=$SYSROOT -static -Wl,-Ttext-segment=$USER_WINDOW_BASE" \
    SKIP_STRIP=y

# 5. Sanity check.  busybox's own Makefile prints a size summary;
# we add the file-type check so a corrupt link is caught loudly.
file "$BUSYBOX_DIR/busybox"
size_bytes="$(stat -c %s "$BUSYBOX_DIR/busybox")"
echo "build-busybox.sh: ok ($size_bytes bytes)"
