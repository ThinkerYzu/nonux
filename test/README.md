# test/

All test code. Three execution tiers — host, kernel (QEMU), and interactive —
plus a placeholder for benchmarks. Run with:

```sh
make test              # all tiers (tools + host + kernel)
make test-tools        # Python tool unit tests only
make test-host         # host-side C tests only
make test-kernel       # kernel-side tests under QEMU (semihosting exit)
make test-interactive  # scripted busybox shell sessions
```

## `host/`

Host-side C unit tests. Compiled for the **build machine** (x86-64), not
ARM64 — no QEMU needed, runs in milliseconds. Tests link against the same
framework and component C files as the kernel build but with a thin
`mem_track.c` shim replacing the kernel allocator so leaks are caught.

Structure:

| Path | Role |
|------|------|
| `main.c` / `test_runner.c/.h` | Test harness entry point. Discovers all registered test functions and runs them, printing a pass/fail count. |
| `mem_track.c/.h` | Heap allocation tracker. Wraps `malloc`/`free`; fails the test if the count is non-zero at teardown. |
| `*_test.c` | One file per module under test (e.g. `registry_test.c`, `handle_test.c`, `syscall_test.c`). |
| `slice_*.c` | Cross-cutting integration tests keyed to implementation slices (e.g. `slice_8_0a8_test.c`, `slice_9b_2_test.c`). |
| `conformance/` | Component conformance suites. `conformance_fs.c/.h`, `conformance_mm.c/.h`, `conformance_scheduler.c/.h` define standard test cases that every `fs`/`mm`/`scheduler` implementation must pass. Tests in `component_*_test.c` call the conformance suite for the component under test. |
| `fixtures/` | Small JSON fixtures used by `test_validate_config.py` and `test_gen_config.py` for tempdir-based end-to-end tests. |
| `gen/` | Copies of generated headers (`config.h`, per-component `_deps.h`) used by the host build. Kept separate from the kernel `gen/` to allow independent regeneration. |
| `mock_component.h` | Macro-based mock component builder. Used by integration tests that need a placeholder component to fill a slot without implementing the full interface. |
| `hook_inspector.h` | Hook-chain inspector for tests that verify hook registration order and call counts. |

## `kernel/`

In-kernel tests that boot under QEMU with `-semihosting`. Each `ktest_*.c`
file registers test cases with the `KTEST_*` macros from `ktest.h`; the main
runner (`ktest_main.c`) iterates them in registration order and exits via
ARM semihosting `SYS_EXIT_EXTENDED` with a pass/fail exit code that QEMU
propagates as its own exit status.

Notable groups:

| Pattern | Coverage |
|---------|----------|
| `ktest_bootstrap.c` | Component graph bring-up and slot wiring |
| `ktest_pmm.c`, `ktest_mm_buddy.c`, `ktest_mmu.c` | Physical and virtual memory |
| `ktest_sched*.c` | Scheduler round-robin, preemption, live swap |
| `ktest_el0.c`, `ktest_elf.c` | EL0 entry and ELF loader |
| `ktest_posix*.c` | POSIX syscall surface (open, read, write, fork, exec, wait, pipe, ppoll, signal) |
| `ktest_posix_busybox*.c` | Busybox shell commands (echo, cat, ls, mkdir, pipe, redirect, trap) |
| `ktest_posix_musl*.c` | musl-linked EL0 programs |
| `ktest_pause.c`, `ktest_recompose.c`, `ktest_live_swap.c` | Component pause/drain/resume and runtime recomposition |
| `ktest_config.c`, `ktest_conn_mode.c` | Runtime config manager and async↔sync mode switching |
| `ktest_9b_3.c` | Phase 9b slot-based resource handle routing |

EL0 test programs are compiled separately as AArch64 ELF binaries
(`*_prog.elf`) and embedded into `kernel-test.bin` via `*_prog_blob.S`
(`.incbin` directives). The ELF loader in `framework/elf.c` loads them into
the user address space at test time.

`init_prog.ld` is the shared linker script for EL0 test programs (base VA
`0x48000000`). `banner.txt` is embedded in the kernel image and printed on
boot so QEMU test output is recognisable in CI logs.

## `interactive/`

Scripted busybox shell sessions for smoke-testing the full POSIX stack
end-to-end. Each test is a pair of files:

| File | Role |
|------|------|
| `<name>.script` | Input fed to the busybox shell line by line via `tools/qemu-stdin-feed.sh` |
| `<name>.expected` | Expected output; the test runner diffs actual vs. expected |

`run.sh` boots `kernel-busybox.bin` under QEMU, feeds the script, captures
output, and compares. Run via `make test-interactive`.

Current scripts: `echo_hello`, `echo_cat`, `echo_pipe`, `ls_root`,
`mkdir_tmp`, `ps_smoke`, `visible_prompt`.

## `bench/`

Benchmark programs — placeholder for Phase 10. Empty for now; `make bench`
will boot a benchmark binary under QEMU and emit a reproducible timing report
when Phase 10 lands.
