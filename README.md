# nonux

A composable, Lego-like microkernel for ARM64. Swap components, benchmark designs, let AI build your kernel.

> **Documentation:** [github.com/ThinkerYzu/nonux-meta](https://github.com/ThinkerYzu/nonux-meta) — companion repo with spec, design, implementation guide, handoff package, and per-session work logs.  Current status, phase progress, and test counts live in [HANDOFF.md](https://github.com/ThinkerYzu/nonux-meta/blob/master/HANDOFF.md).

## Quick Start

```bash
# Clone
git clone git@github.com:ThinkerYzu/nonux.git
cd nonux

# Prerequisites (Ubuntu/Debian)
sudo apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
                 qemu-system-arm make python3

# Build and run
make
make run-busybox          # interactive busybox shell over UART — Ctrl-A X to exit
tools/run-qemu.sh -t 5    # timed run (the kernel halts in wfe)

# Test
make test                 # 471/471 green
make test-interactive     # canned scripts piped into the busybox shell

# Validate config
make validate-config
```

## What is this?

nonux is a microkernel where every subsystem (scheduler, memory manager, filesystem, drivers) is a swappable component with a clean interface and a JSON manifest. Configure which components to use in `kernel.json`, build, boot.

**Key properties:**
- **Composable** — snap components together like Lego
- **AI-operable** — JSON manifests + declarative config = AI can build kernels
- **Testable** — every component tested independently with memory tracking
- **Hot-swappable** — replace components at runtime
- **Permissively licensed** — MIT

## Directory Structure

| Directory | Role |
|-----------|------|
| [`core/`](core/README.md) | Platform primitives: boot entry, CPU/exception vectors, IRQ/GIC driver, MMU, PMM, kernel heap, wait-queues, timer. ARM64-specific; no framework dependency. |
| [`framework/`](framework/README.md) | Component framework: registry, lifecycle, async IPC router, dispatcher kthread, sync slot-call, hook chains, ELF loader, syscall dispatch, process/handle tables, recomposition orchestrator, runtime config manager. IDL-generated call/dispatch shims also live here. |
| [`components/`](components/README.md) | Pluggable implementations that fill kernel slots: `sched_rr`, `sched_priority`, `mm_buddy`, `ramfs`, `procfs`, `vfs_simple`, `uart_pl011`, `posix_shim`. Each has a `manifest.json` and a `README.md`. |
| [`interfaces/`](interfaces/README.md) | IDL-generated interface headers (`*.h`, `*_msg.h`) for every slot type. JSON IDL sources live in `interfaces/idl/`. Do not edit the generated headers directly. |
| [`lib/`](lib/README.md) | Userspace libraries for EL0 programs. Currently: `libnxlibc/` — header-only POSIX syscall wrappers + `crt0.S` + thin C helpers. Not linked into `kernel.bin`. |
| [`gen/`](gen/README.md) | Build-time generated files: `config.h`, `sources.mk`, `slot_table.c`, and per-component `*_deps.h`. Outputs of `tools/gen-config.py` — do not edit by hand. |
| [`test/`](test/README.md) | All tests: `host/` (native C unit tests), `kernel/` (in-QEMU ktests + EL0 program blobs), `interactive/` (scripted busybox shell sessions), `bench/` (Phase 10 placeholder). |
| [`tools/`](tools/README.md) | Build toolchain: `gen-config.py`, `gen-iface.py`, `validate-config.py`, `verify-registry.py`, `run-qemu.sh`, JSON schemas, and tool unit tests. |
| [`docs/`](docs/README.md) | Framework API reference docs: registry, component lifecycle, IPC router, hook framework, bootstrap. |
| [`third_party/`](third_party/README.md) | Pre-built third-party binaries: busybox (ARM64 static) and musl libc headers/library for EL0 integration tests. |

## Documentation

Framework API reference (types, functions, examples) lives alongside
the code under [`docs/`](docs/):

- [Registry](docs/framework-registry.md) — slots, components, connections; events + change log + snapshots + JSON.
- [Component lifecycle](docs/framework-components.md) — six-verb state machine, `nx_component_ops`, pause protocol, dependency injection.
- [IPC router](docs/framework-ipc.md) — `nx_ipc_send` / dispatch, pause-policy routing, capabilities, `slot_ref_retain/release`.
- [Hook framework](docs/framework-hooks.md) — per-hook-point chains, typed contexts, mark-then-sweep unregister.
- [Tool chain](tools/README.md) — `gen-config.py`, `gen-iface.py`, `validate-config.py`, `verify-registry.py`.

Start at [`docs/README.md`](docs/README.md) for the index and the
shared error-code table.

## License

MIT — see [LICENSE](LICENSE).
