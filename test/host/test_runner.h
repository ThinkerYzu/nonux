#ifndef NONUX_TEST_RUNNER_H
#define NONUX_TEST_RUNNER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Minimal host-side test framework for nonux.
 *
 * Usage:
 *   TEST(pmm_alloc_returns_aligned_page) {
 *       pmm_init(region, size);
 *       void *p = pmm_alloc_page();
 *       ASSERT(p != NULL);
 *       ASSERT_EQ_U((uintptr_t)p & (PMM_PAGE_SIZE - 1), 0);
 *   }
 *
 *   int main(void) { return test_main(); }
 *
 * Tests self-register via a .test_registry ELF section (GNU ld / gcc only —
 * host tool only, not used in the kernel build).
 */

typedef void (*test_fn_t)(void);

struct test_entry {
    const char *name;
    test_fn_t   fn;
};

/* Emitted per TEST() into a dedicated section so test_main can walk them
 * without maintaining a hand-written list. */
#define TEST(name)                                                       \
    static void test_##name(void);                                       \
    static const struct test_entry _test_entry_##name                    \
        __attribute__((used, section("test_registry")))                  \
        = { #name, test_##name };                                        \
    static void test_##name(void)

extern const struct test_entry __start_test_registry[];
extern const struct test_entry __stop_test_registry[];

int test_main(void);

/* --- Assertions --- */

/* Fail the current test. Called by the ASSERT* macros; never call directly. */
void test_fail(const char *file, int line, const char *msg);

#define ASSERT(expr)                                                     \
    do {                                                                 \
        if (!(expr)) {                                                   \
            test_fail(__FILE__, __LINE__, "ASSERT(" #expr ")");          \
            return;                                                      \
        }                                                                \
    } while (0)

#define ASSERT_EQ_U(a, b)                                                \
    do {                                                                 \
        unsigned long long _a = (unsigned long long)(a);                 \
        unsigned long long _b = (unsigned long long)(b);                 \
        if (_a != _b) {                                                  \
            char _buf[128];                                              \
            snprintf(_buf, sizeof _buf,                                  \
                     "ASSERT_EQ_U(" #a ", " #b "): %llu != %llu",        \
                     _a, _b);                                            \
            test_fail(__FILE__, __LINE__, _buf);                         \
            return;                                                      \
        }                                                                \
    } while (0)

#define ASSERT_EQ_PTR(a, b)                                              \
    do {                                                                 \
        const void *_a = (a);                                            \
        const void *_b = (b);                                            \
        if (_a != _b) {                                                  \
            char _buf[128];                                              \
            snprintf(_buf, sizeof _buf,                                  \
                     "ASSERT_EQ_PTR(" #a ", " #b "): %p != %p",          \
                     _a, _b);                                            \
            test_fail(__FILE__, __LINE__, _buf);                         \
            return;                                                      \
        }                                                                \
    } while (0)

#define ASSERT_NULL(p)    ASSERT((p) == NULL)
#define ASSERT_NOT_NULL(p) ASSERT((p) != NULL)

#endif /* NONUX_TEST_RUNNER_H */
