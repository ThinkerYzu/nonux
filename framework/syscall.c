#include "framework/syscall.h"
#include "framework/handle.h"
#include "framework/registry.h"

#include <stddef.h>
#include <stdint.h>

#if !__STDC_HOSTED__
#include "core/lib/lib.h"              /* uart_putc */
#include "core/cpu/exception.h"        /* struct trap_frame */
#else
/* Host build: core/cpu/exception.h contains ARM64 inline asm, which
 * won't compile on x86.  Mirror the trap_frame layout locally so host
 * dispatch tests can construct synthetic frames without depending on
 * the CPU header. */
struct trap_frame {
    uint64_t x[31];
    uint64_t sp_el0;
    uint64_t pc;
    uint64_t pstate;
};
#endif

/*
 * Syscall dispatch — slice 5.4.
 *
 * One static dispatch table keyed by syscall number.  Each entry takes
 * the six AArch64-ABI argument registers (x0..x5) as raw uint64_t and
 * returns an nx_status_t; each handler is responsible for type-
 * converting its own args.  A NULL slot is reserved (NX_SYS_RESERVED_0
 * at number 0) so stray `svc #0` with x8 unset gets a crisp NX_EINVAL
 * rather than silently invoking op 0.
 *
 * The table is const, so dispatch is lock-free by construction.  The
 * handlers themselves may touch shared state (the global handle table,
 * the UART) — those touch points manage their own invariants.
 */

typedef nx_status_t (*syscall_fn)(uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t, uint64_t);

/* ---------- Current handle table -------------------------------------- *
 *
 * Slice 5.4 has no per-process concept yet, so the dispatch operates
 * against a single static table.  Slice 5.5 rewrites
 * `nx_syscall_current_table()` to return `&nx_task_current()->
 * handle_table` — every other call site stays unchanged.
 */

static struct nx_handle_table g_kernel_handles;

/* Slice 5.5: test-observable counter of successful debug_write calls.
 * Bumped inside the syscall body on the happy path; tests read via
 * nx_syscall_debug_write_calls() to confirm EL0 code reached the SVC
 * without relying on UART output capture. */
static _Atomic uint64_t g_debug_write_calls;

struct nx_handle_table *nx_syscall_current_table(void)
{
    return &g_kernel_handles;
}

void nx_syscall_reset_for_test(void)
{
    nx_handle_table_init(&g_kernel_handles);
    __atomic_store_n(&g_debug_write_calls, 0, __ATOMIC_RELAXED);
}

uint64_t nx_syscall_debug_write_calls(void)
{
    return __atomic_load_n(&g_debug_write_calls, __ATOMIC_RELAXED);
}

/* ---------- Syscall bodies ------------------------------------------- */

/*
 * NX_SYS_DEBUG_WRITE — (const char *buf, size_t len).
 *
 * Writes `len` bytes from `buf` to the kernel UART.  Returns the byte
 * count on success (always == len in v1; partial writes arrive when
 * flow control is added).  NX_EINVAL on NULL buf, NX_OK-but-zero on
 * zero len.
 *
 * On host there is no UART — the call fails cleanly with NX_EINVAL so
 * the slice 5.4 plumbing can still link under the host test harness
 * (a host test could mock it if needed; kernel tests exercise the real
 * path).
 */
static nx_status_t sys_debug_write(uint64_t a0, uint64_t a1,
                                   uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    const char *buf = (const char *)(uintptr_t)a0;
    size_t      len = (size_t)a1;
    if (!buf && len > 0) return NX_EINVAL;

#if !__STDC_HOSTED__
    for (size_t i = 0; i < len; i++)
        uart_putc(buf[i]);
#endif
    /* Slice 5.5: bump the test-observable counter on the happy path
     * only.  Atomic because EL0 and kernel-test tasks can both call
     * debug_write under preemption; no correctness rides on this
     * beyond "counter monotonically increases once per successful
     * call", which __ATOMIC_RELAXED delivers. */
    __atomic_fetch_add(&g_debug_write_calls, 1, __ATOMIC_RELAXED);
    return (nx_status_t)len;
}

/*
 * NX_SYS_HANDLE_CLOSE — (nx_handle_t h).
 *
 * Closes a handle in the caller's table.  Returns NX_OK on success,
 * NX_ENOENT if the handle is already closed / stale, NX_EINVAL for a
 * malformed handle value.  Matches the direct `nx_handle_close`
 * semantics — the syscall is just a thin wrapper that picks up the
 * caller's table via `nx_syscall_current_table`.
 */
static nx_status_t sys_handle_close(uint64_t a0, uint64_t a1,
                                    uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    nx_handle_t h = (nx_handle_t)a0;
    return nx_handle_close(nx_syscall_current_table(), h);
}

/* ---------- Dispatch table ------------------------------------------- */

static const syscall_fn g_syscall_table[NX_SYSCALL_COUNT] = {
    [NX_SYS_RESERVED_0]   = NULL,             /* caught below → NX_EINVAL */
    [NX_SYS_DEBUG_WRITE]  = sys_debug_write,
    [NX_SYS_HANDLE_CLOSE] = sys_handle_close,
};

/* ---------- Entry point ---------------------------------------------- */

void nx_syscall_dispatch(struct trap_frame *tf)
{
    if (!tf) return;

    uint64_t num = tf->x[8];
    nx_status_t rc;

    if (num >= NX_SYSCALL_COUNT || g_syscall_table[num] == NULL) {
        rc = NX_EINVAL;
    } else {
        rc = g_syscall_table[num](tf->x[0], tf->x[1], tf->x[2],
                                  tf->x[3], tf->x[4], tf->x[5]);
    }
    tf->x[0] = (uint64_t)rc;
}
