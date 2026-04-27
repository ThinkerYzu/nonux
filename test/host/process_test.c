/*
 * Host tests for framework/process.{h,c} (slice 7.1).
 *
 * Each test resets process state to a clean slate so the counter-
 * based pid assertions are stable.  The always-present kernel
 * process (pid 0, `g_kernel_process`) is a static allocation, so
 * `nx_process_reset_for_test` only wipes its handle table — it can't
 * be destroyed.
 */

#include "test_runner.h"

#include "framework/handle.h"
#include "framework/process.h"
#include "framework/syscall.h"

#include <stdlib.h>
#include <string.h>

TEST(process_kernel_process_is_pid_zero_and_always_present)
{
    nx_process_reset_for_test();
    /* After a reset the kernel process is still there (pid 0) and
     * `nx_process_count` reports exactly one. */
    ASSERT_EQ_U(nx_process_count(), 1);
    ASSERT_EQ_PTR(nx_process_lookup_by_pid(0), &g_kernel_process);
    ASSERT_EQ_U(g_kernel_process.pid, 0);
    ASSERT(g_kernel_process.state == NX_PROCESS_STATE_ACTIVE);
}

TEST(process_current_defaults_to_kernel_process_with_no_task)
{
    nx_process_reset_for_test();
    /* Host tests have no scheduled task by default.
     * `nx_process_current` must fall back to the kernel process. */
    ASSERT_EQ_PTR(nx_process_current(), &g_kernel_process);
}

TEST(process_create_allocates_fresh_pid_and_pre_installs_console_handles)
{
    nx_process_reset_for_test();

    struct nx_process *p1 = nx_process_create("proc1");
    ASSERT_NOT_NULL(p1);
    ASSERT_EQ_U(p1->pid, 1);                /* first user pid is 1 */
    ASSERT(strcmp(p1->name, "proc1") == 0);
    ASSERT(p1->state == NX_PROCESS_STATE_ACTIVE);
    /* Slice 7.6d.N.6b: nx_process_create pre-installs three CONSOLE
     * handles at slots 0/1/2 (encoded values 1/2/3) so STDOUT_FILENO=1
     * and STDERR_FILENO=2 round-trip through real handle dispatch and
     * pipe() allocations naturally land at slot 3+. */
    ASSERT_EQ_U(nx_handle_table_count(&p1->handles), 3);

    struct nx_process *p2 = nx_process_create("proc2");
    ASSERT_NOT_NULL(p2);
    ASSERT_EQ_U(p2->pid, 2);
    ASSERT(p1 != p2);

    ASSERT_EQ_U(nx_process_count(), 3);     /* kernel + p1 + p2 */
    ASSERT_EQ_PTR(nx_process_lookup_by_pid(1), p1);
    ASSERT_EQ_PTR(nx_process_lookup_by_pid(2), p2);

    nx_process_destroy(p1);
    nx_process_destroy(p2);
    ASSERT_EQ_U(nx_process_count(), 1);
    ASSERT_NULL(nx_process_lookup_by_pid(1));
}

TEST(process_destroy_closes_outstanding_handles)
{
    nx_process_reset_for_test();
    struct nx_process *p = nx_process_create("lifecycle");
    ASSERT_NOT_NULL(p);

    /* Seed a couple of VMO handles.  Destroy should release them via
     * nx_handle_close (VMO has no type-aware destructor, so this just
     * exercises the table-walk path).  Pre-installed CONSOLE handles
     * (slice 7.6d.N.6b) account for 3 of the count; the two VMOs land
     * at slots 3 and 4 for a total of 5. */
    static int dummy1, dummy2;
    nx_handle_t h1, h2;
    ASSERT_EQ_U(nx_handle_alloc(&p->handles, NX_HANDLE_VMO, NX_RIGHTS_ALL,
                                &dummy1, &h1), NX_OK);
    ASSERT_EQ_U(nx_handle_alloc(&p->handles, NX_HANDLE_VMO, NX_RIGHTS_ALL,
                                &dummy2, &h2), NX_OK);
    ASSERT_EQ_U(nx_handle_table_count(&p->handles), 5);

    nx_process_destroy(p);
    /* The process is gone — no way to inspect its table after destroy,
     * but nx_process_count confirms the registration is gone. */
    ASSERT_EQ_U(nx_process_count(), 1);
}

