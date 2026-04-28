#!/bin/sh
# run.sh — slice 7.6d.N.final.d.  Drives each *.script file in this
# directory through `tools/qemu-stdin-feed.sh` and verifies the
# captured kernel log contains every line of the matching *.expected
# file (substring match — busybox's prompts and ash's other output
# are treated as noise).  Exits non-zero on the first mismatch.
#
# Why substring rather than `diff` against the whole log: the kernel
# boot dump (`[boot] ... [pmm] ... [fw] ...`) lands in the same UART
# stream as busybox's output and would tank a strict diff.  The
# expected file is just the "user-visible commands' stdout" we care
# about.
set -eu

DIR=$(dirname "$0")
KERNEL=kernel-busybox.bin

if [ ! -f "$KERNEL" ]; then
    echo "test/interactive/run.sh: $KERNEL missing" >&2
    echo "  run \`make $KERNEL\` first" >&2
    exit 2
fi

fail=0
for script in "$DIR"/*.script; do
    [ -f "$script" ] || continue
    name=$(basename "$script" .script)
    expected="$DIR/$name.expected"
    if [ ! -f "$expected" ]; then
        echo "  $name: SKIP (no .expected)"
        continue
    fi

    log="test/kernel-output-busybox-$name.log"
    : > "$log"

    # Trickle the script byte-by-byte with a small gap between
    # consecutive bytes.  QEMU's `-serial stdio` chardev does not
    # backpressure when PL011's 16-byte RX FIFO is full — surplus
    # bytes are silently dropped at the host/guest boundary — so a
    # full-line burst like `echo PIPE_INPUT | tr a-z A-Z\n` (29 B)
    # routinely loses characters even though our IRQ-drained software
    # ring is 256 B wide.  Per-byte feeding gives the IRQ handler
    # time to drain the hardware FIFO between bytes; the per-byte
    # cost (4 ms × ~30 B = 120 ms / line) is invisible against the
    # ~10 s end-to-end test budget.  Slice 7.6d.N.final.e changed the
    # feed mode from line-burst → per-byte for this reason; the
    # earlier line-burst trickle worked in slice .d only because
    # baseline busybox ran without a visible prompt and ash's
    # parser tolerated lost echo bytes (the test only grep'd for
    # the final pipe output, not the typed input echo).
    {
        # Give the kernel ~5 s to finish boot before the first byte.
        # Anything less occasionally races with busybox's slow startup
        # path (mallocng's first slab, AUXV walk, isatty probe);
        # bytes that arrive before the first read(0, ...) get queued
        # in the RX ring fine, but QEMU's chardev pipe is sticky enough
        # that early bytes occasionally don't make it across.
        sleep 5
        while IFS= read -r line; do
            i=0
            while [ $i -lt ${#line} ]; do
                printf '%s' "$(printf '%s' "$line" | cut -c $((i+1)))"
                i=$((i+1))
                sleep 0.05
            done
            printf '\n'
            sleep 0.5
        done < "$script"
        # Hold the pipe open briefly after the last command so QEMU's
        # chardev has a moment to flush before stdin EOF.
        sleep 3
    } | timeout --preserve-status 60 \
        qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a53 \
            -display none -serial stdio -monitor none \
            -m 1G -kernel "$KERNEL" > "$log" 2>&1 || true

    miss=0
    while IFS= read -r line; do
        # Skip blank lines in expected
        case $line in '') continue ;; esac
        if ! grep -qF "$line" "$log"; then
            echo "  $name: FAIL — missing line: $line"
            miss=1
            fail=1
        fi
    done < "$expected"
    if [ $miss -eq 0 ]; then
        echo "  $name: PASS"
    fi
done

if [ $fail -ne 0 ]; then
    echo
    echo "test-interactive: at least one canned script failed."
    echo "  inspect test/kernel-output-busybox-*.log for full output"
    exit 1
fi
echo
echo "test-interactive: all canned scripts passed."
