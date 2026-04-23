/*
 * Host-side tests for the channel framework (slice 5.6).
 *
 * Exercises the ring-buffer send/recv paths, the pair refcount +
 * lifetime story, and the error-return surface.  SVC-driven tests
 * of the channel syscalls live in test/kernel/ktest_channel.c.
 */

#include "test_runner.h"

#include "framework/channel.h"
#include "framework/registry.h"

#include <string.h>

/* --- Basic send/recv round-trip ------------------------------------- */

TEST(channel_create_returns_two_distinct_endpoints)
{
    struct nx_channel_endpoint *e0 = 0, *e1 = 0;
    int rc = nx_channel_create(&e0, &e1);
    ASSERT_EQ_U(rc, NX_OK);
    ASSERT_NOT_NULL(e0);
    ASSERT_NOT_NULL(e1);
    ASSERT(e0 != e1);

    nx_channel_endpoint_close(e0);
    nx_channel_endpoint_close(e1);
}

TEST(channel_send_on_a_arrives_on_b)
{
    struct nx_channel_endpoint *a = 0, *b = 0;
    nx_channel_create(&a, &b);

    const char payload[] = "hello";
    int sent = nx_channel_send(a, payload, sizeof payload);
    ASSERT_EQ_U((unsigned)sent, sizeof payload);

    /* b receives. */
    char buf[32];
    int got = nx_channel_recv(b, buf, sizeof buf);
    ASSERT_EQ_U((unsigned)got, sizeof payload);
    ASSERT(memcmp(buf, payload, sizeof payload) == 0);

    /* a's own inbox stays empty (the send went to b, not a). */
    char dummy[32];
    int rc = nx_channel_recv(a, dummy, sizeof dummy);
    ASSERT_EQ_U(rc, NX_EAGAIN);

    nx_channel_endpoint_close(a);
    nx_channel_endpoint_close(b);
}

TEST(channel_send_and_recv_are_fifo)
{
    struct nx_channel_endpoint *a = 0, *b = 0;
    nx_channel_create(&a, &b);

    for (int i = 0; i < 4; i++) {
        char m[2] = { (char)('0' + i), 0 };
        ASSERT_EQ_U((unsigned)nx_channel_send(a, m, 2), 2);
    }

    for (int i = 0; i < 4; i++) {
        char buf[8] = { 0 };
        int got = nx_channel_recv(b, buf, sizeof buf);
        ASSERT_EQ_U(got, 2);
        ASSERT_EQ_U((unsigned)buf[0], (unsigned)(char)('0' + i));
    }

    /* Empty again. */
    char dummy[8];
    ASSERT_EQ_U(nx_channel_recv(b, dummy, sizeof dummy), NX_EAGAIN);

    nx_channel_endpoint_close(a);
    nx_channel_endpoint_close(b);
}

/* --- Error-return surface ------------------------------------------- */

TEST(channel_recv_on_empty_returns_eagain)
{
    struct nx_channel_endpoint *a = 0, *b = 0;
    nx_channel_create(&a, &b);

    char buf[16];
    int rc = nx_channel_recv(a, buf, sizeof buf);
    ASSERT_EQ_U(rc, NX_EAGAIN);

    nx_channel_endpoint_close(a);
    nx_channel_endpoint_close(b);
}

TEST(channel_send_too_large_returns_einval)
{
    struct nx_channel_endpoint *a = 0, *b = 0;
    nx_channel_create(&a, &b);

    static char big[NX_CHANNEL_MSG_MAX + 1];
    int rc = nx_channel_send(a, big, sizeof big);
    ASSERT_EQ_U(rc, NX_EINVAL);

    nx_channel_endpoint_close(a);
    nx_channel_endpoint_close(b);
}

