#!/usr/bin/env python3
"""
gen-config.py — code generator for nonux build artefacts.

Two subcommands:

  manifest <manifest.json> <outdir>
      Emit <outdir>/<name>_deps.h containing:
        - struct <name>_deps     (one `struct nx_slot *` per manifest entry)
        - <NAME>_DEPS_TABLE      (macro expanding to nx_dep_descriptor
                                  initialisers using offsetof() into the
                                  caller-supplied container type)
      Used by every component's build and by the host test fixtures.

  kernel <kernel.json> <components_dir> <outdir>
      Emit three files derived from the top-level kernel config:
        <outdir>/config.h      — #define NX_TARGET, per-slot NX_SLOT_*_IMPL,
                                 per-component NX_CONFIG_* values from each
                                 binding's "config" block
        <outdir>/sources.mk    — COMPONENT_SRCS list of
                                 components/<impl>/<impl>.c paths, one per
                                 kernel.json binding
        <outdir>/slot_table.c  — one static `struct nx_slot` per binding
                                 plus the `nx_boot_slots[]` binding table
                                 that framework/bootstrap.c walks (slice
                                 3.9a)
      Consumed by the top-level Makefile via `-include gen/sources.mk`
      and by core/framework code via `#include "gen/config.h"`.

Both modes are stdlib-only (no jsonschema). Schema + cross-file
validation is tools/validate-config.py's job.

Output is deterministic: dict keys are always sorted before emission so
the regenerate-and-diff gate planned for slice 3.7 produces byte-
identical output across runs.
"""

import argparse
import json
import pathlib
import re
import sys

# ---------------------------------------------------------------------------
# Shared helpers
# ---------------------------------------------------------------------------

MODE_MAP = {
    "async": "NX_CONN_ASYNC",
    "sync":  "NX_CONN_SYNC",
}
POLICY_MAP = {
    "queue":    "NX_PAUSE_QUEUE",
    "reject":   "NX_PAUSE_REJECT",
    "redirect": "NX_PAUSE_REDIRECT",
}

C_IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


class ManifestError(Exception):
    """Structural / type error in a manifest.json or kernel.json."""


def c_ident(name: str) -> str:
    if not C_IDENT_RE.match(name):
        raise ManifestError(f"{name!r} is not a valid C identifier")
    return name


def slot_name_to_macro(slot: str) -> str:
    """
    kernel.json slot names may contain dots as namespacing
    (e.g. "char_device.serial"). Collapse to upper-case underscore form
    for use in #define macro names.
    """
    if not re.match(r"^[A-Za-z_][A-Za-z0-9_.]*$", slot):
        raise ManifestError(f"slot name {slot!r} contains invalid characters")
    return slot.replace(".", "_").upper()


# ---------------------------------------------------------------------------
# Per-component manifest → deps.h  (unchanged from slice 3.4)
# ---------------------------------------------------------------------------

def parse_dep(dep_name: str, spec: dict, required: bool) -> dict:
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

    return {
        "name":    raw["name"],
        "version": raw.get("version"),
        "deps":    deps,
    }


def render_deps_header(manifest: dict, manifest_path: pathlib.Path) -> str:
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

    lines.append(f"struct {name}_deps {{")
    if deps:
        for d in deps:
            tag = "required" if d["required"] else "optional"
            ver = f' — "{d["version_req"]}"' if d["version_req"] else ""
            lines.append(
                f"    struct nx_slot *{d['name']};"
                f"   /* {tag}{ver} */")
    else:
        lines.append("    char _nx_no_deps;  /* placeholder — manifest has no deps */")
    lines.append("};")
    lines.append("")

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
        lines.append(
            f"/* {name} has no deps — use NX_COMPONENT_REGISTER_NO_DEPS. */")
        lines.append("")

    lines.append(f"#endif /* {guard} */")
    lines.append("")
    return "\n".join(lines)


def cmd_manifest(args: argparse.Namespace) -> int:
    try:
        manifest = load_manifest(args.manifest)
    except ManifestError as e:
        print(f"gen-config.py: {e}", file=sys.stderr)
        return 2
    args.outdir.mkdir(parents=True, exist_ok=True)
    out = args.outdir / f"{manifest['name']}_deps.h"
    out.write_text(render_deps_header(manifest, args.manifest))
    return 0


