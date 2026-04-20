#!/bin/bash
# Run the nonux kernel in QEMU (ARM64 virt, cortex-a53).
#
# Usage:
#   tools/run-qemu.sh                run until Ctrl-A X (interactive)
#   tools/run-qemu.sh -t 5           run, terminate after 5 seconds
#   tools/run-qemu.sh -- -append X   pass extra flags to QEMU
#   tools/run-qemu.sh -t 5 -- -append X    both
#
# Env overrides: QEMU (default: qemu-system-aarch64), QEMU_MEM (default: 1G).

set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
KERNEL="$ROOT_DIR/kernel.bin"

QEMU="${QEMU:-qemu-system-aarch64}"
MEM="${QEMU_MEM:-1G}"
TIMEOUT=""

usage() {
    sed -n '3,11p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case "$1" in
        -t|--timeout) TIMEOUT="$2"; shift 2 ;;
        -h|--help)    usage; exit 0 ;;
        --)           shift; break ;;
        -*)           echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
        *)            break ;;
    esac
done

if [ ! -f "$KERNEL" ]; then
    echo "kernel.bin not found at $KERNEL — run 'make' first" >&2
    exit 1
fi

CMD=("$QEMU" -M virt,gic-version=2 -cpu cortex-a53 -nographic -kernel "$KERNEL" -m "$MEM" "$@")

if [ -n "$TIMEOUT" ]; then
    exec timeout --preserve-status "$TIMEOUT" "${CMD[@]}"
else
    exec "${CMD[@]}"
fi
