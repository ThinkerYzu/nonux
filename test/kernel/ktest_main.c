#include "ktest.h"

#include "core/sched/task.h"

/* Linker-generated markers for the kernel_test_registry section. */
extern const struct ktest_entry __start_kernel_test_registry[];
extern const struct ktest_entry __stop_kernel_test_registry[];

/* The core scheduler driver's idle task (slice 4.4).  Exposed so that
 * ktest_main can reset TPIDR_EL1 between tests — a test that set
 * TPIDR_EL1 to its own ad-hoc task (e.g. ktest_context's static
 * boot_task) would otherwise leak that pointer into subsequent tests
 * that call nx_task_current() and assume the idle task is current. */
extern struct nx_task g_idle_task;

static inline void reset_current_to_idle(void)
{
    asm volatile("msr tpidr_el1, %0" :: "r"(&g_idle_task) : "memory");
}

int ktest_current_failed;

void ktest_fail(const char *file, int line, const char *msg)
{
    kprintf("    FAIL %s:%d: %s\n", file, line, msg);
    ktest_current_failed = 1;
}

/* ARM semihosting SYS_EXIT_EXTENDED — QEMU propagates `status` as its own
 * process exit code when the VM was started with -semihosting. */
#define ADP_STOPPED_APPLICATION_EXIT 0x20026UL
#define SYS_EXIT_EXTENDED            0x20UL

void ktest_exit(int status)
{
    uint64_t params[2] = { ADP_STOPPED_APPLICATION_EXIT, (uint64_t)status };
    register uint64_t x0 asm("x0") = SYS_EXIT_EXTENDED;
    register uint64_t x1 asm("x1") = (uint64_t)(uintptr_t)params;
    asm volatile("hlt #0xf000" :: "r"(x0), "r"(x1) : "memory");
    /* If semihosting wasn't enabled on the QEMU command line the HLT
     * falls through; park the CPU. */
    for (;;) asm volatile("wfe");
}

void ktest_main(void)
{
    size_t n = __stop_kernel_test_registry - __start_kernel_test_registry;
    int    failed = 0;

    kprintf("\n=== ktest: running %lu test(s) ===\n", (uint64_t)n);

    for (size_t i = 0; i < n; i++) {
        const struct ktest_entry *t = &__start_kernel_test_registry[i];
        ktest_current_failed = 0;

        /* Reset per-test state that tests may have mutated:
         *   - TPIDR_EL1 → idle task.  Without this, ktest_context's
         *     tests leak their own boot_task.X struct into TPIDR_EL1
         *     and subsequent tests that call nx_task_current()
         *     operate on a stale pointer.
         */
        reset_current_to_idle();

        kprintf("  %s", t->name);
        /* kprintf lacks width specifiers; pad manually so pass/fail
         * columns line up. */
        size_t len = strlen(t->name);
        for (size_t pad = len < 48 ? 48 - len : 1; pad > 0; pad--)
            uart_putc(' ');
        t->fn();

        if (ktest_current_failed) {
            kprintf("FAIL\n");
            failed++;
        } else {
            kprintf("PASS\n");
        }
    }

    kprintf("\n");
    if (failed) {
        kprintf("ktest: %d/%lu FAILED\n", failed, (uint64_t)n);
        ktest_exit(1);
    } else {
        kprintf("ktest: %lu/%lu PASSED, 0 failures\n", (uint64_t)n, (uint64_t)n);
        ktest_exit(0);
    }
}
