#!/usr/bin/env python3
"""
validate-config.py — whole-tree validation for a nonux configuration.

Checks:

  1. Schema:     kernel.json and every referenced component's
                 manifest.json validate against tools/schemas/*.json
                 (via the `jsonschema` library).
  2. Existence:  every component impl named in kernel.json has a
                 components/<impl>/manifest.json file on disk, and the
                 manifest's `name` field matches the directory.
  3. Versions:   every `requires.<dep>.version` / `optional.<dep>.version`
                 constraint is satisfied by the target component's top-
                 level `version`. Only `>=X.Y.Z` is supported in v1.
  4. Cycles:     the directed dep graph (component → its required deps)
                 is acyclic. Optional deps are included in the cycle
                 check because a cycle through optionals is still a
                 boot-order impossibility.

Exit code 0 on success, 2 on any validation failure. Errors are printed
to stderr, one per line, grouped by check.

Also provides two report modes for the existing `make deps` /
`make deps-dot` targets:

  --deps      one-line-per-edge list of every resolved dep edge
  --deps-dot  graphviz `digraph { ... }` dump

Both modes still perform all validation checks; they only add a report.
Exit code reflects validation, not report success.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from typing import Iterable

try:
    import jsonschema
except ImportError:
    print("validate-config.py: jsonschema not installed. "
          "Activate the venv: source .venv/bin/activate && "
          "pip install -r tools/requirements.txt", file=sys.stderr)
    sys.exit(2)


SCHEMA_DIR = pathlib.Path(__file__).resolve().parent / "schemas"


class ValidationError(Exception):
    """A validation failure that should turn into exit code 2."""


# ---------------------------------------------------------------------------
# Schema validation
# ---------------------------------------------------------------------------

def load_schema(name: str) -> dict:
    path = SCHEMA_DIR / f"{name}.schema.json"
    return json.loads(path.read_text())


def validate_against(schema_name: str, instance: dict,
                     source: pathlib.Path) -> list[str]:
    schema = load_schema(schema_name)
    v = jsonschema.Draft202012Validator(schema)
    errors: list[str] = []
    for err in sorted(v.iter_errors(instance), key=lambda e: list(e.path)):
        path = "/".join(str(p) for p in err.path) or "<root>"
        errors.append(f"{source}: {schema_name} schema: {path}: {err.message}")
    return errors


# ---------------------------------------------------------------------------
# Version matching (>=X.Y.Z only, v1)
# ---------------------------------------------------------------------------

SEMVER_RE    = re.compile(r"^([0-9]+)\.([0-9]+)\.([0-9]+)$")
CONSTRAINT_RE = re.compile(r"^>=\s*([0-9]+)\.([0-9]+)\.([0-9]+)$")


def parse_semver(s: str) -> tuple[int, int, int]:
    m = SEMVER_RE.match(s)
    if not m:
        raise ValidationError(f"version {s!r} is not X.Y.Z")
    return int(m.group(1)), int(m.group(2)), int(m.group(3))


def satisfies(constraint: str, version: str) -> bool:
    m = CONSTRAINT_RE.match(constraint)
    if not m:
        raise ValidationError(
            f"version constraint {constraint!r} must be '>=X.Y.Z'")
    required = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
    return parse_semver(version) >= required


# ---------------------------------------------------------------------------
# Loading
# ---------------------------------------------------------------------------

def read_json(path: pathlib.Path) -> dict:
    try:
        return json.loads(path.read_text())
    except FileNotFoundError as e:
        raise ValidationError(f"{path}: not found") from e
    except json.JSONDecodeError as e:
        raise ValidationError(f"{path}: invalid JSON: {e}") from e


def load_component_manifests(components_dir: pathlib.Path,
                             impls: Iterable[str],
                             errors: list[str]) -> dict[str, dict]:
    """
    For every `impl` referenced in kernel.json, load
    components/<impl>/manifest.json. Missing files and name mismatches
    append to `errors` and are returned as absent from the result dict.
    """
    manifests: dict[str, dict] = {}
    for impl in sorted(set(impls)):
        path = components_dir / impl / "manifest.json"
        if not path.exists():
            errors.append(
                f"components/{impl}/manifest.json: not found "
                f"(referenced by kernel.json)")
            continue
        try:
            raw = json.loads(path.read_text())
        except json.JSONDecodeError as e:
            errors.append(f"{path}: invalid JSON: {e}")
            continue
        if raw.get("name") != impl:
            errors.append(
                f"{path}: manifest.name={raw.get('name')!r} does not match "
                f"directory name {impl!r}")
            # Still record it so later version/dep checks have something
            # to work with — the caller will still bail on non-empty
            # errors.
        manifests[impl] = raw
    return manifests


# ---------------------------------------------------------------------------
# Cross-manifest checks
# ---------------------------------------------------------------------------

def check_pause_hook_required(manifests: dict[str, dict],
                              errors: list[str]) -> None:
    """
    Slice 3.8: any component declaring `spawns_threads: true` must also
    declare `pause_hook: true`. The framework's pause protocol relies on
    the hook to quiesce per-component threads within the 1 ms deadline
    — a spawning component without a hook is a silent hang waiting to
    happen, so catch it at build time.
    """
    for impl, m in sorted(manifests.items()):
        if m.get("spawns_threads") and not m.get("pause_hook"):
            errors.append(
                f"components/{impl}/manifest.json: spawns_threads=true "
                f"requires pause_hook=true (pause protocol needs a hook "
                f"to quiesce component-spawned threads)")


def check_versions(kernel: dict, manifests: dict[str, dict],
                   errors: list[str]) -> None:
    """
    For every requires/optional dep declared by a selected component,
    check that the dep name corresponds to another kernel.json slot
    whose bound impl carries a version satisfying the constraint.
    """
    # Map slot_name → bound impl name
    slot_to_impl = {slot: entry["impl"]
                    for slot, entry in kernel["components"].items()}

    for slot, entry in sorted(kernel["components"].items()):
        impl = entry["impl"]
        m = manifests.get(impl)
        if not m:
            continue  # already reported missing
        for kind in ("requires", "optional"):
            for dep_name, dep_spec in sorted(m.get(kind, {}).items()):
                target_impl = slot_to_impl.get(dep_name)
                if not target_impl:
                    # Dep names a slot that isn't in kernel.json. For
                    # `requires`, that's fatal at boot; for `optional`
                    # it's fine (slot pointer stays NULL).
                    if kind == "requires":
                        errors.append(
                            f"components/{impl}/manifest.json: requires "
                            f"{dep_name!r}, but kernel.json has no slot of "
                            f"that name")
                    continue
                version_req = dep_spec.get("version")
                if not version_req:
                    continue  # no constraint → nothing to check
                target = manifests.get(target_impl)
                if not target:
                    continue  # already reported missing
                target_version = target.get("version")
                if not target_version:
                    errors.append(
                        f"components/{target_impl}/manifest.json: missing "
                        f"top-level 'version' (needed by {impl})")
                    continue
                try:
                    ok = satisfies(version_req, target_version)
                except ValidationError as e:
                    errors.append(
                        f"components/{impl}/manifest.json: {kind}.{dep_name}"
                        f".version: {e}")
                    continue
                if not ok:
                    errors.append(
                        f"components/{impl}/manifest.json: {kind}.{dep_name}"
                        f".version={version_req!r} not satisfied by "
                        f"{target_impl} version {target_version!r}")


def build_dep_graph(kernel: dict,
                    manifests: dict[str, dict]) -> dict[str, list[str]]:
    """
    Return {slot_name: [dep_slot_name, ...]} covering both requires and
    optional edges. Uses slot names (kernel.json keys), not impl names,
    because the dep graph is about the running composition.
    """
    graph: dict[str, list[str]] = {}
    slot_names = set(kernel["components"].keys())
    for slot, entry in kernel["components"].items():
        impl = entry["impl"]
        m = manifests.get(impl, {})
        deps: list[str] = []
        for kind in ("requires", "optional"):
            for dep_name in m.get(kind, {}).keys():
                if dep_name in slot_names:
                    deps.append(dep_name)
        graph[slot] = sorted(set(deps))
    return graph


def find_cycle(graph: dict[str, list[str]]) -> list[str] | None:
    """
    Returns the first cycle found as a list [A, B, C, A] or None if
    the graph is acyclic. Uses iterative DFS with an on-stack set so
    deeply nested graphs don't hit Python's recursion limit.
    """
    WHITE, GRAY, BLACK = 0, 1, 2
    colour = {n: WHITE for n in graph}
    parent: dict[str, str | None] = {n: None for n in graph}

    for root in sorted(graph):
        if colour[root] != WHITE:
            continue
        stack: list[tuple[str, int]] = [(root, 0)]
        parent[root] = None
        colour[root] = GRAY
        while stack:
            node, idx = stack[-1]
            neighbours = graph[node]
            if idx < len(neighbours):
                stack[-1] = (node, idx + 1)
                nxt = neighbours[idx]
                if colour.get(nxt, BLACK) == GRAY:
                    # Back-edge: reconstruct cycle A → ... → node → nxt → ... → nxt.
                    # Self-loop (node == nxt) is the minimal case — no
                    # intermediates, so emit [nxt, nxt] directly instead
                    # of the general-case path that would repeat nxt.
                    if nxt == node:
                        return [nxt, nxt]
                    cycle = [nxt, node]
                    p = parent[node]
                    while p is not None and p != nxt:
                        cycle.append(p)
                        p = parent[p]
                    cycle.append(nxt)
                    cycle.reverse()
                    return cycle
                if colour.get(nxt, BLACK) == WHITE:
                    colour[nxt] = GRAY
                    parent[nxt] = node
                    stack.append((nxt, 0))
            else:
                colour[node] = BLACK
                stack.pop()
    return None


def check_cycles(graph: dict[str, list[str]], errors: list[str]) -> None:
    cycle = find_cycle(graph)
    if cycle:
        errors.append(
            "dependency cycle detected: " + " → ".join(cycle))


# ---------------------------------------------------------------------------
# Reports
# ---------------------------------------------------------------------------

def report_deps(graph: dict[str, list[str]]) -> None:
    for src in sorted(graph):
        for dst in graph[src]:
            print(f"{src} -> {dst}")


def report_deps_dot(graph: dict[str, list[str]]) -> None:
    print("digraph nonux_deps {")
    print('    rankdir=LR;')
    print('    node [shape=box, style=rounded];')
    for src in sorted(graph):
        for dst in graph[src]:
            print(f'    "{src}" -> "{dst}";')
    print("}")


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Validate a nonux kernel.json + component manifests.")
    parser.add_argument("kernel",         type=pathlib.Path)
    parser.add_argument("components_dir", type=pathlib.Path)
    parser.add_argument("--deps",     action="store_true",
                        help="Print resolved dep edges (one per line).")
    parser.add_argument("--deps-dot", action="store_true",
                        help="Print the dep graph as graphviz dot.")
    args = parser.parse_args(argv)

    errors: list[str] = []

    # Load kernel.json + schema-check it.
    try:
        kernel = read_json(args.kernel)
    except ValidationError as e:
        print(f"validate-config.py: {e}", file=sys.stderr)
        return 2
    errors.extend(validate_against("kernel", kernel, args.kernel))
    if errors:
        # Kernel structure is broken — further checks would cascade.
        for e in errors:
            print(e, file=sys.stderr)
        return 2

    # Cross-manifest work.
    impls = [entry["impl"] for entry in kernel["components"].values()]
    manifests = load_component_manifests(args.components_dir, impls, errors)

    # Schema-check every loaded manifest.
    for impl, manifest in sorted(manifests.items()):
        path = args.components_dir / impl / "manifest.json"
        errors.extend(validate_against("manifest", manifest, path))

    check_pause_hook_required(manifests, errors)
    check_versions(kernel, manifests, errors)
    graph = build_dep_graph(kernel, manifests)
    check_cycles(graph, errors)

    if errors:
        for e in errors:
            print(e, file=sys.stderr)
        # Still print reports if requested so users can inspect the
        # partially-valid state, but keep the failing exit code.
        if args.deps:     report_deps(graph)
        if args.deps_dot: report_deps_dot(graph)
        return 2

    if args.deps:     report_deps(graph)
    if args.deps_dot: report_deps_dot(graph)
    return 0


if __name__ == "__main__":
    sys.exit(main())
