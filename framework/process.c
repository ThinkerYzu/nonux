/*
 * Process framework — slice 7.1 implementation.  See process.h for
 * the contract.
 */

#include "framework/process.h"
#include "framework/channel.h"
#include "framework/console.h"
#include "framework/handle.h"

#include <stddef.h>
#include <stdint.h>

#if __STDC_HOSTED__
#include <stdlib.h>
#include <string.h>
#else
#include "core/lib/kheap.h"
#include "core/lib/lib.h"
#include "core/mmu/mmu.h"
#include "core/sched/task.h"
#endif

/* ---------- The always-present kernel process (pid 0) --------------- */

struct nx_process g_kernel_process = {
    .pid        = 0,
    .name       = "kernel",
    .state      = NX_PROCESS_STATE_ACTIVE,
    .exit_code  = 0,
    /* .handles is zero-initialised — equivalent to a fresh
     * `nx_handle_table_init` call.  The handle table's invariant is
     * that a zero-initialised table is empty, so we don't need an
     * explicit init in the BSS-backed definition. */
};

/* ---------- Process bookkeeping ------------------------------------- */
/*
 * Small linear table of live user processes.  Fixed capacity in v1 —
 * processes are expensive (pid + handle table) and the test surface
 * doesn't need more than a handful concurrently.  Lookup is O(n) but
 * `n` is tiny; swap for a hashmap when there's a consumer that cares.
 */
/* v1 cap.  Was 16; bumped to 32 in slice 7.6c.4 because the test
 * harness's stranded-task convention leaves processes in the table
 * across tests; bumped again 32 → 64 in slice 7.6d.N.2 after the
 * cumulative test sweep (now exec'ing busybox three times for
 * `--help` / `sh -c "exit 42"` / `sh -c "echo hello"`) crossed
 * the 32 threshold.  Note the harness's `sched_rr_purge_user_tasks`
 * only unlinks tasks from the scheduler runqueue — it doesn't call
 * `nx_process_destroy`, so the process + its handle table + MMU
 * address space + 8 MiB user backing all stay allocated.  Real
 * reap on `wait()` (call `nx_process_destroy(child)` when
 * `sys_wait` collects status) would let us drop this back;
 * tracked under slice 7.7 follow-ups. */
#define NX_PROCESS_TABLE_CAPACITY 64

static struct nx_process *g_process_table[NX_PROCESS_TABLE_CAPACITY];
static uint32_t            g_pid_next = 1;    /* pid 0 reserved for kernel */

static int process_table_add(struct nx_process *p)
{
    for (int i = 0; i < NX_PROCESS_TABLE_CAPACITY; i++) {
        if (!g_process_table[i]) { g_process_table[i] = p; return 0; }
    }
    return -1;
}

static void process_table_remove(struct nx_process *p)
{
    for (int i = 0; i < NX_PROCESS_TABLE_CAPACITY; i++) {
        if (g_process_table[i] == p) { g_process_table[i] = NULL; return; }
    }
}

/* ---------- API ----------------------------------------------------- */

struct nx_process *nx_process_create(const char *name)
{
#if __STDC_HOSTED__
    struct nx_process *p = calloc(1, sizeof *p);
#else
    struct nx_process *p = malloc(sizeof *p);
    if (p) memset(p, 0, sizeof *p);
#endif
    if (!p) return NULL;

    if (process_table_add(p) != 0) {
#if __STDC_HOSTED__
        free(p);
#else
        free(p);
#endif
        return NULL;
    }

    p->pid       = g_pid_next++;
    p->state     = NX_PROCESS_STATE_ACTIVE;
    p->exit_code = 0;

    if (name) {
        size_t i = 0;
        for (; i < NX_PROCESS_NAME_MAX - 1 && name[i]; i++) p->name[i] = name[i];
        p->name[i] = '\0';
    } else {
        p->name[0] = '\0';
    }

    nx_handle_table_init(&p->handles);

