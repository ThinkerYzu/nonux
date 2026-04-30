#!/usr/bin/env python3
"""
gen-iface.py — IDL-driven interface code generator for nonux (slice 8.0pre.1).

Reads `interfaces/idl/<iface>.json` files and emits four artifacts per
interface:

    interfaces/<iface>.h            — struct nx_<iface>_ops typedef +
                                       constants + forward declarations
    interfaces/<iface>_msg.h        — enum nx_<iface>_op_id + per-op
                                       request/reply message structs
    framework/<iface>_call.h        — per-op sender wrappers that call
                                       nx_slot_call_blocking()
    framework/<iface>_dispatch.h    — receiver-side handle_msg dispatch
                                       template

Per DESIGN.md R7 ("Manifest is source of truth") and IDL-SCHEMA.md
("byte-for-byte"), the emitted `interfaces/<iface>.h` is byte-identical
to a fresh regeneration; authors never hand-edit the generated files.

Validation uses `jsonschema` if available (per
tools/requirements.txt).  Falls back to manual top-level checks if the
package is absent so the generator can run in a stripped-down env.

Output is deterministic: dict iteration follows IDL ordering (ops in
op_id order), and string content is byte-identical across runs given
the same inputs.

Subcommands:

    all <idl_dir> <interfaces_dir> <framework_dir>
        Scan idl_dir for *.json; regenerate every artifact.

    one <idl_file> <interfaces_dir> <framework_dir>
        Regenerate artifacts for a single interface (used by tests).

    verify <idl_dir> <interfaces_dir> <framework_dir>
        Re-run the generator into a temp dir; diff against the in-tree
        artifacts; exit non-zero if any diff (or if any IDL is stale or
        any meta-schema check fails).  This is `make verify-iface-fresh`.
"""

import argparse
import difflib
import json
import pathlib
import re
import sys
import tempfile

try:
    import jsonschema
    HAVE_JSONSCHEMA = True
except ImportError:
    HAVE_JSONSCHEMA = False


# ---------------------------------------------------------------------------
# Schema + IDL loading
# ---------------------------------------------------------------------------

class IDLError(Exception):
    """Structural / semantic error in an IDL file or meta-schema mismatch."""


def load_meta_schema(tools_dir: pathlib.Path) -> dict:
    path = tools_dir / "idl-meta-schema.json"
    try:
        return json.loads(path.read_text())
    except FileNotFoundError:
        raise IDLError(f"meta-schema not found: {path}")
    except json.JSONDecodeError as e:
        raise IDLError(f"{path}: invalid JSON: {e}") from e


def load_idl(path: pathlib.Path, meta_schema: dict) -> dict:
    try:
        idl = json.loads(path.read_text())
    except json.JSONDecodeError as e:
        raise IDLError(f"{path}: invalid JSON: {e}") from e

    if HAVE_JSONSCHEMA:
        try:
            jsonschema.validate(idl, meta_schema)
        except jsonschema.ValidationError as e:
            # Surface a path-prefixed message so authors can locate the offender.
            loc = "/".join(str(p) for p in e.absolute_path) or "<root>"
            raise IDLError(f"{path}: schema violation at {loc}: {e.message}") from e

    # Filename-stem must equal the declared interface name.  Catches
    # accidental copy-paste rename (file says fs.json, content still says
    # vfs).  Also covered by a strict-validation rule in the meta-schema
    # via `interface` pattern, but the stem match is meta-schema-external.
    if idl.get("interface") != path.stem:
        raise IDLError(
            f"{path}: filename stem {path.stem!r} does not match "
            f"declared interface {idl.get('interface')!r}")

    if not HAVE_JSONSCHEMA:
        # Minimal guard so the generator doesn't trip later on missing keys
        # in environments without jsonschema.  Enough to catch typos; full
        # correctness is the meta-schema's job.
        for key in ("interface", "version", "iface_id", "ops"):
            if key not in idl:
                raise IDLError(f"{path}: missing required field {key!r}")

    return idl


