# nonux

A composable, Lego-like microkernel for ARM64. Swap components, benchmark designs, let AI build your kernel.

## Quick Start

```bash
# Prerequisites (Ubuntu/Debian)
sudo apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
                 qemu-system-arm make python3

# Build and run
make
make run                  # interactive — Ctrl-A X to exit
tools/run-qemu.sh -t 5    # timed run (the kernel halts in wfe)

# Test
make test

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

## Documentation

Framework API reference (types, functions, examples) lives alongside
the code under [`docs/`](docs/):

- [Registry](docs/framework-registry.md) — slots, components, connections; events + change log + snapshots + JSON.
- [Component lifecycle](docs/framework-components.md) — six-verb state machine, `nx_component_ops`, pause protocol, dependency injection.
- [IPC router](docs/framework-ipc.md) — `nx_ipc_send` / dispatch, pause-policy routing, capabilities, `slot_ref_retain/release`.
- [Hook framework](docs/framework-hooks.md) — per-hook-point chains, typed contexts, mark-then-sweep unregister.
- [Tool chain](tools/README.md) — `gen-config.py`, `validate-config.py`, `verify-registry.py`.

Start at [`docs/README.md`](docs/README.md) for the index and the
shared error-code table.

## License

MIT — see [LICENSE](LICENSE).