    /*
     * Slice 7.6d.N.6b: pre-install three CONSOLE handles at the head of
     * the table so STDOUT_FILENO=1 / STDERR_FILENO=2 round-trip through
     * the regular handle dispatch (no magic-fd fallback) and so that
     * `pipe()` allocations naturally land at slot 3+ rather than
     * colliding with POSIX fds 1/2.
     *
     *   slot 0  encoded handle 1  rights=WRITE  → POSIX STDOUT_FILENO
     *   slot 1  encoded handle 2  rights=WRITE  → POSIX STDERR_FILENO
     *   slot 2  encoded handle 3  rights=READ   → POSIX STDIN_FILENO
     *                                             (also reachable via
     *                                              the h==0 special
     *                                              case in sys_read /
     *                                              sys_handle_close /
     *                                              sys_dup3, since
     *                                              encoded value 0 is
     *                                              reserved for
     *                                              NX_HANDLE_INVALID)
     *
     * `nx_handle_alloc` scans linearly from slot 0 so the encoded
     * values are deterministic on a fresh table.  In a 64-entry table
     * the first three allocs cannot fail, so the per-call status
     * checks here are belt-and-braces.
     */
    nx_handle_t h0, h1, h2;
    int rc0 = nx_handle_alloc(&p->handles, NX_HANDLE_CONSOLE,
                              NX_RIGHT_WRITE, &g_nx_console, &h0);
    int rc1 = nx_handle_alloc(&p->handles, NX_HANDLE_CONSOLE,
                              NX_RIGHT_WRITE, &g_nx_console, &h1);
    int rc2 = nx_handle_alloc(&p->handles, NX_HANDLE_CONSOLE,
                              NX_RIGHT_READ,  &g_nx_console, &h2);
    if (rc0 != NX_OK || rc1 != NX_OK || rc2 != NX_OK
        || h0 != 1 || h1 != 2 || h2 != 3) {
        process_table_remove(p);
        free(p);
        return NULL;
    }

    /*
     * Slice 7.2: allocate a private address space.  On kernel this
     * is a real TTBR0 root; on host it stays 0 (no MMU).  Failure
     * rolls back the process registration.
     */
#if !__STDC_HOSTED__
    p->ttbr0_root = mmu_create_address_space();
    if (p->ttbr0_root == 0) {
        process_table_remove(p);
        free(p);
        return NULL;
    }
    p->brk_addr  = mmu_user_window_base() + NX_PROCESS_HEAP_OFFSET;
    p->mmap_bump = mmu_user_window_base() + NX_PROCESS_MMAP_OFFSET;
#else
    p->ttbr0_root = 0;
    p->brk_addr   = 0;
    p->mmap_bump  = 0;
#endif

    return p;
}

void nx_process_destroy(struct nx_process *p)
{
    if (!p) return;
    if (p == &g_kernel_process) {
        /* Clearing the table is legal (test-fixture use); freeing the
         * static storage isn't. */
        nx_handle_table_init(&p->handles);
        p->state     = NX_PROCESS_STATE_ACTIVE;
        p->exit_code = 0;
        return;
    }

    /* Close every live handle so type-aware destructors run (channel
     * endpoints released, future VMO / FILE destructors fire). */
    for (size_t i = 0; i < NX_HANDLE_TABLE_CAPACITY; i++) {
        struct nx_handle_entry *e = &p->handles.entries[i];
        if (e->type != NX_HANDLE_INVALID) {
            /* Reconstruct the handle value the same way alloc did so
             * nx_handle_close finds it.  Matches the layout documented
             * in framework/handle.h. */
            nx_handle_t h = (e->generation << 8) | (uint32_t)(i + 1);
            nx_handle_close(&p->handles, h);
        }
    }

    process_table_remove(p);
#if !__STDC_HOSTED__
    if (p->ttbr0_root) {
        mmu_destroy_address_space(p->ttbr0_root);
        p->ttbr0_root = 0;
    }
#endif
    free(p);
}

struct nx_process *nx_process_current(void)
{
#if !__STDC_HOSTED__
    struct nx_task *t = nx_task_current();
    if (t && t->process) return t->process;
#endif
    return &g_kernel_process;
}

struct nx_process *nx_process_lookup_by_pid(uint32_t pid)
{
    if (pid == 0) return &g_kernel_process;
    for (int i = 0; i < NX_PROCESS_TABLE_CAPACITY; i++) {
        if (g_process_table[i] && g_process_table[i]->pid == pid)
            return g_process_table[i];
    }
    return NULL;
}

size_t nx_process_count(void)
{
    /* The kernel process is always present. */
    size_t n = 1;
    for (int i = 0; i < NX_PROCESS_TABLE_CAPACITY; i++)
        if (g_process_table[i]) n++;
    return n;
}

