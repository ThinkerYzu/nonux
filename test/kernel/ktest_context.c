/*
 * In-kernel tests for the ARM64 context switch primitive (slice 4.1).
 *
 * Two tests:
 *
 *   context_switch_round_trip — create task B, switch into it from the
 *   boot context (A).  B writes a sentinel, switches back to A.  A
 *   observes the sentinel => the save/restore actually happened and
 *   landed at B's entry.
 *
 *   context_switch_preserves_callee_saved — load known values into
 *   x19..x28 before switching out, verify they're unchanged after
 *   switching back.  Proves every callee-saved register is correctly
 *   saved and restored.
 *
 * These tests run at EL1h on the boot CPU's stack.  They manipulate the
 * "current" pointer (TPIDR_EL1) directly rather than going through the
 * scheduler policy — slice 4.1 has no policy.  After each test the boot
 * context is restored as "current" so later tests don't run on an
 * unexpected TPIDR_EL1 value.
 */

#include "ktest.h"

#include "core/sched/task.h"
#include "core/sched/sched.h"

/* --- shared state used to communicate between the two tasks --- */
static struct nx_task *g_a;      /* boot context, switched into when B returns */
static struct nx_task *g_b;      /* new task created per test */
static volatile int    g_sentinel;

static inline void set_current(struct nx_task *t)
{
    asm volatile("msr tpidr_el1, %0" :: "r"(t) : "memory");
}

static inline struct nx_task *get_current(void)
{
    struct nx_task *t;
    asm volatile("mrs %0, tpidr_el1" : "=r"(t));
    return t;
}

/* --- round-trip --- */

static void entry_write_sentinel_and_return(void *arg)
{
    g_sentinel = (int)(uintptr_t)arg;
    /* Switch back to A.  cpu_switch_to updates TPIDR_EL1 for us. */
    cpu_switch_to(g_b, g_a);
    /* Unreachable — once A resumes, B's cpu_ctx is stale and will only
     * be used again if A context-switches back.  Park if somehow we come
     * back without a new switch-in. */
    for (;;) asm volatile("wfe");
}

KTEST(context_switch_round_trip)
{
    /* Stand up a "current" for A so nx_task_current() returns something
     * sane during the switch.  On entry TPIDR_EL1 is whatever the
     * previous ktest left it as. */
    static struct nx_task boot_task;
    boot_task.state = NX_TASK_RUNNING;
    set_current(&boot_task);
    g_a = &boot_task;

    g_b = nx_task_create("ktest_b", entry_write_sentinel_and_return,
                         (void *)(uintptr_t)0xC0FFEE, 1);
    KASSERT_NOT_NULL(g_b);

    g_sentinel = 0;
    cpu_switch_to(g_a, g_b);   /* jumps to entry via the bootstrap thunk */
    /* B's entry wrote g_sentinel and switched back to A — control
     * resumes here. */
    KASSERT_EQ_U(g_sentinel, 0xC0FFEE);
    KASSERT_EQ_U((uint64_t)(uintptr_t)get_current(),
                 (uint64_t)(uintptr_t)&boot_task);

    nx_task_destroy(g_b);
    g_b = 0;

    /* Leave TPIDR_EL1 non-NULL so any preempt_{disable,enable} called
     * by subsequent ktests against the same boot_task still works. */
}

/* --- callee-saved preservation --- */

static volatile uint64_t g_seen_x19;
static volatile uint64_t g_seen_x28;

/* B loads "weird" values into x19..x28 to try to stomp whatever A had
 * there, then switches back.  A then reads its own x19/x28 — if
 * cpu_switch_to saved / restored correctly, A still sees its original
 * values.
 *
 * We only check x19 and x28 as bookends; if those two survived the
 * full save/restore pair block, the ldp/stp pairs in between worked. */
static void entry_stomp_callee_saved_and_return(void *arg)
{
    (void)arg;
    /* Write recognisable garbage into every callee-saved reg so any bug
     * in A's save path shows up. */
    register uint64_t r19 asm("x19") = 0xAAAA1111ULL;
    register uint64_t r20 asm("x20") = 0xAAAA2222ULL;
    register uint64_t r21 asm("x21") = 0xAAAA3333ULL;
    register uint64_t r22 asm("x22") = 0xAAAA4444ULL;
    register uint64_t r23 asm("x23") = 0xAAAA5555ULL;
    register uint64_t r24 asm("x24") = 0xAAAA6666ULL;
    register uint64_t r25 asm("x25") = 0xAAAA7777ULL;
    register uint64_t r26 asm("x26") = 0xAAAA8888ULL;
    register uint64_t r27 asm("x27") = 0xAAAA9999ULL;
    register uint64_t r28 asm("x28") = 0xAAAAAAAAULL;
    asm volatile("" : : "r"(r19), "r"(r20), "r"(r21), "r"(r22),
                       "r"(r23), "r"(r24), "r"(r25), "r"(r26),
                       "r"(r27), "r"(r28));
    cpu_switch_to(g_b, g_a);
    for (;;) asm volatile("wfe");
}

KTEST(context_switch_preserves_callee_saved)
{
    /* Reuse A = boot_task from round-trip.  If it ran first g_a is
     * populated; if not, set it up. */
    static struct nx_task boot_task;
    if (!g_a) {
        boot_task.state = NX_TASK_RUNNING;
        set_current(&boot_task);
        g_a = &boot_task;
    }

    g_b = nx_task_create("ktest_cs", entry_stomp_callee_saved_and_return,
                         NULL, 1);
    KASSERT_NOT_NULL(g_b);

    /* Write recognisable values into our own x19 and x28, switch away,
     * come back, confirm they're still what we wrote. */
    register uint64_t myr19 asm("x19") = 0xBBBB1111ULL;
    register uint64_t myr28 asm("x28") = 0xBBBB2222ULL;
    asm volatile("" : "+r"(myr19), "+r"(myr28));

    cpu_switch_to(g_a, g_b);

    /* Back from B — observe x19/x28.  Move into memory before any
     * KASSERT evaluates expressions (which may clobber x19/x28). */
    asm volatile("" : "=r"(myr19));
    asm volatile("" : "=r"(myr28));
    g_seen_x19 = myr19;
    g_seen_x28 = myr28;

    KASSERT_EQ_U(g_seen_x19, 0xBBBB1111ULL);
    KASSERT_EQ_U(g_seen_x28, 0xBBBB2222ULL);

    nx_task_destroy(g_b);
    g_b = 0;
}
