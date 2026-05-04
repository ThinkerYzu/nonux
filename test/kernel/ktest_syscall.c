#include "ktest.h"
#include "framework/handle.h"
#include "framework/hook.h"
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

KTEST(syscall_handle_close_zero_handle_with_no_stdin_returns_enoent)
{
    /* Slice 7.6d.N.6b: handle value 0 (= NX_HANDLE_INVALID encoded form,
     * = POSIX STDIN_FILENO) is now routed to slot 2 — the pre-installed
     * CONSOLE STDIN slot in user processes.  Inside ktest the current
     * process is g_kernel_process, which doesn't pre-install console
     * handles, so slot 2 is empty and the close returns NX_ENOENT.
     * (Pre-7.6d.N.6b this returned NX_EINVAL because h=0 had no
     * encoded form; the rename + value rotation reflect the new
     * lookup-then-close semantic.) */
    nx_syscall_reset_for_test();
    int64_t rc = svc1(NX_SYS_HANDLE_CLOSE, NX_HANDLE_INVALID);
    KASSERT_EQ_U((uint64_t)rc, (uint64_t)(int64_t)NX_ENOENT);
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

/* ---- Slice 8.7: NX_HOOK_SYSCALL_ENTER / _EXIT hook points ----------- */

static volatile uint64_t g_hook_num;
static volatile uint64_t g_hook_a0;
static volatile int64_t  g_hook_rc_on_exit;
static volatile int      g_enter_fires;
static volatile int      g_exit_fires;

static enum nx_hook_action sh_enter_observe(struct nx_hook_context *ctx,
                                             void *user)
{
    (void)user;
    g_hook_num     = ctx->u.sc.num;
    g_hook_a0      = ctx->u.sc.a[0];
    g_enter_fires++;
    return NX_HOOK_CONTINUE;
}

static enum nx_hook_action sh_exit_observe(struct nx_hook_context *ctx,
                                            void *user)
{
    (void)user;
    g_hook_rc_on_exit = *ctx->u.sc.rc;
    g_exit_fires++;
    return NX_HOOK_CONTINUE;
}

static enum nx_hook_action sh_enter_abort(struct nx_hook_context *ctx,
                                           void *user)
{
    (void)user;
    *ctx->u.sc.rc = (int64_t)(intptr_t)user;
    return NX_HOOK_ABORT;
}

static enum nx_hook_action sh_exit_override(struct nx_hook_context *ctx,
                                             void *user)
{
    (void)user;
    *ctx->u.sc.rc = 0x7EEF;
    return NX_HOOK_CONTINUE;
}

KTEST(syscall_hook_enter_fires)
{
    nx_syscall_reset_for_test();
    g_enter_fires = 0;
    g_hook_num = 0;
    g_hook_a0  = 0;

    static struct nx_hook h = { .point = NX_HOOK_SYSCALL_ENTER,
                                 .fn    = sh_enter_observe };
    nx_hook_register(&h);

    static const char msg[] = "hook-enter\n";
    svc2(NX_SYS_DEBUG_WRITE, (uint64_t)(uintptr_t)msg, sizeof msg - 1);

    KASSERT_EQ_U(g_enter_fires, 1);
    KASSERT_EQ_U(g_hook_num,    NX_SYS_DEBUG_WRITE);
    KASSERT_EQ_U(g_hook_a0,     (uint64_t)(uintptr_t)msg);

    nx_hook_unregister(&h);
}

KTEST(syscall_hook_exit_fires)
{
    nx_syscall_reset_for_test();
    g_exit_fires      = 0;
    g_hook_rc_on_exit = 0;

    static struct nx_hook h = { .point = NX_HOOK_SYSCALL_EXIT,
                                 .fn    = sh_exit_observe };
    nx_hook_register(&h);

    static const char msg[] = "hook-exit\n";
    int64_t svc_rc = svc2(NX_SYS_DEBUG_WRITE,
                          (uint64_t)(uintptr_t)msg, sizeof msg - 1);

    KASSERT_EQ_U(g_exit_fires, 1);
    /* EXIT hook saw the body's return value (bytes written). */
    KASSERT_EQ_U((uint64_t)g_hook_rc_on_exit, (uint64_t)svc_rc);

    nx_hook_unregister(&h);
}

KTEST(syscall_hook_enter_abort_skips_body)
{
    nx_syscall_reset_for_test();
    uint64_t calls_before = nx_syscall_debug_write_calls();

    /* ENTER hook sets *rc = NX_ENOENT and aborts — body must not run. */
    static struct nx_hook h = { .point = NX_HOOK_SYSCALL_ENTER,
                                 .fn    = sh_enter_abort,
                                 .user  = (void *)(intptr_t)NX_ENOENT };
    nx_hook_register(&h);

    static const char msg[] = "should-not-print\n";
    int64_t rc = svc2(NX_SYS_DEBUG_WRITE,
                      (uint64_t)(uintptr_t)msg, sizeof msg - 1);

    /* Syscall body skipped — debug_write call counter must not move. */
    KASSERT_EQ_U(nx_syscall_debug_write_calls(), calls_before);
    /* Return value is what the hook placed in *rc. */
    KASSERT_EQ_U((uint64_t)rc, (uint64_t)(int64_t)NX_ENOENT);

    nx_hook_unregister(&h);
}

KTEST(syscall_hook_exit_can_override_result)
{
    nx_syscall_reset_for_test();

    static struct nx_hook h = { .point = NX_HOOK_SYSCALL_EXIT,
                                 .fn    = sh_exit_override };
    nx_hook_register(&h);

    /* DEBUG_WRITE normally returns bytes written; hook overrides to 0x7EEF. */
    static const char msg[] = "override\n";
    int64_t rc = svc2(NX_SYS_DEBUG_WRITE,
                      (uint64_t)(uintptr_t)msg, sizeof msg - 1);

    KASSERT_EQ_U((uint64_t)rc, 0x7EEF);

    nx_hook_unregister(&h);
}

KTEST(syscall_hook_enter_and_exit_both_fire)
{
    nx_syscall_reset_for_test();
    g_enter_fires = 0;
    g_exit_fires  = 0;

    static struct nx_hook he = { .point = NX_HOOK_SYSCALL_ENTER,
                                  .fn    = sh_enter_observe };
    static struct nx_hook hx = { .point = NX_HOOK_SYSCALL_EXIT,
                                  .fn    = sh_exit_observe };
    nx_hook_register(&he);
    nx_hook_register(&hx);

    svc2(NX_SYS_DEBUG_WRITE, 0, 0);

    KASSERT_EQ_U(g_enter_fires, 1);
    KASSERT_EQ_U(g_exit_fires,  1);

    nx_hook_unregister(&he);
    nx_hook_unregister(&hx);
}