TEST(process_destroy_on_kernel_process_clears_table_but_keeps_process)
{
    nx_process_reset_for_test();

    static int d;
    nx_handle_t h;
    ASSERT_EQ_U(nx_handle_alloc(&g_kernel_process.handles, NX_HANDLE_VMO,
                                NX_RIGHTS_ALL, &d, &h), NX_OK);
    ASSERT_EQ_U(nx_handle_table_count(&g_kernel_process.handles), 1);

    /* destroy on the static kernel process is documented to clear the
     * handle table without freeing the struct. */
    nx_process_destroy(&g_kernel_process);
    ASSERT_EQ_U(nx_handle_table_count(&g_kernel_process.handles), 0);
    ASSERT_EQ_PTR(nx_process_lookup_by_pid(0), &g_kernel_process);
}

TEST(process_two_processes_have_independent_handle_tables)
{
    nx_process_reset_for_test();
    struct nx_process *a = nx_process_create("a");
    struct nx_process *b = nx_process_create("b");
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);

    static int obj_a, obj_b;
    nx_handle_t ha, hb;
    ASSERT_EQ_U(nx_handle_alloc(&a->handles, NX_HANDLE_VMO,
                                NX_RIGHT_READ, &obj_a, &ha), NX_OK);
    ASSERT_EQ_U(nx_handle_alloc(&b->handles, NX_HANDLE_VMO,
                                NX_RIGHT_READ, &obj_b, &hb), NX_OK);

    /* Same handle value in two different tables resolves to different
     * objects (or nothing, depending on the encoding overlap). */
    void *got = NULL;
    ASSERT_EQ_U(nx_handle_lookup(&a->handles, ha, 0, 0, &got), NX_OK);
    ASSERT_EQ_PTR(got, &obj_a);

    /* Looking up `a`'s handle in `b`'s table fails — independent
     * generation counters make collisions exceedingly unlikely, but
     * even when the encoded value happens to match (index + 1), the
     * lookup either returns NX_ENOENT (slot empty) or a different
     * object.  The critical invariant is "not `obj_a`". */
    got = NULL;
    int rc = nx_handle_lookup(&b->handles, ha, 0, 0, &got);
    if (rc == NX_OK) ASSERT(got != &obj_a);
    else              ASSERT_EQ_U(rc, NX_ENOENT);

    nx_process_destroy(a);
    nx_process_destroy(b);
}

TEST(process_syscall_current_table_points_at_current_processs_table)
{
    nx_process_reset_for_test();
    /* With no current task, `nx_syscall_current_table` returns the
     * kernel process's table.  Verify by allocating a handle through
     * the table and observing it via both the process pointer and the
     * syscall-current-table helper. */
    struct nx_handle_table *t = nx_syscall_current_table();
    ASSERT_EQ_PTR(t, &g_kernel_process.handles);

    static int obj;
    nx_handle_t h;
    ASSERT_EQ_U(nx_handle_alloc(t, NX_HANDLE_VMO, NX_RIGHTS_ALL,
                                &obj, &h), NX_OK);
    ASSERT_EQ_U(nx_handle_table_count(&g_kernel_process.handles), 1);
}

TEST(process_reset_for_test_wipes_user_processes_and_kernel_handles)
{
    nx_process_reset_for_test();   /* pre-clean so the seed count below is 1 */
    struct nx_process *p = nx_process_create("to-wipe");
    ASSERT_NOT_NULL(p);

    static int d;
    nx_handle_t h;
    ASSERT_EQ_U(nx_handle_alloc(&g_kernel_process.handles, NX_HANDLE_VMO,
                                NX_RIGHTS_ALL, &d, &h), NX_OK);

    ASSERT(nx_process_count() >= 2);
    ASSERT_EQ_U(nx_handle_table_count(&g_kernel_process.handles), 1);

    nx_process_reset_for_test();
    ASSERT_EQ_U(nx_process_count(), 1);
    ASSERT_EQ_U(nx_handle_table_count(&g_kernel_process.handles), 0);
    /* pid allocation restarts at 1. */
    struct nx_process *q = nx_process_create("fresh");
    ASSERT_NOT_NULL(q);
    ASSERT_EQ_U(q->pid, 1);
    nx_process_destroy(q);
}
