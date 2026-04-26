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
    /*
     * Pending-signal bitmask (slice 7.5).  Bit `signo` set means that
     * signal has been delivered but not yet acted on by the target.
     * Read + cleared at every `sched_check_resched` for the current
     * task; SIGKILL / SIGTERM cause `nx_process_exit(128 + signo)`
     * inline.  `_Atomic` because `sys_signal` may run on one CPU
     * while the target runs on another — v1 is single-CPU so the
     * race is academic, but the project rule is C11 atomics on
     * shared counters from day 1.
     */
    _Atomic uint32_t        pending_signals;
    /*
     * Program break (slice 7.6c.3c).  User-VA of the current top of
     * the heap.  Initialized in nx_process_create / sys_exec to
     * `mmu_user_window_base() + NX_PROCESS_HEAP_OFFSET`; grown by
     * `NX_SYS_BRK = 17` up to `... + NX_PROCESS_HEAP_LIMIT`.  musl's
     * mallocng uses this as its primary heap-extension primitive
     * (`brk(0)` reads, `brk(end)` extends).  Heap lives inside the
     * existing 8 MiB user-window backing — no extra kernel
     * allocation; we just track the high-water mark per process.
     */
    uint64_t                brk_addr;
};

/* Heap layout within the 8 MiB user window (slice 7.6d.2b grew the
 * window from 2 MiB to 8 MiB so a static-linked busybox image
 * (~1.91 MiB text+data) leaves room for stack + heap):
 *   [base ..        +6 MiB)   code + data + bss (loaded by exec/elf)
 *   [base + 6 MiB . +7.5 MiB) heap (NX_SYS_BRK)
 *   [base + 7.5 MiB ..top)    stack (sp_el0 starts at top - alignment)
 *
 * 1.5 MiB heap is plenty for our v1 demos (musl mallocng's chunked
 * allocations rarely exceed a few KiB; busybox sh / printf handful
 * of bytes per run).  Real /proc-style heap grows-on-demand lands
 * with a future "user-window-grows-via-PMM" slice.
 *
 * The 6 MiB code-segment ceiling is generous: busybox's two LOAD
 * segments span end-to-end ~1.91 MiB, so we have ~4 MiB of headroom
 * before any future binary collides with the heap base.  Bumping
 * further would push the heap into the stack region.
 */
#define NX_PROCESS_HEAP_OFFSET  (6u << 20)             /* 6 MiB into window */
#define NX_PROCESS_HEAP_LIMIT   ((7u << 20) + (1u << 19))  /* 7.5 MiB into window */

/* Slice 7.6d.3c — kernel-pre-initialized TLS area for musl-linked
 * (and any other libc-using) EL0 programs.  Lives in the unused gap
 * between code (busybox segment 2 ends at ~+1.91 MiB) and the brk
 * heap region (+6 MiB).  TPIDR_EL0 is set to `mmu_user_window_base()
 * + NX_PROCESS_TLS_OFFSET` before the first eret to EL0 in any
 * process.  musl's `__init_libc` reads `errno` (at offset 0x20 of
 * `struct pthread`) on every syscall return path; without a valid
 * TLS pointer pointing at zeroed memory, the very first errno write
 * faults at VA `0x20` (slice 7.6d.2c captured this exact failure).
 *
 * 4 KiB is plenty for musl's pre-`__set_thread_area` lifetime: musl
 * touches `errno` (offset 0x20), the canary, and a handful of small
 * fields, all within the first ~256 bytes of `struct pthread`.  Once
 * `__init_libc` calls `__set_thread_area(td)`, musl writes its own
 * properly-allocated `struct pthread` address into TPIDR_EL0 and the
 * kernel's pre-init buffer is dead. */
#define NX_PROCESS_TLS_OFFSET   (5u << 20)             /* 5 MiB into window */
#define NX_PROCESS_TLS_SIZE     4096u                  /* one page */

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

/*
 * Slice 7.4: duplicate `parent` as a fresh child process.
 *
 * The child gets:
 *   - a new pid (via `nx_process_create`),
 *   - its own address space with the parent's user-window contents
 *     byte-copied in at fork time (subsequent writes by either
 *     process stay private),
 *   - an empty handle table (handle inheritance lands in a later
 *     sub-slice — v1 fork's use case is "fork + exec" where the
 *     child rebuilds its handles anyway).
 *
 * Returns the child process on success, NULL on allocation failure.
 * Caller is responsible for:
 *   - creating a child TASK whose trap frame matches `parent`'s at
 *     the fork SVC point (see `nx_task_create_forked` in
 *     `core/sched/task.h`) and whose `process` pointer is set to
 *     the returned child.  `sys_fork` is the single kernel caller.
 *
 * Host builds have no MMU so the child has no real address-space
 * copy — `ttbr0_root` stays 0 on both parent and child.
 */
struct nx_process *nx_process_fork(struct nx_process *parent);

#endif /* NX_FRAMEWORK_PROCESS_H */
