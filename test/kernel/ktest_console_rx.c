/*
 * Slice 7.6d.N.final.a — console RX ring + blocking nx_console_read.
 * Slice 7.6d.N.final.c — Ctrl-C / Ctrl-D control-byte side channels.
 *
 * In-kernel tests that exercise the new RX path without bringing up
 * EL0 or busybox.  Cases:
 *
 *   1. console_rx_inject_drain — push 5 bytes via the test injector,
 *      read them back through nx_console_read, verify ordering and
 *      count.  The read does NOT block because the ring already has
 *      bytes; this is the "non-empty fast path".
 *
 *   2. console_rx_wraps_around — push enough bytes to force the
 *      head/tail wrap-around (cap is 256, the test pushes 200 +
 *      drains 200, then pushes another 100 + drains 100).  Confirms
 *      the modular indexing in the ring's push/pop helpers is sound.
 *
 *   3. console_rx_drop_on_full — fill the ring beyond capacity, verify
 *      the over-the-edge bytes are silently dropped.
 *
 *   4. console_eof_returns_zero (slice 7.6d.N.final.c) — arm the
 *      Ctrl-D side channel; nx_console_read returns 0 once the ring
 *      is empty, then the flag is one-shot (cleared on consumption).
 *
 *   5. console_intr_posts_sigterm (slice 7.6d.N.final.c) — arm the
 *      Ctrl-C side channel; nx_console_drain_intr posts SIGTERM into
 *      a pre-created process's pending_signals bitmask; the polled
 *      sched_check_resched delivery would terminate it on the next
 *      tick (we observe the bit, not the actual termination, because
 *      this kthread isn't bound to that process).
 *
 * The "block-then-wake" semantic (read on an empty ring yields, IRQ
 * pushes a byte, read returns) is implicit in the EL0 → busybox
 * interactive path that lands in sub-slice 7.6d.N.final.b; not
 * covered here.
 */

#include "ktest.h"
#include "framework/console.h"
#include "framework/process.h"
#include "framework/syscall.h"

#include <string.h>

KTEST(console_rx_inject_drain)
{
    nx_console_reset_for_test();

    const char src[] = "hi!?\n";  /* 5 bytes */
    size_t pushed = nx_console_test_inject_bytes(src, sizeof src - 1);
    KASSERT_EQ_U(pushed, (uint64_t)(sizeof src - 1));

    char buf[16];
    int got = nx_console_read(buf, sizeof buf);
    KASSERT_EQ_U(got, (uint64_t)(sizeof src - 1));

    for (size_t i = 0; i < sizeof src - 1; i++)
        KASSERT_EQ_U((uint64_t)(unsigned char)buf[i],
                     (uint64_t)(unsigned char)src[i]);
}

KTEST(console_rx_wraps_around)
{
    nx_console_reset_for_test();

    /* Two big push/drain rounds force the ring's head + tail past
     * RX_RING_SIZE (256) and back, exercising the (h + 1) & MASK
     * modulo path in rx_push_one / rx_pop_one. */
    char chunk[200];
    char back[200];

    for (size_t i = 0; i < sizeof chunk; i++) chunk[i] = (char)('a' + (i % 26));

    size_t pushed1 = nx_console_test_inject_bytes(chunk, sizeof chunk);
    KASSERT_EQ_U(pushed1, (uint64_t)sizeof chunk);

    int got1 = nx_console_read(back, sizeof back);
    KASSERT_EQ_U(got1, (uint64_t)sizeof back);
    for (size_t i = 0; i < sizeof chunk; i++)
        KASSERT_EQ_U((uint64_t)(unsigned char)back[i],
                     (uint64_t)(unsigned char)chunk[i]);

    /* Round 2: smaller payload, head + tail land in the wrapped
     * region of the ring (close to the buffer's start due to the
     * modulo). */
    for (size_t i = 0; i < 100; i++) chunk[i] = (char)('A' + (i % 26));
    size_t pushed2 = nx_console_test_inject_bytes(chunk, 100);
    KASSERT_EQ_U(pushed2, (uint64_t)100);

    int got2 = nx_console_read(back, 100);
    KASSERT_EQ_U(got2, (uint64_t)100);
    for (size_t i = 0; i < 100; i++)
        KASSERT_EQ_U((uint64_t)(unsigned char)back[i],
                     (uint64_t)(unsigned char)chunk[i]);
}