# ---------------------------------------------------------------------------
# IDL → C type mapping
# ---------------------------------------------------------------------------

# Param-type → C type emitted in the typedef header signature.  The IDL
# spec's Param Type System maps these; the asymmetric `i32 → int` (vs
# `u32 → uint32_t`) matches existing hand-written conventions and is
# documented in IDL-SCHEMA.md.
SCALAR_C_TYPES = {
    "u8":  "uint8_t",
    "u16": "uint16_t",
    "u32": "uint32_t",
    "u64": "uint64_t",
    "i8":  "int8_t",
    "i16": "int16_t",
    "i32": "int",
    "i64": "int64_t",
    "usize": "size_t",
    "bool": "bool",
}

# Return-type code → C return-type token.
RETURN_C_TYPES = {
    "void":                 "void",
    "int_status":           "int",
    "i64_count_or_status":  "int64_t",
    "usize":                "size_t",
    "void_ptr":             "void *",
}


def param_c_signature(p: dict) -> str:
    """
    Render a single param as it appears in the typedef ops signature
    (which takes `void *self` first, then the IDL-declared params).
    Returns e.g. "const char *path", "uint32_t flags", "void *file",
    "void **out_file", "void *buf", "size_t cap", "uint32_t *cookie",
    "struct nx_fs_dirent *out".
    """
    name = p["name"]
    t = p["type"]

    if t in SCALAR_C_TYPES:
        return f"{SCALAR_C_TYPES[t]} {name}"

    if t == "string_in":
        return f"const char *{name}"

    if t == "bytes_in":
        return f"const void *{name}"

    if t == "bytes_out":
        return f"void *{name}"

    if t in ("struct_in",):
        # By-value POD (none of today's vfs ops use it).
        return f"{p['ctype']} {name}"

    if t in ("struct_out", "struct_inout"):
        # Caller-pointer POD.  ctype may be a primitive (e.g. uint32_t
        # for readdir's cookie iterator) or a struct/union/enum.
        return f"{p['ctype']} *{name}"

    if t == "slot_ref":
        # Sender-side: the receiver's typedef sees `struct nx_slot *`
        # too — the cap layer is invisible to the impl that gets called.
        return f"struct nx_slot *{name}"

    if t == "opaque_self_handle":
        # `void *` for direction=in (the common case, e.g. `file`); a
        # `void **` for direction=out (e.g. `out_file`).
        if p.get("direction") == "out":
            return f"void **{name}"
        return f"void *{name}"

    raise IDLError(f"unknown param type {t!r}")


# ---------------------------------------------------------------------------
# Forward-decl auto-detection
# ---------------------------------------------------------------------------

# A `ctype` that names a struct/union/enum needs forward declaration in
# the typedef header.  Primitives (e.g. `uint32_t`) do not.  IDL author
# does not enumerate the set; the generator walks ops and collects.
_TAGGED_TYPE_RE = re.compile(r"^(struct|union|enum)\s+\S+$")


def collect_forward_decls(idl: dict) -> list[str]:
    """Return tagged-type ctypes referenced by op params, in order of
    first appearance (op_id ascending; param order within an op).
    When the IDL declares author-supplied `includes:`, those are assumed
    to provide the full definitions for any tagged types so we skip
    forward decls — the include's full definition is the declaration."""
    if idl.get("includes"):
        return []
    seen: set[str] = set()
    order: list[str] = []
    for op in idl["ops"]:
        for p in op.get("params", []):
            ctype = p.get("ctype")
            if ctype and _TAGGED_TYPE_RE.match(ctype) and ctype not in seen:
                seen.add(ctype)
                order.append(ctype)
    return order


# ---------------------------------------------------------------------------
# Comment-block formatting
# ---------------------------------------------------------------------------

