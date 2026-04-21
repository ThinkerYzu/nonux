#!/usr/bin/env python3
"""
gen-config.py — Phase 3 slice 3.4 minimum.

Read a single component manifest.json and emit `gen/<name>_deps.h`.
This header contains:

  1. `struct <name>_deps` — one `struct nx_slot *` field per manifest
     entry (required + optional).
  2. `<NAME>_DEPS_TABLE(CONTAINER, FIELD)` — macro that expands to a
     comma-separated list of `nx_dep_descriptor` initialisers, using
     `offsetof()` into the caller-supplied container type.

Output is deterministic: dependencies are emitted in sorted order so the
regenerate-and-diff check planned for `verify-registry.py` (R7) produces
byte-identical output across runs.

Slice 3.5 will extend this tool to consume a full `kernel.json`, emit
`gen/config.h` and `gen/sources.mk`, and add real schema validation.
For 3.4 the scope is intentionally narrow: one manifest → one deps.h.

Manifest schema (minimum):

  {
    "name":    "sched_rr",               # required
    "version": "0.1.0",                  # optional, unused in 3.4
    "requires": {                        # optional — required deps
      "timer": {
        "version":  ">=0.1.0",           # optional
        "mode":     "async" | "sync",    # optional, default "async"
        "stateful": true | false,        # optional, default false
        "policy":   "queue" | "reject" | "redirect"  # optional, default "queue"
      }
    },
    "optional": {                        # optional — optional deps
      "stats": { ... same shape ... }
    }
  }
"""

import argparse
import json
import pathlib
import sys

MODE_MAP = {
    "async": "NX_CONN_ASYNC",
    "sync":  "NX_CONN_SYNC",
}

POLICY_MAP = {
    "queue":    "NX_PAUSE_QUEUE",
    "reject":   "NX_PAUSE_REJECT",
    "redirect": "NX_PAUSE_REDIRECT",
}


class ManifestError(Exception):
    """Raised when a manifest fails validation."""


def parse_dep(dep_name: str, spec: dict, required: bool) -> dict:
    """Normalise one requires/optional entry into a fully-populated dict."""
    if not isinstance(spec, dict):
        raise ManifestError(
            f"dep {dep_name!r}: expected object, got {type(spec).__name__}")

    mode = spec.get("mode", "async")
    if mode not in MODE_MAP:
        raise ManifestError(
            f"dep {dep_name!r}: mode must be 'async' or 'sync', got {mode!r}")

    policy = spec.get("policy", "queue")
    if policy not in POLICY_MAP:
        raise ManifestError(
            f"dep {dep_name!r}: policy must be 'queue', 'reject', or "
            f"'redirect', got {policy!r}")

    stateful = spec.get("stateful", False)
    if not isinstance(stateful, bool):
        raise ManifestError(
            f"dep {dep_name!r}: stateful must be boolean, got {stateful!r}")

    version_req = spec.get("version")
    if version_req is not None and not isinstance(version_req, str):
        raise ManifestError(
            f"dep {dep_name!r}: version must be string, got "
            f"{type(version_req).__name__}")

    return {
        "name":        dep_name,
        "required":    required,
        "version_req": version_req,
        "mode":        MODE_MAP[mode],
        "stateful":    stateful,
        "policy":      POLICY_MAP[policy],
    }


def load_manifest(path: pathlib.Path) -> dict:
    try:
        raw = json.loads(path.read_text())
    except json.JSONDecodeError as e:
        raise ManifestError(f"{path}: invalid JSON: {e}") from e

    if not isinstance(raw, dict):
        raise ManifestError(f"{path}: top-level must be an object")
    if "name" not in raw:
        raise ManifestError(f"{path}: missing required field 'name'")
    if not isinstance(raw["name"], str) or not raw["name"]:
        raise ManifestError(f"{path}: 'name' must be a non-empty string")

    # Deterministic ordering: sorted by dep name, required before optional
    # on name collision (which validate-config.py in slice 3.5 will
    # flag — for 3.4 we just emit required first).
    deps: list[dict] = []
    seen: set[str] = set()

    for dep_name in sorted(raw.get("requires", {}).keys()):
        if dep_name in seen:
            raise ManifestError(f"duplicate dep name: {dep_name!r}")
        seen.add(dep_name)
        deps.append(parse_dep(dep_name, raw["requires"][dep_name], True))

    for dep_name in sorted(raw.get("optional", {}).keys()):
        if dep_name in seen:
            raise ManifestError(
                f"dep {dep_name!r} listed in both requires and optional")
        seen.add(dep_name)
        deps.append(parse_dep(dep_name, raw["optional"][dep_name], False))

    return {"name": raw["name"], "deps": deps}


