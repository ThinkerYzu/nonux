#include "ktest.h"
#include "framework/handle.h"
#include "framework/syscall.h"

/*
 * Kernel-side coverage for slice 5.4.
 *
 * Each test issues `svc #0` from EL1 kernel code, the sync-exception
 * handler dispatches via `nx_syscall_dispatch`, and the test verifies
 * the return-value + observable side effects.  No EL0 involvement yet
 * — slice 5.5 adds the drop-to-EL0 path; the handler is the same.
 *
 * Calling convention (matches Linux AArch64 ABI so slice 5.5 EL0
 * userspace issues the same form):
 *
 *   x8       syscall number
 *   x0..x5   args
 *   x0       return value
 */

/* ---- SVC wrappers ---------------------------------------------------- *
 *
 * Inline asm that pins x8 to the syscall number, pins x0..x1 to the
 * args, issues `svc #0`, and extracts x0 as the nx_status_t result.
 * Keeping these in the test file (not the framework) because production
 * kernel code shouldn't issue SVCs — userspace does that, kernel code
 * calls the syscall body directly.  These wrappers exist purely to
 * exercise the dispatch plumbing.
 */

static inline int64_t svc0(uint64_t num)
{
    register int64_t  x0 asm("x0");
    register uint64_t x8 asm("x8") = num;
    asm volatile("svc #0"
                 : "=r"(x0)
                 : "r"(x8)
                 : "memory");
    return x0;
}

static inline int64_t svc1(uint64_t num, uint64_t a0)
{
    register int64_t  x0 asm("x0") = (int64_t)a0;
    register uint64_t x8 asm("x8") = num;
    asm volatile("svc #0"
                 : "+r"(x0)
                 : "r"(x8)
                 : "memory");
    return x0;
}

static inline int64_t svc2(uint64_t num, uint64_t a0, uint64_t a1)
{
    register int64_t  x0 asm("x0") = (int64_t)a0;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x8 asm("x8") = num;
    asm volatile("svc #0"
                 : "+r"(x0)
                 : "r"(x1), "r"(x8)
                 : "memory");
    return x0;
}

/* ---- Tests ---------------------------------------------------------- */

KTEST(syscall_unknown_number_returns_enosys)
{
    nx_syscall_reset_for_test();
    /* Reserved slot 0 and out-of-range values both reject with
     * NX_ENOSYS — proves the dispatcher validates against the table
     * bounds before reading a function pointer.  Linux-compat value
     * (-38) so musl surfaces unmapped syscalls as `errno = ENOSYS`
     * instead of `EPERM` (which is what NX_EINVAL = -1 collides with). */
    KASSERT_EQ_U((uint64_t)svc0(0),      (uint64_t)(int64_t)NX_ENOSYS);
    KASSERT_EQ_U((uint64_t)svc0(9999),   (uint64_t)(int64_t)NX_ENOSYS);
    KASSERT_EQ_U((uint64_t)svc0(NX_SYSCALL_COUNT),
                 (uint64_t)(int64_t)NX_ENOSYS);
}

KTEST(syscall_debug_write_returns_byte_count)
{
    nx_syscall_reset_for_test();
    /* Short message; round-trip through the dispatcher and back.
     * The UART side effect is observable in the test-output log but
     * not something KASSERT can verify directly — return-value check
     * proves the dispatch happened and the handler ran. */
    static const char msg[] = "[ktest] svc debug_write\n";
    int64_t rc = svc2(NX_SYS_DEBUG_WRITE,
                      (uint64_t)(uintptr_t)msg,
                      sizeof msg - 1);
    KASSERT_EQ_U((uint64_t)rc, (uint64_t)(sizeof msg - 1));
}

KTEST(syscall_debug_write_zero_length_returns_zero)
{
    nx_syscall_reset_for_test();
    int64_t rc = svc2(NX_SYS_DEBUG_WRITE, 0, 0);
    /* NULL + len==0 is explicitly OK: no bytes requested, no bytes
     * written.  Matches Linux write(2) semantics. */
    KASSERT_EQ_U((uint64_t)rc, 0);
}

KTEST(syscall_debug_write_null_buf_nonzero_len_returns_einval)
{
    nx_syscall_reset_for_test();
    int64_t rc = svc2(NX_SYS_DEBUG_WRITE, 0, 8);
    KASSERT_EQ_U((uint64_t)rc, (uint64_t)(int64_t)NX_EINVAL);
}

KTEST(syscall_handle_close_through_svc_closes_handle_in_kernel_table)
{
    nx_syscall_reset_for_test();
    struct nx_handle_table *t = nx_syscall_current_table();

    /* Slice 5.6: `sys_handle_close` now calls `nx_channel_endpoint_close`
     * on objects stored under NX_HANDLE_CHANNEL.  Use NX_HANDLE_VMO
     * here so the dummy-pointer placeholder doesn't hit the channel
     * destructor. */
    static int dummy;
    nx_handle_t h;
    KASSERT_EQ_U(nx_handle_alloc(t, NX_HANDLE_VMO,
                                 NX_RIGHT_READ | NX_RIGHT_WRITE,
                                 &dummy, &h), NX_OK);

    /* Close via the SVC path, then verify the slot is gone via the
     * direct handle lookup.  This is end-to-end proof that the
     * dispatcher passed the argument through unchanged AND that the
     * syscall body touched the right table. */
    int64_t rc = svc1(NX_SYS_HANDLE_CLOSE, h);
    KASSERT_EQ_U((uint64_t)rc, (uint64_t)NX_OK);
    KASSERT_EQ_U(nx_handle_lookup(t, h, 0, 0, 0), NX_ENOENT);
}

KTEST(syscall_handle_close_invalid_handle_returns_einval)
{
    nx_syscall_reset_for_test();
    int64_t rc = svc1(NX_SYS_HANDLE_CLOSE, NX_HANDLE_INVALID);
    KASSERT_EQ_U((uint64_t)rc, (uint64_t)(int64_t)NX_EINVAL);
}

KTEST(syscall_resumes_at_instruction_after_svc)
{
    /* After SVC returns, execution must continue at the instruction
     * after `svc #0` with PSTATE restored.  If ELR_EL1 / SPSR_EL1 save
     * or restore is wrong, either we never come back, or we re-execute
     * the SVC.  Both would fail the equality check or hang the test. */
    nx_syscall_reset_for_test();
    volatile int sentinel = 0xA5;
    svc2(NX_SYS_DEBUG_WRITE, 0, 0);   /* round-trip, no bytes */
    KASSERT_EQ_U(sentinel, 0xA5);
}