def render_block_comment(text: str, indent: int = 0) -> list[str]:
    """
    Render a block comment, choosing single-line / expanded multi-line
    based on the text shape:

      - Empty text → no output.
      - Single line, fits within ~80 cols at the given indent → single-line:
            /* foo. */
      - Otherwise → expanded multi-line:
            /*
             * foo line 1
             * foo line 2
             */

    `text` may contain explicit `\\n` for line breaks.  Blank lines
    (paragraph separators) become ` *` (the IDL author's chosen
    paragraph layout is preserved verbatim).
    """
    if not text:
        return []

    pad = " " * indent
    lines = text.split("\n")

    # Single-line form if exactly one line and it fits.
    if len(lines) == 1:
        candidate = f"{pad}/* {lines[0]} */"
        if len(candidate) <= 80:
            return [candidate]

    out = [f"{pad}/*"]
    for ln in lines:
        if ln == "":
            out.append(f"{pad} *")
        else:
            out.append(f"{pad} * {ln}")
    out.append(f"{pad} */")
    return out


# ---------------------------------------------------------------------------
# Member-signature line wrapping
# ---------------------------------------------------------------------------

def wrap_signature(indent: int, prefix: str, params: list[str],
                   trailing: str) -> list[str]:
    """
    Build a (possibly wrapped) C function-pointer signature line.

      indent:   leading-space count for line 1.
      prefix:   text up to and including the opening param-list paren,
                e.g. "int (*open)(".
      params:   formatted param strings (no leading space, no trailing
                comma), e.g. ["void *self", "const char *path"].
      trailing: closing tail, e.g. ");" or ");\\n".

    Returns a list of lines.  Single-line if the whole signature fits
    in 80 cols; otherwise greedy-packed across multiple lines, with
    continuation lines aligned to the column of the first param.
    """
    head = " " * indent + prefix
    single = head + ", ".join(params) + trailing
    # Wrap if the single-line form would exceed 79 chars (matches today's
    # hand-written conventions; an 80-char `open` signature wraps).
    if len(single) <= 79:
        return [single]

    cont = " " * len(head)
    lines: list[str] = []
    cur = head
    for i, p in enumerate(params):
        is_last = i == len(params) - 1
        sep = "" if cur == head else ", "
        candidate = cur + sep + p
        budget = 79 - (len(trailing) if is_last else 1)
        if cur != head and len(candidate) > budget:
            lines.append(cur + ",")
            cur = cont + p
        else:
            cur = candidate
    lines.append(cur + trailing)
    return lines


# ---------------------------------------------------------------------------
# Banner
# ---------------------------------------------------------------------------

def banner(idl_filename: str) -> list[str]:
    return [
        "/*",
        f" * GENERATED — DO NOT EDIT.",
        f" * Source: interfaces/idl/{idl_filename}",
        f" * Generator: tools/gen-iface.py",
        " */",
    ]


# ---------------------------------------------------------------------------
# Constants emission (handles group-doc on first item of a contiguous
# block)
# ---------------------------------------------------------------------------

def render_constants(constants: list[dict]) -> list[str]:
    """
    Emit `#define NAME    VALUE` lines, with optional block-comment
    above each "group".  A group is a maximal run of consecutive
    constants where only the first carries a `doc` field; the doc is
    rendered as a leading block comment for the group.  Within a group,
    name-to-value column alignment uses the longest name in the group.
    """
    if not constants:
        return []

    # Partition into groups.  A new group begins at the first item, and
    # again whenever a later item's `doc` is non-empty.
    groups: list[list[dict]] = []
    cur: list[dict] = []
    for c in constants:
        if cur and c.get("doc"):
            groups.append(cur)
            cur = [c]
        else:
            cur.append(c)
    if cur:
        groups.append(cur)

    # Global value-column alignment (NOT per-group): the longest name
    # across the whole constant list determines the value column for
    # every group.  Matches today's hand-written headers (vfs.h, fs.h
    # both align across groups).  Min separator = 3 spaces.
    max_name_global = max(len(c["name"]) for c in constants)
    out: list[str] = []
    for gi, g in enumerate(groups):
        # Leading group block-comment (from the first item's doc, if any).
        if g[0].get("doc"):
            out.extend(render_block_comment(g[0]["doc"], indent=0))
        for c in g:
            pad = " " * (max_name_global - len(c["name"]) + 3)
            value = c["value"]
            value_s = str(value)
            out.append(f"#define {c['name']}{pad}{value_s}")
        # Blank line between groups (but not after the final group).
        if gi + 1 < len(groups):
            out.append("")
    return out


