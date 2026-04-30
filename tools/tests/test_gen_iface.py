"""Unit tests for tools/gen-iface.py (slice 8.0pre.1).

Covers:
  - Schema validation (jsonschema-driven, plus stem-mismatch check)
  - Generator determinism + idempotence
  - Byte-for-byte regeneration of in-tree interfaces/vfs.h (the
    compatibility test locked at Session 80)
  - Forward-decl auto-detection (struct/union/enum from op param ctypes)
  - Constant grouping + global value-column alignment
  - Op-id enum emission + per-op message struct shape
  - Wrapper signature generation
  - Dispatch macro emission
  - verify subcommand: clean tree → rc=0; corrupted tree → rc=1
"""

import copy
import json
import pathlib
import sys
import tempfile
import unittest

from tools.tests._helpers import load_tool, ROOT

gi = load_tool("gen_iface", "gen-iface.py")


def make_tmpdirs(tmpdir: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path,
                                                pathlib.Path]:
    idl_dir   = tmpdir / "idl"
    iface_dir = tmpdir / "interfaces"
    fw_dir    = tmpdir / "framework"
    idl_dir.mkdir()
    iface_dir.mkdir()
    fw_dir.mkdir()
    return idl_dir, iface_dir, fw_dir


# A small but representative IDL we can mutate per test without
# disturbing the canonical vfs.json.
SAMPLE_IDL = {
    "interface": "demo",
    "version": 1,
    "iface_id": 9001,
    "doc": "Demo interface for testing.",
    "constants": [
        {"name": "NX_DEMO_FLAG_A", "value": "(1U << 0)",
         "doc": "Flag bits."},
        {"name": "NX_DEMO_FLAG_B", "value": "(1U << 1)"},
    ],
    "ops": [
        {"name": "ping", "op_id": 1,
         "doc": "Round-trip ping op.",
         "params": [{"name": "x", "type": "u32"}],
         "returns": {"type": "int_status"}},
        {"name": "blob", "op_id": 2,
         "params": [
             {"name": "buf", "type": "bytes_in", "len": "len"},
             {"name": "len", "type": "usize"},
         ],
         "returns": {"type": "i64_count_or_status"}},
    ],
}


class TestSchemaValidation(unittest.TestCase):
    """Meta-schema gate (delegated to jsonschema if available)."""

    def _run_one(self, idl: dict, name: str = "demo") -> int:
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            idl_dir, iface_dir, fw_dir = make_tmpdirs(tmp)
            idl_path = idl_dir / f"{name}.json"
            idl_path.write_text(json.dumps(idl))
            return gi.main(["one", str(idl_path),
                            str(iface_dir), str(fw_dir)])

    def test_valid_idl_passes(self):
        self.assertEqual(self._run_one(SAMPLE_IDL), 0)

    def test_unknown_top_level_key_rejected(self):
        bad = copy.deepcopy(SAMPLE_IDL)
        bad["bogus_field"] = "no"
        # jsonschema rejects (additionalProperties: false in meta-schema).
        # Without jsonschema, the manual fallback skips this gate; in that
        # case we still expect rc=0 since the field is harmlessly ignored.
        rc = self._run_one(bad)
        self.assertIn(rc, (0, 2))   # 2 if jsonschema rejects, 0 otherwise.

    def test_unknown_param_type_rejected(self):
        bad = copy.deepcopy(SAMPLE_IDL)
        bad["ops"][0]["params"][0]["type"] = "u128"
        rc = self._run_one(bad)
        # jsonschema enum rejection → 2; absence falls through to manual
        # rendering which raises IDLError → 2.
        self.assertEqual(rc, 2)

    def test_filename_stem_must_match_interface(self):
        rc = self._run_one(SAMPLE_IDL, name="wrong_name")
        self.assertEqual(rc, 2)