TEST(channel_send_full_ring_returns_ebusy)
{
    struct nx_channel_endpoint *a = 0, *b = 0;
    nx_channel_create(&a, &b);

    /* Ring holds NX_CHANNEL_RING_LEN - 1 messages due to the
     * circular-buffer head==tail means "empty" convention. */
    const char m[] = "x";
    size_t pushed = 0;
    while (nx_channel_send(a, m, sizeof m) > 0) pushed++;
    ASSERT_EQ_U(pushed, (unsigned)(NX_CHANNEL_RING_LEN - 1));

    int rc = nx_channel_send(a, m, sizeof m);
    ASSERT_EQ_U(rc, NX_EBUSY);

    /* After draining one, we can push one again. */
    char buf[8];
    ASSERT_EQ_U(nx_channel_recv(b, buf, sizeof buf), (int)sizeof m);
    ASSERT_EQ_U((unsigned)nx_channel_send(a, m, sizeof m), sizeof m);

    nx_channel_endpoint_close(a);
    nx_channel_endpoint_close(b);
}

TEST(channel_recv_with_cap_too_small_returns_enomem_and_keeps_message)
{
    struct nx_channel_endpoint *a = 0, *b = 0;
    nx_channel_create(&a, &b);

    const char payload[] = "longer than three";
    nx_channel_send(a, payload, sizeof payload);

    char small[3];
    int rc = nx_channel_recv(b, small, sizeof small);
    ASSERT_EQ_U(rc, NX_ENOMEM);

    /* Message must still be dequeueable with a proper-sized buffer. */
    char big[64];
    int got = nx_channel_recv(b, big, sizeof big);
    ASSERT_EQ_U((unsigned)got, sizeof payload);

    nx_channel_endpoint_close(a);
    nx_channel_endpoint_close(b);
}

/* --- Close semantics + refcount ------------------------------------- */

TEST(channel_send_after_peer_close_returns_ebusy)
{
    struct nx_channel_endpoint *a = 0, *b = 0;
    nx_channel_create(&a, &b);

    /* Close b first, then try to send on a.  The close decremented
     * refcount to 1; the channel allocation is still alive because
     * a still holds a reference.  Send must fail cleanly. */
    nx_channel_endpoint_close(b);
    ASSERT(nx_channel_endpoint_peer_closed(a));

    const char m[] = "orphan";
    int rc = nx_channel_send(a, m, sizeof m);
    ASSERT_EQ_U(rc, NX_EBUSY);

    /* Closing a too drops refcount to 0 and frees the channel.  Host
     * leak-detector will fail the test at exit if either endpoint's
     * allocation is leaked. */
    nx_channel_endpoint_close(a);
}

TEST(channel_double_close_on_same_endpoint_is_idempotent)
{
    struct nx_channel_endpoint *a = 0, *b = 0;
    nx_channel_create(&a, &b);

    nx_channel_endpoint_close(a);
    nx_channel_endpoint_close(a);   /* second close must be a no-op */
    nx_channel_endpoint_close(b);   /* still frees the channel */
}

TEST(channel_endpoint_depth_tracks_pending_messages)
{
    struct nx_channel_endpoint *a = 0, *b = 0;
    nx_channel_create(&a, &b);

    ASSERT_EQ_U(nx_channel_endpoint_depth(b), 0);

    nx_channel_send(a, "x", 1);
    nx_channel_send(a, "y", 1);
    nx_channel_send(a, "z", 1);
    ASSERT_EQ_U(nx_channel_endpoint_depth(b), 3);

    char buf[4];
    nx_channel_recv(b, buf, sizeof buf);
    ASSERT_EQ_U(nx_channel_endpoint_depth(b), 2);

    nx_channel_endpoint_close(a);
    nx_channel_endpoint_close(b);
}

TEST(channel_null_args_rejected_cleanly)
{
    ASSERT_EQ_U(nx_channel_create(0, 0), NX_EINVAL);

    struct nx_channel_endpoint *a = 0, *b = 0;
    nx_channel_create(&a, &b);

    char buf[4];
    ASSERT_EQ_U(nx_channel_send(0, "x", 1), NX_EINVAL);
    ASSERT_EQ_U(nx_channel_send(a, 0, 1), NX_EINVAL);
    ASSERT_EQ_U(nx_channel_recv(0, buf, sizeof buf), NX_EINVAL);
    ASSERT_EQ_U(nx_channel_recv(a, 0, sizeof buf), NX_EINVAL);
    ASSERT_EQ_U(nx_channel_recv(a, buf, 0), NX_EINVAL);

    /* NULL close is a no-op, not a crash. */
    nx_channel_endpoint_close(0);

    nx_channel_endpoint_close(a);
    nx_channel_endpoint_close(b);
}