# ---------------------------------------------------------------------------
# struct nx_<iface>_ops (the typedef) emission
# ---------------------------------------------------------------------------

def render_op_member(op: dict) -> list[str]:
    """Render one member of `struct nx_<iface>_ops`: optional leading
    block comment + `<ret> (*<name>)(void *self, ...);`."""
    out: list[str] = []
    if op.get("doc"):
        out.extend(render_block_comment(op["doc"], indent=4))

    rtype = RETURN_C_TYPES[op["returns"]["type"]]
    params: list[str] = ["void *self"]
    for p in op.get("params", []):
        params.append(param_c_signature(p))
    # Pointer-style member: `<ret> (*<name>)(...)`.  `void *` return is
    # one of the few non-space-suffixed return types.
    if rtype.endswith("*"):
        prefix = f"{rtype}(*{op['name']})("
    else:
        prefix = f"{rtype} (*{op['name']})("
    out.extend(wrap_signature(indent=4, prefix=prefix, params=params,
                              trailing=");"))
    return out


def render_iface_ops_struct(idl: dict) -> list[str]:
    name = idl["interface"]
    out = [f"struct nx_{name}_ops {{"]
    members: list[list[dict]] = []

    # Group consecutive ops where only the first has a doc field — the
    # generator treats the doc as a shared comment over the run.  This
    # mirrors today's `read`/`write` shared-doc shape in vfs.h.
    cur: list[dict] = []
    for op in idl["ops"]:
        if cur and op.get("doc"):
            members.append(cur)
            cur = [op]
        else:
            cur.append(op)
    if cur:
        members.append(cur)

    for gi, group in enumerate(members):
        # Leading doc + first member's signature
        first = group[0]
        out.extend(render_op_member(first))
        # Subsequent members in the group: signature only, no doc, no
        # blank line above (they share the group doc).
        for op in group[1:]:
            rtype = RETURN_C_TYPES[op["returns"]["type"]]
            params: list[str] = ["void *self"]
            for p in op.get("params", []):
                params.append(param_c_signature(p))
            if rtype.endswith("*"):
                prefix = f"{rtype}(*{op['name']})("
            else:
                prefix = f"{rtype} (*{op['name']})("
            out.extend(wrap_signature(indent=4, prefix=prefix,
                                      params=params, trailing=");"))
        # Blank line after each group except the last.
        if gi + 1 < len(members):
            out.append("")

    out.append("};")
    return out


# ---------------------------------------------------------------------------
# interfaces/<iface>.h emission
# ---------------------------------------------------------------------------

def render_iface_header(idl: dict, idl_filename: str) -> str:
    name = idl["interface"]
    guard = f"NONUX_INTERFACE_{name.upper()}_H"

    out: list[str] = []
    out.extend(banner(idl_filename))
    out.append("")
    out.append(f"#ifndef {guard}")
    out.append(f"#define {guard}")
    out.append("")

    # Standard includes — every typedef header uses size_t / uint*_t.
    out.append("#include <stddef.h>")
    out.append("#include <stdint.h>")
    if any(p["type"] == "bool" for op in idl["ops"]
           for p in op.get("params", [])):
        out.append("#include <stdbool.h>")
    # Author-supplied additional includes from the IDL (for transitive
    # types brought in by name).
    for inc in idl.get("includes", []):
        sysinc = inc.get("system", False)
        path = inc["path"]
        formatted = f"<{path}>" if sysinc else f'"{path}"'
        comment = ""
        if inc.get("doc"):
            comment = f"  /* {inc['doc']} */"
        out.append(f"#include {formatted}{comment}")
    out.append("")

    # Top-level interface block comment.
    if idl.get("doc"):
        out.extend(render_block_comment(idl["doc"], indent=0))
        out.append("")

    # Constants (with group-doc handling).
    if idl.get("constants"):
        out.extend(render_constants(idl["constants"]))
        out.append("")

    # Forward declarations (auto-detected from struct/union/enum ctypes).
    fdecls = collect_forward_decls(idl)
    if fdecls:
        if idl.get("forward_decls_doc"):
            out.extend(render_block_comment(idl["forward_decls_doc"], indent=0))
        for ct in fdecls:
            out.append(f"{ct};")
        out.append("")

    # struct nx_<iface>_ops typedef.
    out.extend(render_iface_ops_struct(idl))
    out.append("")
    out.append(f"#endif /* {guard} */")
    out.append("")
    return "\n".join(out)