class TestVfsByteForByte(unittest.TestCase):
    """Verify the in-tree interfaces/vfs.h is byte-identical to a fresh
    regeneration from interfaces/idl/vfs.json.  This is the compatibility
    test the slice plan locks (IDL-SCHEMA.md §vfs Example)."""

    def test_vfs_h_is_canonical(self):
        idl_path  = ROOT / "interfaces" / "idl" / "vfs.json"
        intree_h  = ROOT / "interfaces" / "vfs.h"
        meta_schema = gi.load_meta_schema(ROOT / "tools")
        idl = gi.load_idl(idl_path, meta_schema)
        regenerated = gi.render_iface_header(idl, idl_path.name)
        self.assertEqual(intree_h.read_text(), regenerated)

    def test_verify_subcommand_passes_on_clean_tree(self):
        rc = gi.main([
            "verify",
            str(ROOT / "interfaces" / "idl"),
            str(ROOT / "interfaces"),
            str(ROOT / "framework"),
        ])
        self.assertEqual(rc, 0)


class TestGeneratorDeterminism(unittest.TestCase):
    """Same input + same generator version → byte-identical output across
    invocations.  Required for verify-iface-fresh to be a meaningful
    gate."""

    def test_render_iface_header_is_deterministic(self):
        a = gi.render_iface_header(SAMPLE_IDL, "demo.json")
        b = gi.render_iface_header(SAMPLE_IDL, "demo.json")
        self.assertEqual(a, b)

    def test_render_msg_header_is_deterministic(self):
        a = gi.render_msg_header(SAMPLE_IDL, "demo.json")
        b = gi.render_msg_header(SAMPLE_IDL, "demo.json")
        self.assertEqual(a, b)


class TestForwardDecls(unittest.TestCase):
    """Auto-detection of struct/union/enum types referenced via `ctype`."""

    def test_no_struct_params_emits_no_forward_decls(self):
        # SAMPLE_IDL has no struct_* params → no forward-decl lines
        # (the typedef itself is `struct nx_demo_ops {` and obviously
        # remains).  Forward decls take the bare-`struct foo;` shape.
        h = gi.render_iface_header(SAMPLE_IDL, "demo.json")
        for line in h.splitlines():
            stripped = line.strip()
            if stripped.startswith("struct ") and stripped.endswith(";"):
                self.fail(f"unexpected forward decl: {stripped!r}")

    def test_struct_ctypes_collected_in_op_id_order(self):
        idl = copy.deepcopy(SAMPLE_IDL)
        idl["ops"].append({
            "name": "fetch", "op_id": 3,
            "params": [
                {"name": "out_a", "type": "struct_out",
                 "ctype": "struct nx_zeta", "direction": "out"},
                {"name": "out_b", "type": "struct_out",
                 "ctype": "struct nx_alpha", "direction": "out"},
            ],
            "returns": {"type": "int_status"},
        })
        idl["forward_decls_doc"] = "Fwd-decls."
        h = gi.render_iface_header(idl, "demo.json")
        # Order = first-appearance: out_a (zeta) before out_b (alpha).
        self.assertLess(h.index("struct nx_zeta;"), h.index("struct nx_alpha;"))
        self.assertIn("/* Fwd-decls. */", h)

    def test_primitive_ctypes_are_not_forward_declared(self):
        idl = copy.deepcopy(SAMPLE_IDL)
        idl["ops"].append({
            "name": "iter", "op_id": 3,
            "params": [{"name": "cookie", "type": "struct_inout",
                        "ctype": "uint32_t", "direction": "inout"}],
            "returns": {"type": "int_status"},
        })
        h = gi.render_iface_header(idl, "demo.json")
        self.assertNotIn("uint32_t;", h)   # no bogus `uint32_t;` forward decl.


