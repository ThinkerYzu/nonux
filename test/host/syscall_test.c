/*
 * Host-side tests for the syscall dispatcher (slice 5.4).
 *
 * No SVC on host — we feed synthetic trap frames directly into
 * `nx_syscall_dispatch` and inspect `tf->x[0]` after the call.
 * Exercises the dispatch table, number validation, and arg-pass-
 * through; the SVC-triggered side of the plumbing is covered in
 * test/kernel/ktest_syscall.c.
 */

#include "test_runner.h"

#include "framework/handle.h"
#include "framework/registry.h"
#include "framework/syscall.h"

#include <string.h>

/* Mirror of the trap_frame layout — same definition as the host branch
 * in framework/syscall.c.  Kept private to the test so a change in
 * framework/syscall.c surfaces as a linker / layout failure rather
 * than silent drift. */
struct trap_frame_host {
    uint64_t x[31];
    uint64_t sp_el0;
    uint64_t pc;
    uint64_t pstate;
};

/* framework/syscall.h forward-declares `struct trap_frame`; host build
 * of framework/syscall.c defines its own layout locally.  Our test-side
 * mirror above has the same layout — we pass our frame through with a
 * plain cast. */
#define CALL_DISPATCH(tf) \
    nx_syscall_dispatch((struct trap_frame *)(tf))

static void reset_frame(struct trap_frame_host *tf)
{
    memset(tf, 0, sizeof *tf);
}

/* --- dispatch table validation -------------------------------------- */

TEST(syscall_unknown_number_returns_einval_on_host)
{
    nx_syscall_reset_for_test();
    struct trap_frame_host tf;

    /* Reserved slot 0. */
    reset_frame(&tf);
    tf.x[8] = 0;
    CALL_DISPATCH(&tf);
    ASSERT_EQ_U(tf.x[0], (uint64_t)(int64_t)NX_EINVAL);

    /* Out-of-range high. */
    reset_frame(&tf);
    tf.x[8] = 9999;
    CALL_DISPATCH(&tf);
    ASSERT_EQ_U(tf.x[0], (uint64_t)(int64_t)NX_EINVAL);

    /* Exactly at sentinel. */
    reset_frame(&tf);
    tf.x[8] = NX_SYSCALL_COUNT;
    CALL_DISPATCH(&tf);
    ASSERT_EQ_U(tf.x[0], (uint64_t)(int64_t)NX_EINVAL);
}

TEST(syscall_null_trap_frame_is_noop)
{
    /* A NULL tf must not crash — defensive guard in the dispatcher.
     * Equivalent of calling dispatch with a garbage pointer that was
     * never populated; we just want "doesn't segfault" here. */
    nx_syscall_reset_for_test();
    CALL_DISPATCH(NULL);
}

/* --- NX_SYS_DEBUG_WRITE arg decoding -------------------------------- */

TEST(syscall_debug_write_returns_len_on_host)
{
    nx_syscall_reset_for_test();
    struct trap_frame_host tf;
    reset_frame(&tf);

    static const char msg[] = "hello syscall";
    tf.x[0] = (uint64_t)(uintptr_t)msg;
    tf.x[1] = sizeof msg - 1;
    tf.x[8] = NX_SYS_DEBUG_WRITE;

    CALL_DISPATCH(&tf);
    /* Host build's debug_write body doesn't touch a UART but still
     * reports len — proof that x0 + x1 reached the handler unaltered. */
    ASSERT_EQ_U(tf.x[0], (uint64_t)(sizeof msg - 1));
}

TEST(syscall_debug_write_null_nonzero_len_rejects_on_host)
{
    nx_syscall_reset_for_test();
    struct trap_frame_host tf;
    reset_frame(&tf);

    tf.x[0] = 0;
    tf.x[1] = 8;
    tf.x[8] = NX_SYS_DEBUG_WRITE;

    CALL_DISPATCH(&tf);
    ASSERT_EQ_U(tf.x[0], (uint64_t)(int64_t)NX_EINVAL);
}

/* --- NX_SYS_HANDLE_CLOSE pass-through ------------------------------- */

TEST(syscall_handle_close_routes_to_current_table)
{
    nx_syscall_reset_for_test();
    struct nx_handle_table *t = nx_syscall_current_table();

    /* Use NX_HANDLE_VMO (no object-side destructor yet) with a local-
     * int placeholder.  Slice 5.6 made `sys_handle_close` type-aware
     * for CHANNEL (calls `nx_channel_endpoint_close` on the object),
     * and passing a bare int as a "channel object" dereferences
     * garbage.  VMO stays an opaque type so the test exercises the
     * table-close path without tripping the channel destructor. */
    int dummy = 0;
    nx_handle_t h;
    int rc_alloc = nx_handle_alloc(t, NX_HANDLE_VMO, NX_RIGHT_READ,
                                   &dummy, &h);
    ASSERT_EQ_U(rc_alloc, NX_OK);

    struct trap_frame_host tf;
    reset_frame(&tf);
    tf.x[0] = h;
    tf.x[8] = NX_SYS_HANDLE_CLOSE;
    CALL_DISPATCH(&tf);
    ASSERT_EQ_U(tf.x[0], (uint64_t)NX_OK);

    /* Handle is really closed — lookup misses. */
    int rc_look = nx_handle_lookup(t, h, 0, 0, 0);
    ASSERT_EQ_U(rc_look, NX_ENOENT);
}

TEST(syscall_reset_for_test_clears_kernel_table)
{
    struct nx_handle_table *t = nx_syscall_current_table();
    int dummy = 0;
    nx_handle_t h;
    nx_handle_alloc(t, NX_HANDLE_CHANNEL, NX_RIGHT_READ, &dummy, &h);
    ASSERT_EQ_U(nx_handle_table_count(t), 1);

    nx_syscall_reset_for_test();
    ASSERT_EQ_U(nx_handle_table_count(t), 0);
}