void nx_process_exit(int code)
{
    struct nx_process *p = nx_process_current();
    p->state     = NX_PROCESS_STATE_EXITED;
    p->exit_code = code;

    /*
     * Slice 7.6d.N.6b: close every live CHANNEL handle in the exiting
     * process's table so peer endpoints (e.g. the read side of a pipe
     * the parent shell holds) observe `peer->closed = true` once all
     * writer references drop to zero.  Without this, pipelines like
     * `echo hello | cat` hang: cat reads "hello", queue empties, but
     * the writer count never reaches 0 because the zombie echo process
     * still holds its dup3'd stdout handle.
     *
     * CONSOLE entries are singletons (no per-handle destructor) — we
     * close their slots so the table is clean if a future reaper
     * walks it, but the underlying object pointer doesn't need
     * decrementing.  FILE/DIR entries are deferred to a real reap-
     * on-wait — they'd need vfs dispatch + kheap free, but the
     * v1 ramfs leaks them anyway since wait() doesn't free the
     * process struct.
     *
     * The process struct itself is NOT freed here — wait() needs the
     * exit_code + state visible.  Real reap-on-wait is still deferred.
     */
    for (size_t i = 0; i < NX_HANDLE_TABLE_CAPACITY; i++) {
        struct nx_handle_entry *e = &p->handles.entries[i];
        if (e->type == NX_HANDLE_INVALID) continue;
        if (e->type == NX_HANDLE_CHANNEL && e->object) {
            nx_channel_endpoint_close(e->object);
        }
        /* Free the slot regardless of type — bump generation, clear
         * fields.  Mirrors sys_handle_close's `nx_handle_close` tail. */
        nx_handle_t h = (e->generation << 8) | (uint32_t)(i + 1);
        nx_handle_close(&p->handles, h);
    }

    /* Park in a tight wfe loop on kernel, an infinite loop on host.
     * v1 ktest harness dequeues the stranded task externally.  Real
     * scheduler-integrated exit (dequeue + switch-to-next) lands in
     * slice 7.4 alongside `wait()`.
     *
     * Slice 7.4b: unmask IRQs before the wfe loop.  Hardware masks
     * DAIF.I on exception entry; if we leave the mask set, `wfe`
     * here doesn't wake on timer ticks — the CPU resumes from wfe
     * but the interrupt isn't delivered, so the outer `b 1b` just
     * re-enters wfe forever.  Unmasked, the tick fires, the IRQ
     * stub's tail `sched_check_resched` preempts this task, and a
     * waiting parent's `sys_wait` gets its chance to see the
     * EXITED state.
     */
#if __STDC_HOSTED__
    for (;;) { /* unreachable in tests — callers set up their own loop
                * via a host fixture before invoking sys_exit. */ }
#else
    asm volatile ("msr daifclr, #2" ::: "memory");   /* IRQ-enable */
    for (;;) asm volatile ("wfe");
#endif
}

struct nx_process *nx_process_find_exited_child(
    const struct nx_process *parent,
    struct nx_process **any_active_child)
{
    if (any_active_child) *any_active_child = NULL;
    if (!parent) return NULL;
    struct nx_process *active = NULL;
    for (int i = 0; i < NX_PROCESS_TABLE_CAPACITY; i++) {
        struct nx_process *p = g_process_table[i];
        if (!p) continue;
        if (p->parent_pid != parent->pid) continue;
        if (p->reaped) continue;   /* skip already-waited zombies */
        if (p->state == NX_PROCESS_STATE_EXITED) return p;
        if (p->state == NX_PROCESS_STATE_ACTIVE && !active) active = p;
    }
    if (any_active_child) *any_active_child = active;
    return NULL;
}

struct nx_process *nx_process_fork(struct nx_process *parent)
{
    if (!parent) return NULL;
    /* Same name as parent (+ a trailing asterisk would be nice but
     * NX_PROCESS_NAME_MAX is tight).  For v1 the name doesn't carry
     * semantics; a later slice could plumb a distinct child name. */
    struct nx_process *child = nx_process_create(parent->name);
    if (!child) return NULL;

    /* Slice 7.6d.N.6b: track parentage so `sys_wait(pid=-1)` (POSIX
     * waitpid-any-child) can identify children of the caller. */
    child->parent_pid = parent->pid;

#if !__STDC_HOSTED__
    mmu_copy_user_backing(parent->ttbr0_root, child->ttbr0_root);
#endif
    /* Inherit the parent's program break (slice 7.6c.3c).  The
     * forked backing already contains the parent's heap data byte-
     * for-byte; we just propagate the high-water mark so the
     * child's mallocng knows where to continue from. */
    child->brk_addr = parent->brk_addr;
    /* Same logic for the mmap arena bump pointer (slice 7.6d.N.1) —
     * the parent's mmap'd pages are byte-copied into the child by
     * mmu_copy_user_backing, and the child's mallocng inherits the
     * same view of "what's been allocated". */
    child->mmap_bump = parent->mmap_bump;
    /* Handle table left empty — see the header comment for rationale. */
    return child;
}

void nx_process_reset_for_test(void)
{
    for (int i = 0; i < NX_PROCESS_TABLE_CAPACITY; i++) {
        if (g_process_table[i]) {
            struct nx_process *p = g_process_table[i];
            g_process_table[i] = NULL;
            free(p);
        }
    }
    g_pid_next = 1;
    nx_handle_table_init(&g_kernel_process.handles);
    g_kernel_process.state     = NX_PROCESS_STATE_ACTIVE;
    g_kernel_process.exit_code = 0;
}