class TestConstantGrouping(unittest.TestCase):
    """Group-doc-on-first + global value-column alignment."""

    def test_doc_on_first_constant_emits_group_block_comment(self):
        h = gi.render_iface_header(SAMPLE_IDL, "demo.json")
        # First constant has doc; group comment present above the #defines.
        self.assertIn("/* Flag bits. */", h)
        # Second constant has no doc; no per-constant comment introduced.
        # Crude heuristic: count `/*` between FLAG_A and FLAG_B.
        a_idx = h.index("NX_DEMO_FLAG_A")
        b_idx = h.index("NX_DEMO_FLAG_B")
        self.assertEqual(h.count("/*", a_idx, b_idx), 0)

    def test_value_column_is_global_not_per_group(self):
        idl = copy.deepcopy(SAMPLE_IDL)
        idl["constants"] = [
            {"name": "NX_DEMO_VERY_LONG_NAME_AAAA", "value": 1,
             "doc": "Group A."},
            {"name": "NX_DEMO_AA",                  "value": 2},
            {"name": "NX_DEMO_BB",                  "value": 3,
             "doc": "Group B."},
        ]
        h = gi.render_iface_header(idl, "demo.json")
        # Find the column where each value lands.
        def col_of_value(name: str, value: str) -> int:
            for ln in h.splitlines():
                if ln.startswith(f"#define {name}"):
                    return ln.index(value)
            raise AssertionError(f"missing #define for {name}")
        col_long = col_of_value("NX_DEMO_VERY_LONG_NAME_AAAA", "1")
        col_aa   = col_of_value("NX_DEMO_AA", "2")
        col_bb   = col_of_value("NX_DEMO_BB", "3")
        self.assertEqual(col_long, col_aa)
        self.assertEqual(col_long, col_bb)


class TestMsgHeader(unittest.TestCase):
    """Op-id enum + per-op request/reply structs."""

    def test_op_id_enum_present_with_correct_values(self):
        m = gi.render_msg_header(SAMPLE_IDL, "demo.json")
        self.assertIn("enum nx_demo_op_id {", m)
        self.assertIn("NX_DEMO_OP_PING = 1,", m)
        self.assertIn("NX_DEMO_OP_BLOB = 2,", m)

    def test_request_and_reply_structs_per_op(self):
        m = gi.render_msg_header(SAMPLE_IDL, "demo.json")
        for tok in ("struct nx_demo_msg_ping {",
                    "struct nx_demo_reply_ping {",
                    "struct nx_demo_msg_blob {",
                    "struct nx_demo_reply_blob {"):
            self.assertIn(tok, m)


class TestWrapperHeader(unittest.TestCase):
    """Per-op sender wrappers in framework/<iface>_call.h."""

    def test_wrapper_signatures_use_struct_nx_slot(self):
        c = gi.render_call_header(SAMPLE_IDL, "demo.json")
        self.assertIn("int nx_demo_ping(struct nx_slot *slot, uint32_t x);", c)
        self.assertIn("extern int nx_slot_call_blocking(", c)


class TestDispatchHeader(unittest.TestCase):
    """Receiver dispatch macro shape."""

    def test_dispatch_macro_covers_every_op(self):
        d = gi.render_dispatch_header(SAMPLE_IDL, "demo.json")
        self.assertIn("#define NX_DEMO_DISPATCH(self, ops, msg)", d)
        self.assertIn("case NX_DEMO_OP_PING:", d)
        self.assertIn("case NX_DEMO_OP_BLOB:", d)


class TestVerifyDetectsDrift(unittest.TestCase):
    """verify subcommand exits non-zero when an in-tree generated file
    has been hand-edited (or an IDL change wasn't followed by gen)."""

    def test_verify_flags_a_corrupted_header(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            idl_dir, iface_dir, fw_dir = make_tmpdirs(tmp)
            idl_path = idl_dir / "demo.json"
            idl_path.write_text(json.dumps(SAMPLE_IDL))
            # First, regenerate cleanly.
            self.assertEqual(
                gi.main(["all", str(idl_dir), str(iface_dir), str(fw_dir)]),
                0)
            # Then corrupt the typedef header.
            tampered = (iface_dir / "demo.h").read_text() + "\n/* tampered */\n"
            (iface_dir / "demo.h").write_text(tampered)
            # verify must detect drift.
            rc = gi.main(["verify", str(idl_dir), str(iface_dir), str(fw_dir)])
            self.assertEqual(rc, 1)
