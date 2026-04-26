/*
 * Slice 7.6d.3a — EL0 undefined-instruction demo: convert EC=0x00
 * (Unknown reason) to SIGILL.
 *
 * Parent forks; child executes `.word 0` (encoding `0x00000000`,
 * which is `udf #0` — the canonical "this is never a valid
 * instruction" 32-bit aarch64 word).  The CPU traps with EC=0x00
 * (Unknown reason) — `on_sync`'s slice-7.6d.3a logic checks
 * `tf->pstate` to confirm the saved EL was EL0t, then calls
 * `nx_process_exit(128 + SIGILL) = 132`.  Parent's `wait()`
 * collects the 132 and emits `[undef-ok]`.  Three markers expected:
 * `[undef-parent][undef-child][undef-ok]`.
 *
 * EC=0x00 from EL1 (kernel undef) stays a panic — it's a kernel bug,
 * not user-program behaviour.  This demo only exercises the EL0 path.
 */

#include "components/posix_shim/nxlibc.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    nxlibc_pid_t pid = nxlibc_fork();
    if (pid < 0) nxlibc_exit(1);

    if (pid == 0) {
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[undef-child]", 13);
        asm volatile (".word 0");   /* udf #0 → SIGILL → exit 132 */
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[undef-NOT-REACHED]", 19);
        nxlibc_exit(99);
    }

    nxlibc_write(NXLIBC_STDOUT_FILENO, "[undef-parent]", 14);
    int status = 0;
    (void)nxlibc_waitpid(pid, &status, 0);
    if (status == 128 + 4)   /* SIGILL */
        nxlibc_write(NXLIBC_STDOUT_FILENO, "[undef-ok]", 10);
    nxlibc_exit(0);
}
