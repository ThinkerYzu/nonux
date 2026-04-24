#ifndef NX_FRAMEWORK_PROCESS_H
#define NX_FRAMEWORK_PROCESS_H

#include "framework/handle.h"
#include "framework/registry.h"

#include <stdint.h>

/*
 * Process framework — Phase 7 slice 7.1.
 *
 * A process is the owner of a handle table.  Slice 7.1 stops short of
 * address-space isolation (all processes still share TTBR0; slice 7.2
 * adds per-process TTBR0) and of blocking wait (slice 7.4), so what
 * this slice delivers is:
 *
 *   - A `struct nx_process` with a unique pid, a private
 *     `nx_handle_table`, a lifecycle state, and an exit_code slot.
 *   - `nx_process_create(name)` allocates a fresh process with an
 *     empty handle table.  `nx_process_destroy(p)` tears it down.
 *   - Every task now carries a `struct nx_process *process` pointer;
 *     `nx_task_create` inherits from the caller's current process
 *     (falling back to the always-present `g_kernel_process` during
 *     bootstrap).
 *   - The syscall dispatcher reads the current task's process's
 *     handle table rather than a process-agnostic global.
 *   - `NX_SYS_EXIT(code)` marks the caller's process EXITED and parks
 *     the task in a `wfe` loop.  External dequeue (the ktest harness)
 *     reclaims the task.  Real `wait()` lands in slice 7.4.
 *
 * PID allocation.
 *   PIDs are monotonic from 1, allocated by the `nx_pid_next` counter.
 *   PID 0 is reserved for `g_kernel_process` — the always-present
 *   process that covers the idle task and any framework kthread
 *   spawned before user-process creation.  Reuse of a freed PID is
 *   not attempted in v1 (the counter grows monotonically); a 32-bit
 *   wraparound would take billions of fork/exec cycles.
 */

enum nx_process_state {
    NX_PROCESS_STATE_ACTIVE = 0,
    NX_PROCESS_STATE_EXITED = 1,
};

#define NX_PROCESS_NAME_MAX  16

struct nx_process {
    uint32_t                pid;
    char                    name[NX_PROCESS_NAME_MAX];
    enum nx_process_state   state;
    int                     exit_code;
    struct nx_handle_table  handles;
    /*
     * TTBR0 root (slice 7.2).  Physical address of the L1 page table
     * backing this process's user-VA space — or the always-present
     * kernel root when the field carries `mmu_kernel_address_space()`.
     * Zero on host builds (no MMU); zero also means "no per-process
     * address space allocated" on kernel, in which case context-
     * switching to this process leaves TTBR0 unchanged.
     */
    uint64_t                ttbr0_root;
};

/*
 * The always-present kernel process (pid 0).  Used as the fallback
 * for `nx_process_current` when there's no scheduled task yet (host
 * test startup, kernel bootstrap before `sched_start`) and as the
 * initial owner of every kthread spawned before someone explicitly
 * reassigns `task->process`.  Defined as a weak tentative in the
 * header so every translation unit that includes this can reference
 * the symbol; the single authoritative definition lives in
 * `framework/process.c`.
 */
extern struct nx_process g_kernel_process;

/* ---------- API ------------------------------------------------------ */

/*
 * Allocate and return a fresh process with a private handle table.
 * `name` is copied (null-safe; NULL becomes the empty string).
 *
 * Returns NULL on allocation failure.  Caller owns the pointer and
 * must eventually `nx_process_destroy` it; the framework doesn't GC
 * processes in v1.
 */
struct nx_process *nx_process_create(const char *name);

/*
 * Tear down a process allocated by `nx_process_create`.  Closes every
 * handle still in the process's table (running any type-aware
 * destructors) and frees the storage.  Idempotent against NULL and
 * against `&g_kernel_process` (the latter is statically allocated and
 * can't be freed, but its handle table gets cleared — same semantics
 * as the old `nx_syscall_reset_for_test` against the retired global).
 */
void nx_process_destroy(struct nx_process *p);

/*
 * Return the current task's process, or `&g_kernel_process` when
 * there's no current task (bootstrap / host-test start-up).  Never
 * returns NULL.
 */
struct nx_process *nx_process_current(void);

/*
 * Find a process by pid.  Returns NULL if no process with that pid
 * exists (including the PID-0 kernel process, which can be looked up
 * but is never destroyed).  Linear scan; v1 has few processes so a
 * faster index isn't warranted.
 */
struct nx_process *nx_process_lookup_by_pid(uint32_t pid);

/*
 * Count of currently-registered processes (including the kernel
 * process).  Test helper — production code has no reason to care.
 */
size_t nx_process_count(void);

/*
 * Mark the current process EXITED with the given code and park the
 * current task in a `wfe` loop.  Never returns.  In v1 the task stays
 * on the scheduler runqueue and gets cycled uselessly on every pick;
 * the ktest harness calls `ops->dequeue` after observing the stored
 * exit_code to stop cycling.  A real `wait()` that handles this
 * inline lands in slice 7.4.
 */
void nx_process_exit(int code) __attribute__((noreturn));

/*
 * Host-test helper: reset process bookkeeping to a clean state.
 * Destroys every process created by `nx_process_create` (but not the
 * static kernel process — its handle table is merely cleared) and
 * resets the pid counter.  Matches the role `nx_graph_reset` plays
 * for the registry.
 */
void nx_process_reset_for_test(void);

#endif /* NX_FRAMEWORK_PROCESS_H */