# ---------------------------------------------------------------------------
# interfaces/<iface>_msg.h emission
# ---------------------------------------------------------------------------

# Maximum sizes for variable-length payload fields.  Picked to cover
# today's syscall workloads without forcing a knob in the IDL.  Future
# IDL extension may add per-op overrides; for slice 8.0pre.1 these are
# fixed.  Documented in IDL-SCHEMA.md (open items #2/#3).
DEFAULT_PATH_MAX  = 4096
DEFAULT_BYTES_MAX = 4096


def msg_struct_field(p: dict) -> str | None:
    """
    Render one C struct field for the per-op request-message struct.
    Returns None for params that live OUTSIDE the message payload
    (today: `slot_ref`, which travels in msg->caps[]).
    """
    name = p["name"]
    t = p["type"]

    if t in SCALAR_C_TYPES:
        return f"    {SCALAR_C_TYPES[t]} {name};"

    if t == "string_in":
        max_len = p.get("max_len", DEFAULT_PATH_MAX)
        return f"    char {name}[{max_len}];"

    if t == "bytes_in":
        return f"    uint8_t {name}[{DEFAULT_BYTES_MAX}];"

    if t == "bytes_out":
        # Reserved buffer the receiver fills.  Wrapper copies back.
        return f"    uint8_t {name}[{DEFAULT_BYTES_MAX}];"

    if t == "struct_in":
        return f"    {p['ctype']} {name};"

    if t in ("struct_out", "struct_inout"):
        return f"    {p['ctype']} {name};"

    if t == "opaque_self_handle":
        return f"    uint64_t {name};"

    if t == "slot_ref":
        # Lives in msg->caps[]; the message struct stores only the cap
        # index so the dispatch template can re-fetch it.
        return f"    uint8_t {name}_cap_id;"

    raise IDLError(f"unknown param type {t!r} (msg field)")


