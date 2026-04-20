#include "test_runner.h"
#include "mem_track.h"

#include <setjmp.h>

static jmp_buf     g_test_env;
static const char *g_current_test_name;
static int         g_current_test_failed;

void test_fail(const char *file, int line, const char *msg)
{
    g_current_test_failed = 1;
    fprintf(stderr, "\n    FAIL  %s:%d: %s\n", file, line, msg);
    longjmp(g_test_env, 1);
}

static int run_one(const struct test_entry *t)
{
    g_current_test_name   = t->name;
    g_current_test_failed = 0;

    mt_reset();

    printf("  %-40s ", t->name);
    fflush(stdout);

    if (setjmp(g_test_env) == 0) {
        t->fn();
    }

    int leaks = mt_check_leaks();
    if (leaks > 0) {
        g_current_test_failed = 1;
        fprintf(stderr, "    FAIL  %s: %d memory leak(s)\n", t->name, leaks);
    }

    if (g_current_test_failed) {
        printf("FAIL\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}

int test_main(void)
{
    size_t n = __stop_test_registry - __start_test_registry;
    int    failed = 0;

    printf("Running %zu test(s):\n", n);
    for (size_t i = 0; i < n; i++)
        failed += run_one(&__start_test_registry[i]);

    printf("\n");
    if (failed) {
        printf("FAIL: %d/%zu tests failed\n", failed, n);
        return 1;
    }
    printf("PASS: %zu/%zu tests passed, 0 leaks, 0 errors\n", n, n);
    return 0;
}
