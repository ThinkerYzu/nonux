# nonux

A composable, Lego-like microkernel for ARM64. Swap components, benchmark designs, let AI build your kernel.

## Quick Start

```bash
# Prerequisites (Ubuntu/Debian)
sudo apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
                 qemu-system-arm make python3

# Build and run
make
make run

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

See `proj_docs/nonux/` for full project documentation:
- [SPEC.md](../../proj_docs/nonux/SPEC.md) — Requirements and constraints
- [DESIGN.md](../../proj_docs/nonux/DESIGN.md) — Architecture and design decisions
- [IMPLEMENTATION-GUIDE.md](../../proj_docs/nonux/IMPLEMENTATION-GUIDE.md) — Build guide and phase plan
- [HANDOFF.md](../../proj_docs/nonux/HANDOFF.md) — Current status and next actions

## License

MIT — see [LICENSE](LICENSE).