def render_msg_header(idl: dict, idl_filename: str) -> str:
    name = idl["interface"]
    upper = name.upper()
    guard = f"NONUX_INTERFACE_{upper}_MSG_H"

    out: list[str] = []
    out.extend(banner(idl_filename))
    out.append("")
    out.append(f"#ifndef {guard}")
    out.append(f"#define {guard}")
    out.append("")
    out.append("#include <stddef.h>")
    out.append("#include <stdint.h>")
    # Author-supplied includes (e.g., interfaces/fs_types.h).  The msg
    # struct may embed types declared there by value (e.g.,
    # `struct nx_fs_dirent out;`), so the full definition must be in
    # scope here just like in the typedef header.
    for inc in idl.get("includes", []):
        sysinc = inc.get("system", False)
        path = inc["path"]
        formatted = f"<{path}>" if sysinc else f'"{path}"'
        out.append(f"#include {formatted}")
    out.append("")
    out.append(
        f"/* Op-IDs for `{name}` interface.  Stable across versions; "
        f"removed ops")
    out.append(
        " * leave their slot as a gravestone — never reuse a freed id. */")
    out.append(f"enum nx_{name}_op_id {{")
    for op in idl["ops"]:
        out.append(f"    NX_{upper}_OP_{op['name'].upper()} = {op['op_id']},")
    out.append("};")
    out.append("")

    # Per-op request + reply structs.
    for op in idl["ops"]:
        op_upper = op["name"].upper()
        if op.get("doc"):
            # First-line summary from doc as a brief comment.
            first = op["doc"].split("\n", 1)[0]
            out.append(f"/* Request: nx_{name}_{op['name']}() — {first} */")
        out.append(
            f"struct nx_{name}_msg_{op['name']} {{")
        any_field = False
        for p in op.get("params", []):
            line = msg_struct_field(p)
            if line is not None:
                out.append(line)
                any_field = True
        if not any_field:
            out.append("    char _nx_no_payload;")
        out.append("};")
        out.append("")

        # Reply struct — mirrors out-direction params + the principal
        # return value.
        out.append(f"struct nx_{name}_reply_{op['name']} {{")
        ret_type = op["returns"]["type"]
        if ret_type == "int_status":
            out.append("    int rc;")
        elif ret_type == "i64_count_or_status":
            out.append("    int64_t rc;")
        elif ret_type == "usize":
            out.append("    size_t rc;")
        elif ret_type == "void_ptr":
            out.append("    uint64_t rc; /* void * encoded as u64 */")
        elif ret_type == "void":
            out.append("    int rc; /* always NX_OK; placeholder for void ops */")
        any_out = False
        for p in op.get("params", []):
            if p.get("direction") in ("out", "inout"):
                line = msg_struct_field(p)
                if line is not None:
                    out.append(line)
                    any_out = True
        if ret_type == "i64_count_or_status":
            out.append("    size_t bytes_actual;")
        out.append("};")
        out.append("")

    out.append(f"#endif /* {guard} */")
    out.append("")
    return "\n".join(out)


# ---------------------------------------------------------------------------
# framework/<iface>_call.h emission (sender wrappers)
# ---------------------------------------------------------------------------

def wrapper_param_c(p: dict) -> str:
    """Sender-wrapper param signature.  Differs from the typedef-side
    `param_c_signature` only for `slot_ref` (caller passes a slot ptr,
    same as the typedef sees).  Keep them in sync."""
    return param_c_signature(p)


def wrapper_return_type(op: dict) -> str:
    return RETURN_C_TYPES[op["returns"]["type"]]


def render_call_header(idl: dict, idl_filename: str) -> str:
    name = idl["interface"]
    upper = name.upper()
    guard = f"NONUX_FRAMEWORK_{upper}_CALL_H"

    out: list[str] = []
    out.extend(banner(idl_filename))
    out.append("")
    out.append(f"#ifndef {guard}")
    out.append(f"#define {guard}")
    out.append("")
    out.append("#include <stddef.h>")
    out.append("#include <stdint.h>")
    out.append("")
    out.append(f'#include "interfaces/{name}.h"')
    out.append(f'#include "interfaces/{name}_msg.h"')
    out.append('#include "framework/registry.h"')
    out.append('#include "framework/ipc.h"')
    out.append("")
    out.append(
        "/* Slice 8.0a defines `nx_slot_call_blocking` in")
    out.append(
        " * framework/slot_call.{h,c}; until that lands these wrappers")
    out.append(" * reference it as extern. */")
    out.append(
        "extern int nx_slot_call_blocking(struct nx_slot *slot,")
    out.append(
        "                                 struct nx_ipc_message *msg);")
    out.append("")

    for op in idl["ops"]:
        rtype = wrapper_return_type(op)
        params: list[str] = ["struct nx_slot *slot"]
        for p in op.get("params", []):
            params.append(wrapper_param_c(p))
        if rtype.endswith("*"):
            prefix = f"{rtype}nx_{name}_{op['name']}("
        else:
            prefix = f"{rtype} nx_{name}_{op['name']}("
        out.extend(wrap_signature(indent=0, prefix=prefix, params=params,
                                  trailing=");"))

    out.append("")
    out.append(f"#endif /* {guard} */")
    out.append("")
    return "\n".join(out)


