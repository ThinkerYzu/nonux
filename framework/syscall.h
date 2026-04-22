#ifndef NX_FRAMEWORK_SYSCALL_H
#define NX_FRAMEWORK_SYSCALL_H

#include "framework/handle.h"
#include "framework/registry.h"

#include <stdint.h>

/*
 * Syscall framework — Phase 5 slice 5.4.
 *
 * v1 wires up the SVC-from-EL1 path only — tests issue `svc #0` from
 * kernel code to exercise the dispatch plumbing before slice 5.5 drops
 * to EL0.  Calling convention follows the Linux AArch64 ABI so later
 * EL0 consumers need no adapter:
 *
 *   x8      syscall number (enum nx_syscall_number)
 *   x0..x5  arguments
 *   x0      return value (sign-extended nx_status_t)
 *
 * Unknown syscall numbers and invalid argument pointers return
 * NX_EINVAL; handle operations pass through to the current owner's
 * handle table (for slice 5.4 that's a single process-agnostic global
 * table; slice 5.5 moves it per-task).
 */

/* Signed 64-bit status so negative NX_E* codes survive the x0 trip
 * through register storage without ambiguity.  Non-negative returns are
 * op-defined (bytes written, handle value, etc). */
typedef int64_t nx_status_t;

/* Syscall numbers.  Gaps are reserved for later growth without
 * renumbering already-shipped calls — this enum is effectively a stable
 * ABI surface from slice 5.5 onward. */
enum nx_syscall_number {
    NX_SYS_RESERVED_0     = 0,   /* reserved: SVC #0 with x8 == 0 should
                                  * be a clear "syscall number not set"
                                  * error rather than some real op. */
    NX_SYS_DEBUG_WRITE    = 1,   /* (const char *buf, size_t len) → bytes written */
    NX_SYS_HANDLE_CLOSE   = 2,   /* (nx_handle_t h)               → NX_OK / NX_E* */

    NX_SYSCALL_COUNT,            /* sentinel — keep last */
};

/*
 * Dispatch entry — invoked from `on_sync` after it has decoded
 * ESR_EL1.EC as SVC (0x15).  `tf->x[8]` holds the syscall number; args
 * live in `tf->x[0..5]`.  The return value is written into `tf->x[0]`
 * so it lands in x0 when the exception handler does `eret`.
 *
 * Safe to call from EL1 sync-exception context with preemption
 * implicitly disabled (DAIF-I masked by the hardware on exception
 * entry).  The dispatch table itself is `const`, so no locking needed.
 */
struct trap_frame;
void nx_syscall_dispatch(struct trap_frame *tf);

/*
 * Current owner's handle table.  Slice 5.4 returns a single global
 * kernel-wide table — there's no "process" concept yet.  Slice 5.5
 * replaces the body with `&nx_task_current()->handle_table` so each
 * task (later: each process) has its own handle namespace.
 *
 * Exposed in the header so kernel tests can seed + inspect the table
 * before/after issuing SVCs.  Userspace never calls this directly;
 * it's a kernel-internal helper that the syscall bodies consume.
 */
struct nx_handle_table *nx_syscall_current_table(void);

/*
 * Test-only: clear the kernel handle table.  Called by kernel tests
 * between cases so one test's leftover handles don't affect the
 * next.  No-op safe.
 */
void nx_syscall_reset_for_test(void);

#endif /* NX_FRAMEWORK_SYSCALL_H */
