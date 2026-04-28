#!/bin/sh
# qemu-stdin-feed.sh — drive kernel-busybox.bin under QEMU with a
# scripted stdin payload.  Slice 7.6d.N.final.d.
#
# Usage:
#   tools/qemu-stdin-feed.sh SCRIPT [TIMEOUT_S]
#
# SCRIPT  — path to a file whose contents are streamed verbatim into
#           QEMU's UART RX.  Lines should be NEWLINE-terminated; ash
#           reads byte-by-byte.  A final `exit\n` (or Ctrl-D = octal
#           \004) is recommended so busybox doesn't sit in a read
#           after the payload is exhausted.
# TIMEOUT — wall-clock seconds before the run is forcibly killed
#           (default 20).
#
# Combined output (kernel boot log + busybox stdout) is written to
# `test/kernel-output-busybox.log` AND streamed to stdout.  Exit
# status is the QEMU process's exit status; `timeout`'s 124 means
# the script ran past the budget.
#
# Bytes between SCRIPT and QEMU travel through a host-side `cat`
# pipe; each script line is delivered as a stream of UART bytes.
# The PL011 RX FIFO is only 16 entries deep, so unusually long
# pasted lines may drop characters — keep individual commands
# short.
set -eu

SCRIPT=${1:?missing SCRIPT path}
TIMEOUT=${2:-20}

if [ ! -f "$SCRIPT" ]; then
    echo "qemu-stdin-feed.sh: no such file: $SCRIPT" >&2
    exit 2
fi

KERNEL=kernel-busybox.bin
LOG=test/kernel-output-busybox.log

if [ ! -f "$KERNEL" ]; then
    echo "qemu-stdin-feed.sh: $KERNEL missing — run \`make $KERNEL\` first" >&2
    exit 2
fi

: > "$LOG"

cat "$SCRIPT" | timeout --preserve-status "$TIMEOUT" \
    qemu-system-aarch64 \
        -M virt,gic-version=2 -cpu cortex-a53 -nographic \
        -kernel "$KERNEL" -m 1G | tee "$LOG"