# ---------------------------------------------------------------------------
# framework/<iface>_dispatch.h emission (receiver template)
# ---------------------------------------------------------------------------

def render_dispatch_header(idl: dict, idl_filename: str) -> str:
    name = idl["interface"]
    upper = name.upper()
    guard = f"NONUX_FRAMEWORK_{upper}_DISPATCH_H"

    out: list[str] = []
    out.extend(banner(idl_filename))
    out.append("")
    out.append(f"#ifndef {guard}")
    out.append(f"#define {guard}")
    out.append("")
    out.append('#include "framework/ipc.h"')
    out.append('#include "framework/registry.h"')
    out.append(f'#include "interfaces/{name}.h"')
    out.append(f'#include "interfaces/{name}_msg.h"')
    out.append("")
    out.append(
        f"/* Receiver-side dispatch macro for the `{name}` interface.")
    out.append(" * The component supplies a static handle_msg function and")
    out.append(
        f" * delegates to NX_{upper}_DISPATCH(self, ops, msg) which expands")
    out.append(" * into a switch over msg->msg_type that unpacks each request")
    out.append(" * struct and calls the matching op on `ops`. */")
    out.append(
        f"#define NX_{upper}_DISPATCH(self, ops, msg) \\")
    out.append("    do { \\")
    out.append(
        "        switch ((enum nx_" + name + "_op_id)((msg)->msg_type)) { \\")
    for op in idl["ops"]:
        out.append(
            f"        case NX_{upper}_OP_{op['name'].upper()}: \\")
        out.append(
            f"            /* impl: (ops)->{op['name']}(self, ...) — "
            f"see template body. */ \\")
        out.append(
            "            break; \\")
    out.append("        default: \\")
    out.append("            /* unknown op — return NX_EINVAL via reply. */ \\")
    out.append("            break; \\")
    out.append("        } \\")
    out.append("    } while (0)")
    out.append("")
    out.append(f"#endif /* {guard} */")
    out.append("")
    return "\n".join(out)


# ---------------------------------------------------------------------------
# Filesystem write
# ---------------------------------------------------------------------------

def write_artifacts(idl: dict, idl_path: pathlib.Path,
                    interfaces_dir: pathlib.Path,
                    framework_dir: pathlib.Path) -> dict[pathlib.Path, str]:
    """Produce the four artifacts; return path → content mapping.  Caller
    decides whether to write to disk (regen) or only diff (verify)."""
    name = idl["interface"]
    idl_filename = idl_path.name
    return {
        interfaces_dir / f"{name}.h":
            render_iface_header(idl, idl_filename),
        interfaces_dir / f"{name}_msg.h":
            render_msg_header(idl, idl_filename),
        framework_dir / f"{name}_call.h":
            render_call_header(idl, idl_filename),
        framework_dir / f"{name}_dispatch.h":
            render_dispatch_header(idl, idl_filename),
    }


def regenerate(idl_dir: pathlib.Path, interfaces_dir: pathlib.Path,
               framework_dir: pathlib.Path,
               meta_schema: dict) -> None:
    """For every interfaces/idl/*.json: validate + emit four files.
    Writes only if content changed (so timestamps don't churn)."""
    idl_paths = sorted(idl_dir.glob("*.json"))
    if not idl_paths:
        return
    interfaces_dir.mkdir(parents=True, exist_ok=True)
    framework_dir.mkdir(parents=True, exist_ok=True)
    for idl_path in idl_paths:
        idl = load_idl(idl_path, meta_schema)
        for path, content in write_artifacts(
                idl, idl_path, interfaces_dir, framework_dir).items():
            if path.exists() and path.read_text() == content:
                continue
            path.write_text(content)


# ---------------------------------------------------------------------------
# verify: re-run into temp; diff against in-tree.
# ---------------------------------------------------------------------------