def c_ident(name: str) -> str:
    """Manifest names are already valid C identifiers; this is a sanity hook."""
    if not name.replace("_", "").isalnum():
        raise ManifestError(f"name {name!r} is not a valid C identifier")
    return name


def render_header(manifest: dict, manifest_path: pathlib.Path) -> str:
    name  = c_ident(manifest["name"])
    upper = name.upper()
    guard = f"NX_GEN_{upper}_DEPS_H"
    deps  = manifest["deps"]

    lines: list[str] = []
    lines.append(
        f"/* Auto-generated by tools/gen-config.py from "
        f"{manifest_path.name}. Do not edit. */")
    lines.append(f"#ifndef {guard}")
    lines.append(f"#define {guard}")
    lines.append("")
    lines.append('#include "framework/registry.h"')
    lines.append("")
    lines.append("#include <stdbool.h>")
    lines.append("#include <stddef.h>")
    lines.append("")

    # Struct
    lines.append(f"struct {name}_deps {{")
    if deps:
        for d in deps:
            tag = "required" if d["required"] else "optional"
            ver = f' — "{d["version_req"]}"' if d["version_req"] else ""
            lines.append(
                f"    struct nx_slot *{d['name']};"
                f"   /* {tag}{ver} */")
    else:
        # C forbids empty structs; emit a sentinel so the struct compiles
        # and every component can uniformly embed a `_deps` field even
        # when the manifest declares none.
        lines.append("    char _nx_no_deps;  /* placeholder — manifest has no deps */")
    lines.append("};")
    lines.append("")

    # Macro
    if deps:
        lines.append(f"#define {upper}_DEPS_TABLE(CONTAINER, FIELD) \\")
        for idx, d in enumerate(deps):
            version = (f'"{d["version_req"]}"'
                       if d["version_req"] else "(const char *)0")
            required = "true" if d["required"] else "false"
            stateful = "true" if d["stateful"] else "false"
            entry = (
                f"    {{ .name = \"{d['name']}\","
                f" .offset = offsetof(CONTAINER, FIELD.{d['name']}),"
                f" .required = {required}, .version_req = {version},"
                f" .mode = {d['mode']}, .stateful = {stateful},"
                f" .policy = {d['policy']} }}")
            if idx + 1 < len(deps):
                entry += ", \\"
            lines.append(entry)
        lines.append("")
    else:
        # No deps means no DEPS_TABLE macro — component uses
        # NX_COMPONENT_REGISTER_NO_DEPS instead.
        lines.append(
            f"/* {name} has no deps — use NX_COMPONENT_REGISTER_NO_DEPS. */")
        lines.append("")

    lines.append(f"#endif /* {guard} */")
    lines.append("")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Generate <name>_deps.h from a component manifest.json")
    parser.add_argument("manifest", type=pathlib.Path,
                        help="path to manifest.json")
    parser.add_argument("outdir", type=pathlib.Path,
                        help="directory to write <name>_deps.h into "
                             "(created if missing)")
    args = parser.parse_args(argv)

    try:
        manifest = load_manifest(args.manifest)
    except ManifestError as e:
        print(f"gen-config.py: {e}", file=sys.stderr)
        return 2

    args.outdir.mkdir(parents=True, exist_ok=True)
    out_path = args.outdir / f"{manifest['name']}_deps.h"
    out_path.write_text(render_header(manifest, args.manifest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
