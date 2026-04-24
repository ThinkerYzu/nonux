# posix_shim

Header-only POSIX-style C wrappers over the `NX_SYS_*` syscall surface.
Introduced in slice 7.4d; the spine of any future user-C code that
wants to target nonux without hand-rolling `svc #0` asm.

## Interface

- **iface:** *none* — posix_shim doesn't bind to a kernel slot.  It
  lives entirely on the EL0 side of the boundary.
- **Bound by default:** *no* — **not** referenced in `kernel.json`
  and **not** linked into `kernel.bin`.  EL0 C programs `#include
  "components/posix_shim/posix.h"` directly; the kernel stays
  unaware.
- **Dependencies:** none.
- **Worker threads:** none.
- **Pause hook:** not applicable.

## Shape

Every entry is a `static inline` function in `posix.h` that emits
`svc #0` with the right `NX_SYS_*` number in `x8` and arguments in
`x0..x5`, matching the calling convention in `framework/syscall.h`.
No `.c` file, no link dependency — the header is the whole
implementation.

```c
static inline int64_t
nx_posix_debug_write(const char *buf, size_t len);

static inline void __attribute__((noreturn))
nx_posix_exit(int status);

static inline nx_posix_pid_t  nx_posix_fork(void);
static inline int             nx_posix_execve(const char *path,
                                              char *const argv[],
                                              char *const envp[]);
static inline nx_posix_pid_t  nx_posix_waitpid(nx_posix_pid_t pid,
                                               int *status,
                                               int options);

static inline nx_posix_fd_t   nx_posix_open(const char *path,
                                            int flags, int mode);
static inline int             nx_posix_close(nx_posix_fd_t fd);
static inline nx_posix_ssize_t
                              nx_posix_read(nx_posix_fd_t fd,
                                            void *buf, size_t len);
static inline nx_posix_ssize_t
                              nx_posix_write(nx_posix_fd_t fd,
                                             const void *buf,
                                             size_t len);
static inline int64_t         nx_posix_lseek(nx_posix_fd_t fd,
                                             int64_t offset,
                                             int whence);
```

Type aliases (`nx_posix_pid_t`, `nx_posix_fd_t`, `nx_posix_ssize_t`)
are plain `int` / `int64_t` so the header compiles cleanly under
`-ffreestanding -nostdlib` without pulling in any system headers.
`posix.h`'s only `#include`s are `<stddef.h>` and `<stdint.h>`,
both freestanding.

## Why header-only

A `.c` file would need a compile + link + component-registration
pass against `kernel.bin`, which is wrong for two reasons:

1. **The code belongs to EL0, not EL1.**  A `.c` file
   compiled into `kernel.bin` would add kernel text for something
   that never runs at EL1 — just the SVC wrappers get trapped as
   "unreachable EL1 branches".
2. **Different build flags.**  EL0 binaries are linked at the
   user window VA (`0x48000000`, per `test/kernel/init_prog.ld`);
   kernel code is linked at `0x40080000`.  Mixing them in one
   linker pass requires contortions that buy nothing.

A `static inline` header gets compiled into whichever EL0 binary
includes it, at whatever flags that binary uses — which is
exactly what we want.

## Mapping to POSIX

Best-effort for v1, with deliberate gaps:

| POSIX idiom                 | v1 handling |
|-----------------------------|-------------|
| `errno` / return `-1`       | raw `nx_status_t` return; caller tests negative |
| `execve(path, argv, envp)`  | argv + envp accepted but ignored (NX_SYS_EXEC takes path only) |
| `WIFEXITED` / `WEXITSTATUS` | `*status` receives exit_code directly |
| `signal(...)`               | absent until slice 7.5 |
| `getpid()`                  | absent; no NX_SYS_GETPID yet |
| `fstat` / `stat`            | absent; no NX_SYS_STAT yet |

The translation layer (errno, WIFEXITED, etc.) is deferred to a
thin libc-compat wrapper that a later slice (or an externally
cross-compiled libc like musl) can layer on top of posix_shim.
Separating the ABI surface (posix_shim) from the API ergonomics
(errno, macros) keeps the v1 surface minimal.

## Test

`test/kernel/posix_prog.c` is a tiny EL0 C program that uses the
wrappers to fork + child exit(23) + parent wait + compare status +
emit a marker.  Built with `-ffreestanding -nostdlib
-mgeneral-regs-only -fno-pic` using the shared
`test/kernel/init_prog.ld` linker script, then embedded into
`kernel-test.bin` via `posix_prog_blob.S`'s `.incbin`.
`ktest_posix.c` loads the blob via the slice 7.3 ELF loader, drops
to EL0, and verifies all three markers (`[posix-parent]`,
`[posix-child]`, `[posix-ok]`) appear plus an EXITED process with
`exit_code == 23` is in the process table afterwards.

A failure mode worth noting: if the wrappers emit the wrong
syscall number (e.g. mis-alias between the POSIX and NX enum), the
markers either won't fire or the wait will see the wrong exit code.
Either makes the ktest fail loudly — so the C-level ABI is
regression-tested every run.

## What's NOT here

- A crt0 setting up argc/argv/envp for `main()`.  The demo program
  uses `_start` as its entry directly.  `crt0.S` lands when a
  third-party EL0 program (busybox, slice 7.6) expects POSIX-style
  `main(argc, argv)`.
- A `libc.a` wrapping `posix_*` as `open`/`read`/`write`/etc.
  Deliberately deferred — external busybox-style code will link
  against a real libc (musl / uclibc) mapping those names into
  posix_shim-like SVCs through the libc's own syscall table.
- `fork` + `clone`-style flag variants.  v1 has exactly one fork
  semantic; POSIX's flag combinations layer on top.

## Slice history

- **7.4d (introduced):** header + demo + ktest; no kernel-side
  changes.
