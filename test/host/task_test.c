/*
 * Host-side tests for core/sched/task.{c,h} + core/sched/sched.c.
 *
 * Host cannot exercise the ARM64 context-switch assembly (cpu_switch_to
 * lives in context.S and needs ARM64 registers / TPIDR_EL1).  The
 * round-trip + callee-saved tests live in test/kernel/ktest_context.c.
 *
 * What we verify here:
 *   - nx_task_create populates cpu_ctx so the first switch would land at
 *     entry(arg): x19 == entry, x20 == arg, SP inside the allocated
 *     kstack, 16-byte aligned.
 *   - Task name is copied with truncation.
 *   - Each task gets a unique nonzero id.
 *   - nx_task_destroy releases everything (no leaks via mem_track).
 *   - preempt_disable / enable nest correctly on the host-stubbed
 *     "current" task.
 */

#include "test_runner.h"

#include "core/sched/task.h"
#include "core/sched/sched.h"

static void dummy_entry(void *arg) { (void)arg; }

TEST(task_create_populates_cpu_ctx_for_first_switch)
{
    int arg_sentinel = 0;
    struct nx_task *t = nx_task_create("t0", dummy_entry, &arg_sentinel, 1);
    ASSERT_NOT_NULL(t);

    ASSERT_EQ_U(t->cpu_ctx.x19, (uint64_t)(uintptr_t)dummy_entry);
    ASSERT_EQ_U(t->cpu_ctx.x20, (uint64_t)(uintptr_t)&arg_sentinel);

    uintptr_t sp_top = (uintptr_t)t->kstack_base + t->kstack_size;
    sp_top &= ~(uintptr_t)0xf;
    ASSERT_EQ_U(t->cpu_ctx.sp, (uint64_t)sp_top);
    ASSERT_EQ_U(t->cpu_ctx.sp & 0xf, 0);

    ASSERT_EQ_U(t->state, NX_TASK_READY);
    ASSERT_EQ_U(t->preempt_count, 0);
    ASSERT_EQ_U(t->need_resched, 0);

    nx_task_destroy(t);
}

TEST(task_create_truncates_long_name)
{
    /* Name longer than NX_TASK_NAME_MAX must truncate with a NUL at the
     * last slot. */
    const char *long_name = "abcdefghijklmnopqrstuvwxyz";
    struct nx_task *t = nx_task_create(long_name, dummy_entry, NULL, 1);
    ASSERT_NOT_NULL(t);

    ASSERT_EQ_U(t->name[NX_TASK_NAME_MAX - 1], '\0');
    /* First NX_TASK_NAME_MAX-1 bytes match the long name. */
    for (int i = 0; i < NX_TASK_NAME_MAX - 1; i++)
        ASSERT_EQ_U(t->name[i], long_name[i]);

    nx_task_destroy(t);
}

TEST(task_create_assigns_unique_nonzero_ids)
{
    struct nx_task *a = nx_task_create("a", dummy_entry, NULL, 1);
    struct nx_task *b = nx_task_create("b", dummy_entry, NULL, 1);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);

    ASSERT(a->id != 0);
    ASSERT(b->id != 0);
    ASSERT(a->id != b->id);

    nx_task_destroy(a);
    nx_task_destroy(b);
}

TEST(task_create_rejects_null_entry)
{
    struct nx_task *t = nx_task_create("bad", NULL, NULL, 1);
    ASSERT_NULL(t);
}

TEST(preempt_disable_and_enable_nest_on_current)
{
    struct nx_task *t = nx_task_create("ctl", dummy_entry, NULL, 1);
    ASSERT_NOT_NULL(t);

    nx_task_set_current_for_test(t);
    ASSERT_EQ_U(nx_preempt_count(), 0);

    nx_preempt_disable();
    ASSERT_EQ_U(nx_preempt_count(), 1);
    nx_preempt_disable();
    ASSERT_EQ_U(nx_preempt_count(), 2);
    nx_preempt_enable();
    ASSERT_EQ_U(nx_preempt_count(), 1);
    nx_preempt_enable();
    ASSERT_EQ_U(nx_preempt_count(), 0);

    /* Underflow guard — enable with zero count stays at zero. */
    nx_preempt_enable();
    ASSERT_EQ_U(nx_preempt_count(), 0);

    nx_task_set_current_for_test(NULL);
    nx_task_destroy(t);
}

TEST(preempt_count_without_current_is_zero)
{
    nx_task_set_current_for_test(NULL);
    ASSERT_EQ_U(nx_preempt_count(), 0);
    /* These calls must be safe with no current — they early-return. */
    nx_preempt_disable();
    nx_preempt_enable();
    ASSERT_EQ_U(nx_preempt_count(), 0);
}