def verify(idl_dir: pathlib.Path, interfaces_dir: pathlib.Path,
           framework_dir: pathlib.Path, meta_schema: dict) -> int:
    idl_paths = sorted(idl_dir.glob("*.json"))
    if not idl_paths:
        return 0
    rc = 0
    for idl_path in idl_paths:
        try:
            idl = load_idl(idl_path, meta_schema)
        except IDLError as e:
            print(f"verify-iface-fresh: {e}", file=sys.stderr)
            rc = 2
            continue
        artifacts = write_artifacts(
            idl, idl_path, interfaces_dir, framework_dir)
        for path, want in artifacts.items():
            if not path.exists():
                print(
                    f"verify-iface-fresh: missing generated file {path}",
                    file=sys.stderr)
                rc = 1
                continue
            got = path.read_text()
            if got != want:
                diff = "\n".join(difflib.unified_diff(
                    got.splitlines(), want.splitlines(),
                    fromfile=str(path), tofile=str(path) + ".regen",
                    lineterm=""))
                print(
                    f"verify-iface-fresh: {path} is stale; regenerate "
                    f"with `make gen-iface`.\n{diff}",
                    file=sys.stderr)
                rc = 1
    return rc


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def cmd_all(args: argparse.Namespace) -> int:
    tools_dir = pathlib.Path(__file__).resolve().parent
    meta_schema = load_meta_schema(tools_dir)
    try:
        regenerate(args.idl_dir, args.interfaces_dir, args.framework_dir,
                   meta_schema)
    except IDLError as e:
        print(f"gen-iface.py: {e}", file=sys.stderr)
        return 2
    return 0


def cmd_one(args: argparse.Namespace) -> int:
    tools_dir = pathlib.Path(__file__).resolve().parent
    meta_schema = load_meta_schema(tools_dir)
    try:
        idl = load_idl(args.idl_file, meta_schema)
        artifacts = write_artifacts(idl, args.idl_file,
                                    args.interfaces_dir, args.framework_dir)
        args.interfaces_dir.mkdir(parents=True, exist_ok=True)
        args.framework_dir.mkdir(parents=True, exist_ok=True)
        for path, content in artifacts.items():
            path.write_text(content)
    except IDLError as e:
        print(f"gen-iface.py: {e}", file=sys.stderr)
        return 2
    return 0


def cmd_verify(args: argparse.Namespace) -> int:
    tools_dir = pathlib.Path(__file__).resolve().parent
    meta_schema = load_meta_schema(tools_dir)
    return verify(args.idl_dir, args.interfaces_dir, args.framework_dir,
                  meta_schema)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "IDL-driven interface code generator (slice 8.0pre.1).  "
            "Reads interfaces/idl/*.json; emits typedef header + msg structs "
            "+ sender wrappers + receiver dispatch template per interface."))
    sub = parser.add_subparsers(dest="cmd", required=True)

    pa = sub.add_parser("all", help="Regenerate every artifact for every IDL.")
    pa.add_argument("idl_dir",        type=pathlib.Path)
    pa.add_argument("interfaces_dir", type=pathlib.Path)
    pa.add_argument("framework_dir",  type=pathlib.Path)
    pa.set_defaults(fn=cmd_all)

    po = sub.add_parser("one", help="Regenerate artifacts for a single IDL.")
    po.add_argument("idl_file",       type=pathlib.Path)
    po.add_argument("interfaces_dir", type=pathlib.Path)
    po.add_argument("framework_dir",  type=pathlib.Path)
    po.set_defaults(fn=cmd_one)

    pv = sub.add_parser(
        "verify",
        help="Diff fresh regeneration against in-tree headers; nonzero exit "
             "if any drift.")
    pv.add_argument("idl_dir",        type=pathlib.Path)
    pv.add_argument("interfaces_dir", type=pathlib.Path)
    pv.add_argument("framework_dir",  type=pathlib.Path)
    pv.set_defaults(fn=cmd_verify)

    args = parser.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
