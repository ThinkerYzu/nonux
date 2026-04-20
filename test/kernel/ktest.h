#ifndef NONUX_KTEST_H
#define NONUX_KTEST_H

#include "core/lib/lib.h"

/*
 * In-kernel test harness.
 *
 * Separate from the host-side framework in test/host — host tests run
 * against mocked hardware and libc; these tests run in the real kernel
 * image under QEMU after boot.c has brought up UART, PMM, vectors, GIC,
 * and the timer.
 *
 * Tests register themselves into the `kernel_test_registry` linker
 * section (the linker script KEEPs it).  The runner walks
 * `__start_kernel_test_registry` → `__stop_kernel_test_registry`
 * (GNU ld auto-generates those symbols).
 *
 * No setjmp/longjmp: on failure a KASSERT macro prints, sets a global
 * fail flag, and returns from the test function.  Each test must `return`
 * after calling a failing KASSERT so cleanup in the test's own scope
 * still runs.
 */

struct ktest_entry {
    const char *name;
    void      (*fn)(void);
};

#define KTEST(name)                                                         \
    static void ktest_##name(void);                                         \
    static const struct ktest_entry _ktest_entry_##name                     \
        __attribute__((used, section("kernel_test_registry")))              \
        = { #name, ktest_##name };                                          \
    static void ktest_##name(void)

extern int ktest_current_failed;

/* Print failure diagnostic and mark the current test failed. */
void ktest_fail(const char *file, int line, const char *msg);

#define KASSERT(expr)                                                       \
    do {                                                                    \
        if (!(expr)) {                                                      \
            ktest_fail(__FILE__, __LINE__, "KASSERT(" #expr ")");           \
            return;                                                         \
        }                                                                   \
    } while (0)

#define KASSERT_EQ_U(a, b)                                                  \
    do {                                                                    \
        uint64_t _a = (uint64_t)(a);                                        \
        uint64_t _b = (uint64_t)(b);                                        \
        if (_a != _b) {                                                     \
            kprintf("    FAIL %s:%d: KASSERT_EQ_U(" #a "," #b ") "          \
                    "%lu != %lu\n", __FILE__, __LINE__, _a, _b);            \
            ktest_current_failed = 1;                                       \
            return;                                                         \
        }                                                                   \
    } while (0)

#define KASSERT_NOT_NULL(p) KASSERT((p) != 0)
#define KASSERT_NULL(p)     KASSERT((p) == 0)

/* Exits QEMU via ARM semihosting SYS_EXIT_EXTENDED.  status=0 for pass,
 * nonzero for fail; propagated as QEMU's process exit code when the VM
 * was started with `-semihosting`. */
void ktest_exit(int status) __attribute__((noreturn));

/* Entry point: runs every registered test, prints a summary, calls
 * ktest_exit().  Called from boot_main when -DNX_KTEST was set. */
void ktest_main(void) __attribute__((noreturn));

#endif /* NONUX_KTEST_H */