# ---------------------------------------------------------------------------
# Top-level kernel.json → config.h + sources.mk  (new in slice 3.5)
# ---------------------------------------------------------------------------

def load_kernel(path: pathlib.Path) -> dict:
    try:
        raw = json.loads(path.read_text())
    except json.JSONDecodeError as e:
        raise ManifestError(f"{path}: invalid JSON: {e}") from e
    if not isinstance(raw, dict):
        raise ManifestError(f"{path}: top-level must be an object")
    if "target" not in raw or not isinstance(raw["target"], str):
        raise ManifestError(f"{path}: missing or non-string 'target'")
    components = raw.get("components")
    if not isinstance(components, dict) or not components:
        raise ManifestError(
            f"{path}: 'components' must be a non-empty object")

    bindings: list[dict] = []
    for slot in sorted(components.keys()):
        entry = components[slot]
        if not isinstance(entry, dict):
            raise ManifestError(
                f"components.{slot!r}: expected object, got "
                f"{type(entry).__name__}")
        impl = entry.get("impl")
        if not isinstance(impl, str) or not impl:
            raise ManifestError(
                f"components.{slot!r}: missing or non-string 'impl'")
        c_ident(impl)
        slot_name_to_macro(slot)    # sanity-check slot name
        config = entry.get("config", {})
        if not isinstance(config, dict):
            raise ManifestError(
                f"components.{slot!r}.config: expected object")
        bindings.append({"slot": slot, "impl": impl, "config": config})

    return {"target": raw["target"], "bindings": bindings}


def c_literal(value) -> str:
    """Render a Python config value as a C token for #define."""
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, str):
        # Treat numeric-looking strings (hex/decimal) as numeric tokens so
        # kernel.json values like "0x09000000" land as literals, not strings.
        if re.fullmatch(r"0[xX][0-9a-fA-F]+|[0-9]+", value):
            return value
        escaped = value.replace("\\", "\\\\").replace('"', '\\"')
        return f'"{escaped}"'
    raise ManifestError(
        f"unsupported config value type: {type(value).__name__}")


def render_config_h(kernel: dict, kernel_path: pathlib.Path) -> str:
    lines: list[str] = []
    lines.append(
        f"/* Auto-generated by tools/gen-config.py from "
        f"{kernel_path.name}. Do not edit. */")
    lines.append("#ifndef NX_GEN_CONFIG_H")
    lines.append("#define NX_GEN_CONFIG_H")
    lines.append("")
    lines.append(f'#define NX_TARGET "{kernel["target"]}"')
    lines.append("")
    lines.append("/* Slot bindings. */")
    for b in kernel["bindings"]:
        macro = slot_name_to_macro(b["slot"])
        lines.append(f'#define NX_SLOT_{macro}_IMPL "{b["impl"]}"')
    lines.append("")
    any_config = any(b["config"] for b in kernel["bindings"])
    if any_config:
        lines.append("/* Per-component config values. */")
    for b in kernel["bindings"]:
        if not b["config"]:
            continue
        comp_macro = b["impl"].upper()
        for k in sorted(b["config"].keys()):
            v = b["config"][k]
            key_macro = k.upper()
            lines.append(
                f"#define NX_CONFIG_{comp_macro}_{key_macro} {c_literal(v)}")
    lines.append("")
    lines.append("#endif /* NX_GEN_CONFIG_H */")
    lines.append("")
    return "\n".join(lines)


def render_sources_mk(kernel: dict, kernel_path: pathlib.Path) -> str:
    """
    Emits COMPONENT_SRCS as a deterministic, deduplicated list. Multiple
    slots bound to the same impl produce one source entry.
    """
    srcs = sorted({b["impl"] for b in kernel["bindings"]})
    lines: list[str] = []
    lines.append(
        f"# Auto-generated by tools/gen-config.py from "
        f"{kernel_path.name}. Do not edit.")
    lines.append("")
    lines.append("COMPONENT_SRCS := \\")
    for idx, impl in enumerate(srcs):
        entry = f"    components/{impl}/{impl}.c"
        if idx + 1 < len(srcs):
            entry += " \\"
        lines.append(entry)
    lines.append("")
    return "\n".join(lines)


