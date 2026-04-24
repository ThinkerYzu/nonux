/*
 * Slice 7.4d — EL0 C demo program for posix_shim.
 *
 * Compiled as its own aarch64 EL0 binary (linked at VA 0x48000000
 * via the shared init_prog.ld), then embedded into
 * kernel-test.bin through posix_prog_blob.S's `.incbin`.
 * ktest_posix loads the blob via the slice 7.3 ELF loader and
 * drops to EL0.
 *
 * Program flow:
 *   1. posix_fork().
 *   2. Child branch (rv == 0):
 *        - posix_debug_write("[posix-child]", 13).
 *        - posix_exit(23).
 *   3. Parent branch (rv > 0):
 *        - posix_debug_write("[posix-parent]", 14).
 *        - posix_waitpid(child_pid, &status, 0).
 *        - if (status == 23) posix_debug_write("[posix-ok]", 10).
 *        - posix_exit(0).
 *
 * Four-marker invariant the ktest pins to:
 *   [posix-parent] → [posix-child] → [posix-ok].  (Order of the
 *   first two is scheduler-dependent — parent and child race to
 *   emit; the test only checks that all three appear.)
 *
 * Built with `-ffreestanding -nostdlib -mgeneral-regs-only` so no
 * crt0 / no libc / no outline atomics.  The function named
 * `_start` is the ELF entry point (init_prog.ld sets ENTRY(_start)
 * for all standalone EL0 binaries).  drop_to_el0 sets sp_el0 to
 * the top of the user window before eret, so `_start` has a valid
 * stack from its first instruction.
 */

#include "components/posix_shim/posix.h"

void __attribute__((noreturn)) _start(void)
{
    nx_posix_pid_t pid = nx_posix_fork();

    if (pid == 0) {
        /* Child branch. */
        nx_posix_debug_write("[posix-child]", 13);
        nx_posix_exit(23);
    }

    /* Parent branch.  Negative pid would mean fork failed; v1's
     * fork can fail on nx_process_fork's address-space alloc but
     * not on the single-CPU happy path the ktest exercises. */
    nx_posix_debug_write("[posix-parent]", 14);

    int status = 0;
    (void)nx_posix_waitpid(pid, &status, 0);

    if (status == 23) {
        nx_posix_debug_write("[posix-ok]", 10);
    }

    nx_posix_exit(0);
}
