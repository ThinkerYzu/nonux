#ifndef NONUX_MONOTONIC_H
#define NONUX_MONOTONIC_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Monotonic deadline primitive — slice 3.9b.2.
 *
 * The pause protocol (framework/component.c:nx_component_pause) must not
 * wait forever on a misbehaving pause_hook: recomposition has a wall-clock
 * budget, and any single step over-running it has to roll back rather than
 * wedge the system.  `nx_deadline_start` samples a monotonic counter and
 * converts the caller's budget (nanoseconds) into units of that counter
 * once; `nx_deadline_exceeded` compares the current counter against the
 * captured snapshot.  The per-check path does one mrs + one uint64 compare
 * — no division, no floating point (compatible with -mgeneral-regs-only).
 *
 * Kernel reads `cntpct_el0` scaled by `cntfrq_el0`.  Host uses
 * CLOCK_MONOTONIC directly, so the threshold is just the budget in ns.
 *
 * If the clock read fails (host: errno path), the deadline reports
 * "exceeded" on the first check so the caller rolls back rather than
 * blocks waiting on a broken clock.
 */

struct nx_deadline {
    uint64_t start;          /* raw counter / ns at _start() */
    uint64_t threshold;      /* equivalent units for the budget */
    bool     valid;          /* false if _start() couldn't read the clock */
};

#if __STDC_HOSTED__

#include <time.h>

static inline uint64_t nx_monotonic_raw(bool *ok)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        if (ok) *ok = false;
        return 0;
    }
    if (ok) *ok = true;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline uint64_t nx_monotonic_freq(void)
{
    /* CLOCK_MONOTONIC reports nanoseconds directly; one unit = one ns. */
    return 1000000000ULL;
}

#else   /* kernel: ARM generic timer */

static inline uint64_t nx_monotonic_raw(bool *ok)
{
    uint64_t v;
    asm volatile("mrs %0, cntpct_el0" : "=r"(v));
    if (ok) *ok = true;
    return v;
}

static inline uint64_t nx_monotonic_freq(void)
{
    uint64_t v;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

#endif

static inline void nx_deadline_start(struct nx_deadline *d, uint64_t budget_ns)
{
    bool ok = false;
    d->start    = nx_monotonic_raw(&ok);
    d->valid    = ok;

    /* threshold_units = budget_ns * freq / 1e9.  Split the multiplication
     * so budget values under a second don't overflow 64 bits at kernel
     * frequencies (QEMU virt: 62.5 MHz, i.e. budget_ns < 2^34 stays safe
     * even for the straight form — but writing it this way covers longer
     * budgets without relying on that headroom). */
    uint64_t freq = nx_monotonic_freq();
    d->threshold = (budget_ns / 1000000000ULL) * freq
                 + ((budget_ns % 1000000000ULL) * freq) / 1000000000ULL;
}

static inline bool nx_deadline_exceeded(const struct nx_deadline *d)
{
    if (!d->valid) return true;
    bool ok = false;
    uint64_t now = nx_monotonic_raw(&ok);
    if (!ok) return true;
    return (now - d->start) > d->threshold;
}

#endif /* NONUX_MONOTONIC_H */