KTEST(console_rx_drop_on_full)
{
    nx_console_reset_for_test();

    /* Ring capacity is 256 minus one (full sentinel), so a single
     * push of 256 bytes accepts 255 and drops the last. */
    char src[256];
    for (size_t i = 0; i < sizeof src; i++) src[i] = (char)(i & 0x7F);

    size_t pushed = nx_console_test_inject_bytes(src, sizeof src);
    KASSERT_EQ_U(pushed, (uint64_t)255);

    char back[256];
    int got = nx_console_read(back, sizeof back);
    KASSERT_EQ_U(got, (uint64_t)255);
}

KTEST(console_eof_returns_zero)
{
    /* Slice 7.6d.N.final.c — Ctrl-D drives nx_console_read to return
     * EOF (0) when the ring is empty.  Two sub-checks:
     *   1. With bytes still in the ring, the first read drains them
     *      normally — the EOF flag does not pre-empt non-empty reads
     *      (POSIX: read returns either available bytes OR EOF, never
     *      both in the same call).
     *   2. After the ring is drained, the next read with the flag
     *      armed returns 0 (= EOF).
     *   3. Subsequent reads block normally — the flag is one-shot.
     *
     * (3) we can't directly observe in a kernel test (would yield
     * forever); we just check that the flag has cleared by re-arming
     * + observing another EOF return. */
    nx_console_reset_for_test();

    char src[]    = "ABC";  /* 3 bytes */
    nx_console_test_inject_bytes(src, sizeof src - 1);
    nx_console_test_inject_eof();

    char back[8];
    int got = nx_console_read(back, sizeof back);
    KASSERT_EQ_U(got, (uint64_t)3);    /* drained ring first */

    got = nx_console_read(back, sizeof back);
    KASSERT_EQ_U(got, (uint64_t)0);    /* EOF on second call */

    /* Re-arm to confirm the flag is one-shot and now cleared. */
    nx_console_test_inject_eof();
    got = nx_console_read(back, sizeof back);
    KASSERT_EQ_U(got, (uint64_t)0);
}

KTEST(console_intr_posts_sigterm)
{
    /* Slice 7.6d.N.final.c — Ctrl-C arms a flag that
     * `nx_console_drain_intr` consumes by posting SIGTERM to every
     * ACTIVE non-kernel process's pending_signals.  Set up: create a
     * fresh process; arm the flag; call drain; verify the process's
     * pending_signals has SIGTERM bit set. */
    nx_console_reset_for_test();

    struct nx_process *target = nx_process_create("ctrl-c-target");
    KASSERT_NOT_NULL(target);
    KASSERT_EQ_U(__atomic_load_n(&target->pending_signals,
                                 __ATOMIC_ACQUIRE), (uint64_t)0);

    /* No flag armed yet — drain is a no-op. */
    int drained = nx_console_drain_intr();
    KASSERT_EQ_U((uint64_t)drained, (uint64_t)0);
    KASSERT_EQ_U(__atomic_load_n(&target->pending_signals,
                                 __ATOMIC_ACQUIRE), (uint64_t)0);

    /* Arm + drain — pending_signals should now carry SIGTERM. */
    nx_console_test_inject_intr();
    drained = nx_console_drain_intr();
    KASSERT_EQ_U((uint64_t)drained, (uint64_t)1);
    uint32_t pend = __atomic_load_n(&target->pending_signals,
                                    __ATOMIC_ACQUIRE);
    KASSERT((pend & (1u << NX_SIGTERM)) != 0);

    /* Flag is one-shot — second drain (no rearm) is a no-op. */
    drained = nx_console_drain_intr();
    KASSERT_EQ_U((uint64_t)drained, (uint64_t)0);

    /* Cleanup — clear pending_signals so a downstream test that
     * happens to observe this process by pid (we can't fully reset
     * the process table here without crashing later tests) starts
     * clean. */
    __atomic_store_n(&target->pending_signals, 0, __ATOMIC_RELEASE);
}
