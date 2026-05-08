# lib/

Userspace libraries compiled into EL0 programs. Nothing here is linked into
`kernel.bin` — the kernel core lives in `core/lib/` and `framework/`.

## `libnxlibc/`

POSIX-style C wrappers and EL0 C-runtime bootstrap for programs targeting
nonux.

See [`libnxlibc/README.md`](libnxlibc/README.md) for the full interface
reference.

**In brief:**

- **`posix.h`** — header-only `static inline` syscall wrappers
  (`nx_posix_open`, `nx_posix_read`, `nx_posix_write`, `nx_posix_fork`,
  `nx_posix_execve`, `nx_posix_waitpid`, …). Each wrapper issues `svc #0`
  with the corresponding `NX_SYS_*` number in `x8`. No `.c` file; the
  header is the entire implementation. Include with
  `-ffreestanding -nostdlib`.
- **`crt0.S`** — C runtime zero-stage. Sets up `argc`/`argv`/`envp` from the
  kernel-pushed EL0 entry frame and calls `main()`. Linked into EL0 programs
  that use the standard `main(int argc, char *argv[])` entry convention.
- **`nxlibc.c/.h`** — thin C helpers layered on `posix.h`: formatted output,
  string utilities, and small libc-compat stubs that EL0 test programs need
  without pulling in a full libc.
- **`manifest.json`** — component manifest (no slot dependencies; not wired
  into `kernel.json`). Present so `tools/validate-config.py` can verify the
  manifest schema if needed.

> **History:** relocated from `components/libnxlibc/` to `lib/libnxlibc/` in
> Session 117. `components/` now holds only kernel slot implementations.