def slot_name_to_ident(slot: str) -> str:
    """Map a kernel.json slot name (may contain dots) to a legal C
    identifier suffix.  Dots become underscores; validated up front by
    slot_name_to_macro's C-identifier check. Used for static variable
    names in gen/slot_table.c."""
    return slot.replace(".", "_")


def slot_iface(slot: str) -> str:
    """Derive a slot's interface tag from its name.  For names like
    `char_device.serial` the iface is `char_device` (the part before
    the first dot); for plain names like `scheduler` the iface is the
    name itself.  kernel.schema.json doesn't yet carry an explicit
    iface field; slice 3.9b+ can add one once more varied compositions
    exist."""
    return slot.split(".", 1)[0]


def render_slot_table_c(kernel: dict, kernel_path: pathlib.Path) -> str:
    """
    Emits gen/slot_table.c — one static `struct nx_slot` per kernel.json
    slot plus a `nx_boot_slots[]` binding array that framework/
    bootstrap.c walks at boot.  Every binding is deterministic (sorted
    by slot name) so the output is reproducible byte-for-byte.
    """
    lines: list[str] = []
    lines.append(
        f"/* Auto-generated by tools/gen-config.py from "
        f"{kernel_path.name}. Do not edit. */")
    lines.append("")
    lines.append('#include "framework/bootstrap.h"')
    lines.append('#include "framework/registry.h"')
    lines.append("")
    lines.append("/* One static slot per kernel.json binding. */")
    for b in kernel["bindings"]:
        ident = slot_name_to_ident(b["slot"])
        iface = slot_iface(b["slot"])
        lines.append(f"static struct nx_slot nx_slot_{ident} = {{")
        lines.append(f'    .name        = "{b["slot"]}",')
        lines.append(f'    .iface       = "{iface}",')
        lines.append(f"    .mutability  = NX_MUT_HOT,")
        lines.append(f"    .concurrency = NX_CONC_SHARED,")
        lines.append("};")
    lines.append("")
    lines.append("/* Binding table consumed by nx_framework_bootstrap(). */")
    lines.append("struct nx_boot_slot nx_boot_slots[] = {")
    for b in kernel["bindings"]:
        ident = slot_name_to_ident(b["slot"])
        lines.append(
            f'    {{ .slot = &nx_slot_{ident}, '
            f'.impl_name = "{b["impl"]}" }},')
    lines.append("};")
    lines.append("")
    lines.append(
        f"const unsigned nx_boot_slots_count = "
        f"sizeof nx_boot_slots / sizeof nx_boot_slots[0];")
    lines.append("")
    return "\n".join(lines)


def cmd_kernel(args: argparse.Namespace) -> int:
    try:
        kernel = load_kernel(args.kernel)
    except ManifestError as e:
        print(f"gen-config.py: {e}", file=sys.stderr)
        return 2
    args.outdir.mkdir(parents=True, exist_ok=True)
    (args.outdir / "config.h").write_text(render_config_h(kernel, args.kernel))
    (args.outdir / "sources.mk").write_text(
        render_sources_mk(kernel, args.kernel))
    (args.outdir / "slot_table.c").write_text(
        render_slot_table_c(kernel, args.kernel))
    # components_dir is accepted for API parity with validate-config.py but
    # not used in 3.5 — kernel mode only reads kernel.json. Cross-manifest
    # resolution is validate-config.py's job.
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Generate nonux build artefacts "
                    "(per-component deps.h or top-level config.h/sources.mk).")
    sub = parser.add_subparsers(dest="cmd", required=True)

    pm = sub.add_parser(
        "manifest",
        help="Emit <outdir>/<name>_deps.h from a per-component manifest.json")
    pm.add_argument("manifest", type=pathlib.Path)
    pm.add_argument("outdir",   type=pathlib.Path)
    pm.set_defaults(fn=cmd_manifest)

    pk = sub.add_parser(
        "kernel",
        help="Emit <outdir>/config.h and <outdir>/sources.mk from kernel.json")
    pk.add_argument("kernel",         type=pathlib.Path)
    pk.add_argument("components_dir", type=pathlib.Path,
                    help="Root of the components/ tree "
                         "(reserved for cross-manifest checks in 3.5+)")
    pk.add_argument("outdir",         type=pathlib.Path)
    pk.set_defaults(fn=cmd_kernel)

    args = parser.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
