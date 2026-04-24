/*
 * Slice 7.5 — EL0 C demo for NX_SYS_SIGNAL (SIGTERM delivery).
 *
 * Parent forks; child busy-loops; parent sends SIGTERM to child +
 * waitpid; asserts child exited with status `128 + NX_POSIX_SIGTERM
 * == 143`; emits `[signal-ok]` + `exit(31)`.
 *
 * Delivery in v1 is polled — `sched_check_resched` checks the
 * current process's `pending_signals` and, if SIGKILL or SIGTERM
 * is set, routes inline to `nx_process_exit(128 + signo)`.  That
 * means the child only dies on its next timer-preempt / yield.
 * The busy loop ensures the timer tick still fires: each iteration
 * returns to EL0 immediately, and the quantum-based preemption
 * path eventually eats the signal on the child's behalf.
 */

#include "components/posix_shim/posix.h"

static int g_dummy;  /* forces the busy loop to have side-effects
                      * so -O2 can't optimise it away. */

void __attribute__((noreturn)) _start(void)
{
    nx_posix_pid_t pid = nx_posix_fork();

    if (pid == 0) {
        /* Child: emit a presence marker, then busy-loop.  Signals
         * get delivered the next time the scheduler looks at us. */
        nx_posix_debug_write("[signal-child]", 14);
        for (;;) g_dummy++;
    }

    /* Parent.  Give the child a beat to emit its marker (a few
     * yields go through the scheduler, so the child will be
     * scheduled at least once), then send SIGTERM. */
    nx_posix_debug_write("[signal-parent]", 15);

    /* A short busy-yield: stop once we've seen the scheduler
     * rotate at least once, which implies the child has emitted
     * its marker.  v1 has no usleep / nanosleep; this is the
     * crude equivalent. */
    for (volatile int i = 0; i < 2048; i++) { (void)i; }

    if (nx_posix_kill(pid, NX_POSIX_SIGTERM) != 0) nx_posix_exit(1);

    int status = 0;
    (void)nx_posix_waitpid(pid, &status, 0);

    if (status == 128 + NX_POSIX_SIGTERM) {
        nx_posix_debug_write("[signal-ok]", 11);
    }

    nx_posix_exit(31);
}
