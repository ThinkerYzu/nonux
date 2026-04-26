/*
 * Slice 7.6d.3a — EL0 segfault demo: convert data abort to SIGSEGV.
 *
 * Parent forks; child writes to a known-unmapped EL0 VA (PA 0x0
 * lives in slot 0 of L2_mmio with `device_block` perms — UXN + AP =
 * EL1-only, so any EL0 write faults with EC=0x24, DFSC=permission).
 * `on_sync` (slice 7.6d.3a) converts the lower-EL data abort into
 * `nx_process_exit(128 + SIGSEGV) = 139`.  Parent's `wait()` collects
 * the 139 and emits `[segv-ok]`.  Three markers expected:
 * `[segv-parent][segv-child][segv-ok]`.
 *
 * Without the slice-7.6d.3a fault-conversion, the kernel would
 * `halt_forever` on the EL0 fault and the test would never reach
 * `[segv-ok]` — that's the contract this demo locks in.
 */

#include "components/posix_shim/nxlibc.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    nxlibc_pid_t pid = nxlibc_fork();
    if (pid < 0) nxlibc_exit(1);

    if (pid == 0) {
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[segv-child]", 12);
        /* Write to PA 0 via VA 0 — guaranteed permission fault from
         * EL0 (device_block, AP=EL1-only). */
        *(volatile int *)0 = 42;
        /* Unreachable.  Sentinel exit if the kernel ever stops
         * converting EL0 data aborts into SIGSEGV. */
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[segv-NOT-REACHED]", 18);
        nxlibc_exit(99);
    }

    nxlibc_write(NXLIBC_STDOUT_FILENO, "[segv-parent]", 13);
    int status = 0;
    (void)nxlibc_waitpid(pid, &status, 0);
    if (status == 128 + 11)   /* SIGSEGV */
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[segv-ok]", 9);
    nxlibc_exit(0);
}
