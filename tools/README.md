# nonux build tooling

Small Python tool chain that drives the nonux build. Two scripts:

- **`gen-config.py`** — code generator. Stdlib-only; emits C and Make
  artefacts from JSON inputs.
- **`validate-config.py`** — whole-tree validator. Needs the venv
  (depends on `jsonschema`).

Plus a shell runner:

- **`run-qemu.sh`** — launches `kernel.bin` under QEMU. See the script
  header for flag reference.

Output is **deterministic** — sorted keys everywhere, so regenerating
from an unchanged input produces byte-identical output. Slice 3.7's
`verify-registry.py` will rely on this for its R7 regenerate-and-diff
gate.

---

## Setup

Both tools use the project virtualenv at `sources/nonux/.venv/`.

```sh
make venv
```

Creates `.venv/` if missing, installs `tools/requirements.txt`
(currently just `jsonschema>=4.0`), and stamps
`.venv/.installed` as a sentinel. Safe to re-run.

The generator (`gen-config.py`) works without the venv — stdlib only.
The validator (`validate-config.py`) needs `jsonschema` and will tell
you to run `make venv` if the import fails.

---

## `gen-config.py`

Two subcommands — **independent** (neither reads the other's output).
For a full build you need both to have run before compilation: each
component's `.c` includes `gen/<name>_deps.h` (from `manifest`) and
`gen/config.h` (from `kernel`).

### `gen-config.py manifest <manifest.json> <outdir>`

Reads one per-component `manifest.json` and emits
`<outdir>/<name>_deps.h`, which contains:

- **`struct <name>_deps`** — one `struct nx_slot *` field per
  `requires` / `optional` entry in the manifest. Embedded in the
  component's state struct.
- **`<NAME>_DEPS_TABLE(CONTAINER, FIELD)`** — macro expanding to a
  comma-separated list of `nx_dep_descriptor` initialisers using
  `offsetof(CONTAINER, FIELD.<dep>)` to record each field's offset
  from the container root.

The component author feeds these into `NX_COMPONENT_REGISTER` (see
`framework/component.h`); the descriptor then lands in the
`nx_components` linker section and `nx_resolve_deps()` walks the dep
table at boot.

**Accepted manifest shape (v1 — schema in `tools/schemas/manifest.schema.json`):**

```json
{
  "name": "sched_rr",
  "version": "0.1.0",
  "requires": {
    "timer": {
      "version":  ">=0.1.0",
      "mode":     "async",
      "stateful": false,
      "policy":   "queue"
    }
  },
  "optional": {
    "stats": { "mode": "sync", "stateful": true }
  }
}
```

Defaults match `DESIGN.md`: `mode=async`, `stateful=false`,
`policy=queue`. Only `>=X.Y.Z` version constraints are supported in
v1.

**Exit codes:** `0` on success; `2` on malformed JSON, unknown
`mode` / `policy`, non-boolean `stateful`, duplicate dep name across
`requires`/`optional`.

**Example** — regenerate one deps header:

```sh
.venv/bin/python3 tools/gen-config.py manifest \
    components/sched_rr/manifest.json gen/
```

### `gen-config.py kernel <kernel.json> <components_dir> <outdir>`

Reads the top-level `kernel.json` and emits two files:

- **`<outdir>/config.h`** — kernel config `#define`s:
  - `NX_TARGET` — the target triple (e.g. `"aarch64-qemu-virt"`)
  - `NX_SLOT_<SLOT>_IMPL` — per-slot binding. Dotted slot names
    (`char_device.serial`) flatten to underscore form
    (`CHAR_DEVICE_SERIAL`).
  - `NX_CONFIG_<COMP>_<KEY>` — per-component config values from each
    binding's `config` block. Literal shapes:
    - integers verbatim: `10`
    - bools → `1` / `0`
    - hex-looking strings (`"0x09000000"`) → integer literals
    - plain strings → JSON-escape-quoted C string literal
- **`<outdir>/sources.mk`** — deduplicated, sorted
  `COMPONENT_SRCS := components/<impl>/<impl>.c \ …` list for
  `-include` by the top-level Makefile.

`components_dir` is accepted but unused today — reserved for slice
3.7's cross-manifest checks; pass `components/`.

**Accepted kernel.json shape (v1 — schema in `tools/schemas/kernel.schema.json`):**

```json
{
  "target": "aarch64-qemu-virt",
  "components": {
    "scheduler": {
      "impl":   "sched_rr",
      "config": { "time_quantum_ms": 10 }
    },
    "char_device.serial": {
      "impl":   "uart_pl011",
      "config": { "base_addr": "0x09000000" }
    }
  },
  "connections": [
    { "from": "posix_shim", "to": "vfs", "mode": "async" }
  ]
}
```

**Exit codes:** `0` on success; `2` on malformed JSON, missing
`target` / `components`, unknown `impl` format, or unsupported
`config` value types.

**Invoke via the Makefile:**

```sh
make kernel-config      # phony target — emits both files
```

`make kernel-config` is deliberately phony (the files are a recipe
*side-effect*, not named outputs). If the rule named them as outputs,
the top-level `-include gen/sources.mk` would auto-trigger a rebuild
that cascades into missing-source link errors until every component
referenced in `kernel.json` actually exists (Phase 4+).

---

## `validate-config.py`

Whole-tree validation. Walks `kernel.json` + every referenced
`components/<impl>/manifest.json` and runs four checks in order,
accumulating errors so one invocation surfaces everything:

1. **Schema** — both files against
   `tools/schemas/{kernel,manifest}.schema.json` (JSON Schema
   Draft 2020-12 via the `jsonschema` library).
2. **Existence** — every `impl` named in `kernel.json` has a
   `components/<impl>/manifest.json` whose `name` matches the
   directory.
3. **Versions** — `>=X.Y.Z` constraints on each dep's `version` are
   satisfied by the target component's top-level `version`.
4. **Cycles** — iterative DFS over the slot→dep-slot graph (both
   `requires` and `optional` edges, because a cycle through
   optionals is still a boot-order impossibility).

**CLI:**

```
validate-config.py <kernel.json> <components_dir> [--deps | --deps-dot]
```

- `--deps` prints one resolved edge per line (`scheduler -> timer`).
- `--deps-dot` prints a graphviz `digraph { ... }` for piping into
  `dot`.

Both flags still run all four checks; the report just adds output.
Validation failures still drive the exit code, so `make deps` won't
hide a real problem behind a report.

**Exit codes:** `0` on success; `2` on any validation failure.

**Makefile targets (all of which auto-install the venv on first
use via `$(VENV_STAMP)`):**

```sh
make validate-config    # just the checks
make deps               # checks + --deps
make deps-dot           # checks + --deps-dot
```

---

## Unit tests

31 stdlib-unittest tests under `tools/tests/`:

- `test_gen_config.py` — 15 tests covering both subcommands
- `test_validate_config.py` — 16 tests covering version parsing,
  cycle detection, and 8 end-to-end tempdir-based fixtures

Run via:

```sh
make test-tools
```

which is also hooked into `make test` alongside `test-host` /
`test-kernel`. Stdlib `unittest` (no pytest dep) — tests use
`importlib.util.spec_from_file_location` to import the hyphenated
tool filenames (`gen-config.py`, `validate-config.py`) as modules.

---

## See also

- [`framework/component.h`](../framework/component.h) — the
  `NX_COMPONENT_REGISTER` macro and descriptor types consumed by
  generated deps headers.
- [`tools/schemas/`](schemas/) — the authoritative JSON Schema
  definitions for both manifest and kernel config.
- [`proj_docs/nonux/DESIGN.md`](../../proj_docs/nonux/DESIGN.md)
  §Dependency Injection Mechanism — architectural rationale.
- Session logs
  [`session-10-dependency-injection.md`](../../proj_docs/nonux/logs/session-10-dependency-injection.md)
  (slice 3.4) and
  [`session-11-config-tooling.md`](../../proj_docs/nonux/logs/session-11-config-tooling.md)
  (slice 3.5) — design decisions and known gotchas.
