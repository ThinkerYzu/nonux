#!/usr/bin/env python3
"""
verify-registry.py — Layer-1 machine checker for the registry rules
described in DESIGN.md §AI Verification (R1–R8).

Two enforcement layers cover the eight rules:

  Layer 1 (this tool)     — regex-decidable subset, run by `make
                            verify-registry` before `make` and
                            `make test`. Violations fail the build.
  Layer 2 (AI rubric)     — rules that need reasoning beyond regex
                            (slot-pointer dataflow, interface-schema
                            awareness, held-refs tracking, borrowed-cap
                            lifetime, generator identity, ISR/kthread
                            call-graph). Authoring and reviewing agents
                            consult the rubric documented in the
                            project's docs and report pass/fail per
                            rule.

"ai-verified" in this tool's output means "Layer-2 applies — the
agent is responsible for the check" — it is NOT a synonym for
"deferred" or "skipped."

Machine checks implemented here:

  R2 — Every `struct nx_slot *` field inside a component's state
       struct corresponds to a manifest `requires` / `optional` entry.
       Regex scan of `components/<name>/*.c` for declarations of the
       form `struct nx_slot *<ident>;`; the set of idents must equal
       the union of `requires.keys()` and `optional.keys()` in that
       component's manifest.json. Both drift directions are flagged
       (field present without manifest entry, manifest entry without
       field).

  R4 — Every `nx_slot_ref_retain(` call in a component's C source
       has a matching `nx_slot_ref_release(` call. Regex count match.
       Doesn't prove the release is reachable from disable/destroy
       (Layer 2 covers reachability), but catches the common "forgot
       to release at all" case.

CLI:

  verify-registry.py [COMPONENTS_DIR]     default: components/
  verify-registry.py --list               list all rules and status
  verify-registry.py --rule R2 [DIR]      run one rule only

Exit codes:
  0 — no findings (including when components/ is empty)
  2 — one or more findings, OR missing/invalid manifest

Output: one finding per line, formatted as
    <path>:<line> R<n>: <message>
or just
    <path> R<n>: <message>
when line info isn't applicable. Trailing summary line reports totals,
which rules ran as machine checks, and which are handed to Layer 2.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from dataclasses import dataclass


# ---------------------------------------------------------------------------
# Rule registry (the source of truth for --list)
# ---------------------------------------------------------------------------

@dataclass
class Rule:
    tag: str
    description: str
    status: str         # "machine" (this tool enforces it) or
                        # "ai-verified" (Layer-2 rubric; agents enforce)
    ai_reason: str = "" # why the machine layer can't check it today


RULES: list[Rule] = [
    Rule("R1", "No fabricated slot pointers",      "ai-verified",
         "slot-pointer dataflow is outside regex scope "
         "(needs clang/pycparser)"),
    Rule("R2", "Slot fields match manifest",       "machine"),
    Rule("R3", "Cap-only slot transfer",           "ai-verified",
         "interface message schemas aren't defined until slice 3.8+; "
         "until then the rule is rubric-enforced"),
    Rule("R4", "Retain/release pairing",           "machine"),
    Rule("R5", "Sender owns what it passes",       "ai-verified",
         "held-refs tracking across send sites needs symbolic state"),
    Rule("R6", "Handler doesn't stash borrowed caps", "ai-verified",
         "borrowed-cap dataflow to assignment sinks needs real AST"),
    Rule("R7", "Manifest is source of truth",      "ai-verified",
         "gen/ is gitignored so regenerate-and-diff has no on-disk "
         "target; determinism is covered by "
         "tools/tests/test_gen_config.py::*determinism*"),
    Rule("R8", "Slot-resolve locality",            "ai-verified",
         "ISR/kthread call-graph needs an entry-point tagging "
         "convention (slice 3.8+/3.9); runtime harness also asserts "
         "dispatcher-context at each slot-resolve"),
]


# ---------------------------------------------------------------------------
# Findings
# ---------------------------------------------------------------------------

@dataclass
class Finding:
    rule: str
    path: pathlib.Path
    line: int | None
    message: str

    def format(self, cwd: pathlib.Path) -> str:
        try:
            rel = self.path.relative_to(cwd)
        except ValueError:
            rel = self.path
        prefix = f"{rel}:{self.line}" if self.line else str(rel)
        return f"{prefix} {self.rule}: {self.message}"


# ---------------------------------------------------------------------------
# Loading
# ---------------------------------------------------------------------------

def load_manifest(path: pathlib.Path) -> dict | None:
    try:
        return json.loads(path.read_text())
    except FileNotFoundError:
        return None
    except json.JSONDecodeError:
        return None


def iter_component_dirs(components_dir: pathlib.Path):
    """Yield (dir, manifest) for every components/<name>/ with a
    manifest.json, in sorted order."""
    if not components_dir.exists():
        return
    for child in sorted(components_dir.iterdir()):
        if not child.is_dir():
            continue
        manifest = child / "manifest.json"
        if not manifest.exists():
            continue
        yield child, manifest


# ---------------------------------------------------------------------------
# R2 — Slot fields match manifest
# ---------------------------------------------------------------------------

# Matches `struct nx_slot *NAME;` in typical struct-field style. We require
# at least one leading whitespace character to avoid matching function-
# local declarations that start at column 0 (file-scope only has struct
# field decls at indent>0 in normal component code).
SLOT_FIELD_RE = re.compile(
    r"^\s+struct\s+nx_slot\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)\s*;",
    re.MULTILINE,
)


def scan_slot_fields(src: str) -> list[tuple[str, int]]:
    """Return [(field_name, 1-based line), ...] sorted by line."""
    out: list[tuple[str, int]] = []
    for m in SLOT_FIELD_RE.finditer(src):
        line = src.count("\n", 0, m.start()) + 1
        out.append((m.group(1), line))
    return out


def check_r2(comp_dir: pathlib.Path, manifest: dict) -> list[Finding]:
    findings: list[Finding] = []
    declared = set(manifest.get("requires", {}).keys()) \
             | set(manifest.get("optional", {}).keys())

    seen_fields: dict[str, tuple[pathlib.Path, int]] = {}
    for c_file in sorted(comp_dir.glob("*.c")):
        src = c_file.read_text()
        for name, line in scan_slot_fields(src):
            # A field might legitimately appear in more than one .c file
            # (rare but possible); remember the first site for the
            # missing-manifest-entry check below.
            seen_fields.setdefault(name, (c_file, line))

    # Fields present in code but not declared in manifest → R2 violation.
    for name, (path, line) in sorted(seen_fields.items()):
        if name not in declared:
            findings.append(Finding(
                "R2", path, line,
                f"struct nx_slot *{name} has no matching "
                f"requires/optional entry in manifest.json"))

    # Manifest declares a dep that has no corresponding struct field.
    manifest_path = comp_dir / "manifest.json"
    for name in sorted(declared - seen_fields.keys()):
        findings.append(Finding(
            "R2", manifest_path, None,
            f"dependency {name!r} declared in manifest but no matching "
            f"struct nx_slot *{name}; field found in {comp_dir.name}/*.c"))

    return findings


# ---------------------------------------------------------------------------
# R4 — Retain/release pairing
# ---------------------------------------------------------------------------

RETAIN_RE  = re.compile(r"\bnx_slot_ref_retain\s*\(")
RELEASE_RE = re.compile(r"\bnx_slot_ref_release\s*\(")


def check_r4(comp_dir: pathlib.Path, manifest: dict) -> list[Finding]:
    (void := manifest)  # manifest reserved for future per-dep checks
    retains:  list[tuple[pathlib.Path, int]] = []
    releases: list[tuple[pathlib.Path, int]] = []
    for c_file in sorted(comp_dir.glob("*.c")):
        src = c_file.read_text()
        for m in RETAIN_RE.finditer(src):
            retains.append((c_file, src.count("\n", 0, m.start()) + 1))
        for m in RELEASE_RE.finditer(src):
            releases.append((c_file, src.count("\n", 0, m.start()) + 1))

    if len(retains) == len(releases):
        return []

    # Emit one finding per unpaired retain (pointing at the call site).
    # If there are more releases than retains, flag the excess releases.
    findings: list[Finding] = []
    if len(retains) > len(releases):
        excess = len(retains) - len(releases)
        for path, line in retains[-excess:]:
            findings.append(Finding(
                "R4", path, line,
                f"nx_slot_ref_retain with no matching nx_slot_ref_release "
                f"(component has {len(retains)} retains and "
                f"{len(releases)} releases)"))
    else:
        excess = len(releases) - len(retains)
        for path, line in releases[-excess:]:
            findings.append(Finding(
                "R4", path, line,
                f"nx_slot_ref_release with no matching nx_slot_ref_retain "
                f"(component has {len(retains)} retains and "
                f"{len(releases)} releases)"))
    return findings


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

CHECK_FNS = {
    "R2": check_r2,
    "R4": check_r4,
}


def run_checks(components_dir: pathlib.Path,
               rules: list[str]) -> tuple[list[Finding], list[Rule]]:
    findings: list[Finding] = []
    ai_verified: list[Rule] = []
    machine_tags: set[str] = set()

    for r in RULES:
        if r.tag not in rules:
            continue
        if r.status == "ai-verified":
            ai_verified.append(r)
        else:
            machine_tags.add(r.tag)

    for comp_dir, manifest_path in iter_component_dirs(components_dir):
        manifest = load_manifest(manifest_path)
        if manifest is None:
            findings.append(Finding(
                "meta", manifest_path, None,
                "invalid or unreadable manifest.json"))
            continue
        for tag in sorted(machine_tags):
            findings.extend(CHECK_FNS[tag](comp_dir, manifest))

    return findings, ai_verified


def cmd_list() -> int:
    for r in RULES:
        if r.status == "machine":
            marker, tag = "✓", "machine"
        else:
            marker, tag = "○", "ai-verified"
        line = f"  {marker} {r.tag} — {r.description}  [{tag}"
        if r.status == "ai-verified":
            line += f": {r.ai_reason}"
        line += "]"
        print(line)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Layer-1 machine checker for DESIGN.md §AI Verification "
                    "rules. Rules marked ai-verified are enforced by Layer 2 "
                    "(authoring/reviewing agents), not deferred.")
    parser.add_argument("components_dir", nargs="?",
                        type=pathlib.Path, default=pathlib.Path("components"))
    parser.add_argument("--rule", action="append", default=None,
                        metavar="R<n>",
                        help="Only run the named rule (repeatable).")
    parser.add_argument("--list", action="store_true",
                        help="List all rules and their status; exit 0.")
    args = parser.parse_args(argv)

    if args.list:
        return cmd_list()

    rules = args.rule or [r.tag for r in RULES]
    invalid = [r for r in rules if r not in {x.tag for x in RULES}]
    if invalid:
        print(f"verify-registry.py: unknown rule(s): {invalid}",
              file=sys.stderr)
        return 2

    findings, ai_verified = run_checks(args.components_dir, rules)

    cwd = pathlib.Path.cwd()
    for f in findings:
        print(f.format(cwd), file=sys.stderr)

    # Summary line on stdout so CI can still grep it.
    machine_ran = [r.tag for r in RULES
                   if r.tag in rules and r.status == "machine"]
    summary = (f"verify-registry: {len(findings)} finding(s); "
               f"ran {','.join(machine_ran) or 'none'}; "
               f"ai-verified {','.join(r.tag for r in ai_verified) or 'none'}")
    print(summary)

    return 2 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
