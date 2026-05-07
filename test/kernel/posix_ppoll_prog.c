/*
 * Slice 7.8b — EL0 demo for NX_SYS_PPOLL.
 *
 * Three subtests in one program:
 *
 *   1. Initial readiness.  Open a pipe, write a byte, ppoll the
 *      read end with timeout=0 (non-blocking).  ppoll's
 *      registered-listener-then-readiness-check sequence sees
 *      POLLIN immediately and returns 1.
 *
 *   2. Deadline expiry.  Open a fresh empty pipe, ppoll with a
 *      50 ms timeout, no producer.  ppoll blocks on the pollset
 *      waitq; the deadline fires from `nx_waitq_tick_deadlines`
 *      (called from sched_tick) and the wait returns
 *      NX_EDEADLINE; ppoll's post-sleep recheck sees no
 *      readiness and returns 0.
 *
 *   3. Peer-close hangup.  Close the pipe's write end, then
 *      ppoll the read end (non-blocking).  POSIX semantics:
 *      peer-closed pipe is readable (read returns 0=EOF) AND
 *      reports POLLHUP.  Channel readiness logic in
 *      `nx_channel_endpoint_readiness` returns POLLIN | POLLHUP.
 *
 * Emits `[ppoll-ok]` and exits 29 only if all three subtests
 * pass.  Fails with a discrete exit code per subtest so the
 * matching ktest can pinpoint regressions.
 */

#include "lib/libnxlibc/posix.h"

void __attribute__((noreturn)) _start(void)
{
    /* ---------- Test 1: initial readiness ------------------------ */
    int p[2] = { -1, -1 };
    if (nx_posix_pipe(p) != 0) nx_posix_exit(1);

    /* Write a byte before polling so the initial-readiness check
     * has something to find.  Avoids any wake-during-block race in
     * a single-process test. */
    if (nx_posix_write(p[1], "x", 1) != 1) nx_posix_exit(2);

    struct nx_posix_pollfd pf;
    pf.fd      = p[0];
    pf.events  = NX_POSIX_POLLIN;
    pf.revents = 0;

    struct nx_posix_timespec zero = { .tv_sec = 0, .tv_nsec = 0 };
    int n = nx_posix_ppoll(&pf, 1, &zero, 0);
    if (n != 1) nx_posix_exit(3);
    if (!(pf.revents & NX_POSIX_POLLIN)) nx_posix_exit(4);

    /* Drain the byte so subsequent polls on this fd see empty. */
    char drain;
    if (nx_posix_read(p[0], &drain, 1) != 1) nx_posix_exit(5);
    nx_posix_close(p[0]);
    nx_posix_close(p[1]);

    /* ---------- Test 2: deadline expiry -------------------------- */
    int p2[2] = { -1, -1 };
    if (nx_posix_pipe(p2) != 0) nx_posix_exit(6);

    pf.fd      = p2[0];
    pf.events  = NX_POSIX_POLLIN;
    pf.revents = 0;

    /* 50 ms — long enough for the 10 Hz timer to fire at least
     * once but well within the QEMU-test 90 s budget. */
    struct nx_posix_timespec t50ms = { .tv_sec = 0, .tv_nsec = 50000000 };
    n = nx_posix_ppoll(&pf, 1, &t50ms, 0);
    if (n != 0) nx_posix_exit(7);
    if (pf.revents != 0) nx_posix_exit(8);

    /* ---------- Test 3: peer-close hangup ------------------------ */
    nx_posix_close(p2[1]);   /* writer side closes — peer becomes EOF */

    pf.fd      = p2[0];
    pf.events  = NX_POSIX_POLLIN;
    pf.revents = 0;

    n = nx_posix_ppoll(&pf, 1, &zero, 0);
    if (n != 1) nx_posix_exit(9);
    /* POSIX poll: peer-closed pipe reports POLLIN (read-returns-0
     * indicator) and POLLHUP.  Either bit being set means the
     * caller's recv loop will discover EOF; require both for a
     * v1 channel. */
    if (!(pf.revents & NX_POSIX_POLLIN))  nx_posix_exit(10);
    if (!(pf.revents & NX_POSIX_POLLHUP)) nx_posix_exit(11);

    nx_posix_close(p2[0]);

    nx_posix_debug_write("[ppoll-ok]", 10);
    nx_posix_exit(29);
}
