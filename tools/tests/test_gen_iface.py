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


class TestFsByteForByte(unittest.TestCase):
    """Slice 8.0pre.2: in-tree interfaces/fs.h is byte-identical to a
    fresh regeneration from interfaces/idl/fs.json.  Both fs.json and
    vfs.json declare interfaces/fs_types.h via the `includes:` array
    so the typedef header transitively pulls in struct nx_fs_dirent /
    struct nx_fs_stat from a hand-written types header — IDL describes
    operations only; data layout stays in C."""

    def test_fs_h_is_canonical(self):
        idl_path  = ROOT / "interfaces" / "idl" / "fs.json"
        intree_h  = ROOT / "interfaces" / "fs.h"
        meta_schema = gi.load_meta_schema(ROOT / "tools")
        idl = gi.load_idl(idl_path, meta_schema)
        regenerated = gi.render_iface_header(idl, idl_path.name)
        self.assertEqual(intree_h.read_text(), regenerated)

    def test_fs_idl_declares_types_header_include(self):
        idl_path  = ROOT / "interfaces" / "idl" / "fs.json"
        meta_schema = gi.load_meta_schema(ROOT / "tools")
        idl = gi.load_idl(idl_path, meta_schema)
        paths = [inc["path"] for inc in idl.get("includes", [])]
        self.assertIn("interfaces/fs_types.h", paths)


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
    """Receiver dispatch function shape."""

    def test_dispatch_macro_covers_every_op(self):
        d = gi.render_dispatch_header(SAMPLE_IDL, "demo.json")
        self.assertIn("static inline int nx_demo_dispatch(", d)
        self.assertIn("case NX_DEMO_OP_PING:", d)
        self.assertIn("case NX_DEMO_OP_BLOB:", d)


class TestIncludesAuthorDeclared(unittest.TestCase):
    """Slice 8.0pre.2: the IDL's `includes:` array names hand-written
    headers that provide types referenced by op-param ctypes.  The
    generator emits those includes in BOTH the typedef header and the
    msg header (the msg header embeds struct values by sizeof, so it
    needs the full def too).  When `includes:` is non-empty, the
    auto-detected forward-declaration list is suppressed — the
    author's includes carry the definitions."""

    def _idl_with_include(self) -> dict:
        idl = copy.deepcopy(SAMPLE_IDL)
        idl["includes"] = [
            {"path": "interfaces/demo_types.h",
             "doc": "Hand-written types shared with another iface."}
        ]
        idl["ops"].append({
            "name": "fetch", "op_id": 3,
            "params": [
                {"name": "out", "type": "struct_out",
                 "ctype": "struct nx_demo_blob", "direction": "out"}
            ],
            "returns": {"type": "int_status"},
        })
        return idl

    def test_include_emitted_in_typedef_header(self):
        h = gi.render_iface_header(self._idl_with_include(), "demo.json")
        self.assertIn('#include "interfaces/demo_types.h"', h)

    def test_include_emitted_in_msg_header(self):
        m = gi.render_msg_header(self._idl_with_include(), "demo.json")
        self.assertIn('#include "interfaces/demo_types.h"', m)

    def test_includes_present_suppresses_forward_decls(self):
        # When includes: is non-empty, types referenced via op-param
        # ctypes are assumed provided by the include — no auto-detected
        # forward decl emitted (which would shadow the include's full def).
        h = gi.render_iface_header(self._idl_with_include(), "demo.json")
        self.assertNotIn("\nstruct nx_demo_blob;\n", h)

    def test_no_includes_keeps_forward_decls(self):
        # Without includes: the auto-detected forward decls still fire
        # (legacy behavior; required for interfaces that consume types
        # via opaque pointer without including the defining header).
        idl = copy.deepcopy(SAMPLE_IDL)
        idl["ops"].append({
            "name": "fetch", "op_id": 3,
            "params": [
                {"name": "out", "type": "struct_out",
                 "ctype": "struct nx_demo_blob", "direction": "out"}
            ],
            "returns": {"type": "int_status"},
        })
        h = gi.render_iface_header(idl, "demo.json")
        self.assertIn("struct nx_demo_blob;", h)

    def test_msg_header_includes_match_typedef_header(self):
        """The IDL's `includes:` should appear in BOTH headers — the
        typedef sees them for forward-decl suppression and signature
        types; the msg header sees them for embedded struct sizeof."""
        idl = self._idl_with_include()
        h = gi.render_iface_header(idl, "demo.json")
        m = gi.render_msg_header(idl, "demo.json")
        self.assertIn('#include "interfaces/demo_types.h"', h)
        self.assertIn('#include "interfaces/demo_types.h"', m)


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


class TestSchedulerByteForByte(unittest.TestCase):
    """Slice 8.0pre.3: in-tree interfaces/scheduler.h is byte-identical
    to a fresh regeneration from interfaces/idl/scheduler.json.  This
    IDL was the first to exercise typed `opaque_self_handle.ctype` (for
    `struct nx_task *task` params) and typed `void_ptr.ctype` (for
    `pick_next` returning `struct nx_task *`)."""

    def test_scheduler_h_is_canonical(self):
        idl_path  = ROOT / "interfaces" / "idl" / "scheduler.json"
        intree_h  = ROOT / "interfaces" / "scheduler.h"
        meta_schema = gi.load_meta_schema(ROOT / "tools")
        idl = gi.load_idl(idl_path, meta_schema)
        regenerated = gi.render_iface_header(idl, idl_path.name)
        self.assertEqual(intree_h.read_text(), regenerated)

    def test_scheduler_idl_uses_typed_opaque_handle(self):
        idl_path  = ROOT / "interfaces" / "idl" / "scheduler.json"
        meta_schema = gi.load_meta_schema(ROOT / "tools")
        idl = gi.load_idl(idl_path, meta_schema)
        # enqueue's `task` param uses the new typed-opaque-handle shape.
        enqueue = next(op for op in idl["ops"] if op["name"] == "enqueue")
        task_p  = enqueue["params"][0]
        self.assertEqual(task_p["type"], "opaque_self_handle")
        self.assertEqual(task_p["ctype"], "struct nx_task")
        # pick_next's return uses the new typed-void_ptr shape.
        pick    = next(op for op in idl["ops"] if op["name"] == "pick_next")
        self.assertEqual(pick["returns"]["type"], "void_ptr")
        self.assertEqual(pick["returns"]["ctype"], "struct nx_task")


class TestMmByteForByte(unittest.TestCase):
    """Slice 8.0pre.3: in-tree interfaces/mm.h is byte-identical to a
    fresh regeneration from interfaces/idl/mm.json.  This IDL was the
    first to exercise the `u32` return type (mm.max_order)."""

    def test_mm_h_is_canonical(self):
        idl_path  = ROOT / "interfaces" / "idl" / "mm.json"
        intree_h  = ROOT / "interfaces" / "mm.h"
        meta_schema = gi.load_meta_schema(ROOT / "tools")
        idl = gi.load_idl(idl_path, meta_schema)
        regenerated = gi.render_iface_header(idl, idl_path.name)
        self.assertEqual(intree_h.read_text(), regenerated)


class TestCharDeviceByteForByte(unittest.TestCase):
    """Slice 8.0pre.3: in-tree interfaces/char_device.h is byte-identical
    to a fresh regeneration.  This IDL was authored fresh (no hand-written
    predecessor) and is the first to exercise `context: "irq"`."""

    def test_char_device_h_is_canonical(self):
        idl_path  = ROOT / "interfaces" / "idl" / "char_device.json"
        intree_h  = ROOT / "interfaces" / "char_device.h"
        meta_schema = gi.load_meta_schema(ROOT / "tools")
        idl = gi.load_idl(idl_path, meta_schema)
        regenerated = gi.render_iface_header(idl, idl_path.name)
        self.assertEqual(intree_h.read_text(), regenerated)

    def test_char_device_isr_header_is_canonical(self):
        idl_path  = ROOT / "interfaces" / "idl" / "char_device.json"
        intree_h  = ROOT / "framework" / "char_device_isr.h"
        meta_schema = gi.load_meta_schema(ROOT / "tools")
        idl = gi.load_idl(idl_path, meta_schema)
        regenerated = gi.render_isr_header(idl, idl_path.name)
        self.assertEqual(intree_h.read_text(), regenerated)


class TestIrqEntryArtifact(unittest.TestCase):
    """Slice 8.0pre.3: framework/<iface>_isr.h emission.  Emitted only
    when at least one op declares `context: "irq"`; carries the pool
    size define + per-IRQ-op `_from_irq` declarations."""

    def _idl_with_irq_op(self) -> dict:
        idl = copy.deepcopy(SAMPLE_IDL)
        idl["ops"].append({
            "name": "rx_byte", "op_id": 3,
            "context": "irq",
            "params": [{"name": "byte", "type": "u8"}],
            "returns": {"type": "void"},
        })
        return idl

    def test_isr_header_emitted_only_for_irq_ops(self):
        # SAMPLE_IDL has no irq ops → no isr header.
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            idl_dir, iface_dir, fw_dir = make_tmpdirs(tmp)
            idl_path = idl_dir / "demo.json"
            idl_path.write_text(json.dumps(SAMPLE_IDL))
            self.assertEqual(
                gi.main(["all", str(idl_dir), str(iface_dir), str(fw_dir)]),
                0)
            self.assertFalse((fw_dir / "demo_isr.h").exists())

    def test_isr_header_emitted_when_op_has_irq_context(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            idl_dir, iface_dir, fw_dir = make_tmpdirs(tmp)
            idl_path = idl_dir / "demo.json"
            idl_path.write_text(json.dumps(self._idl_with_irq_op()))
            self.assertEqual(
                gi.main(["all", str(idl_dir), str(iface_dir), str(fw_dir)]),
                0)
            isr = (fw_dir / "demo_isr.h").read_text()
            self.assertIn("NX_DEMO_ISR_POOL_SIZE", isr)
            # Per-IRQ-op `_from_irq` wrapper declared.
            self.assertIn("nx_demo_rx_byte_from_irq(struct nx_slot *slot,", isr)
            # The non-irq ops in SAMPLE_IDL must NOT get _from_irq wrappers.
            self.assertNotIn("nx_demo_ping_from_irq", isr)
            self.assertNotIn("nx_demo_blob_from_irq", isr)

    def test_pool_size_default_is_thirty_two(self):
        h = gi.render_isr_header(self._idl_with_irq_op(), "demo.json")
        self.assertIn("#define NX_DEMO_ISR_POOL_SIZE 32", h)


class TestTypedOpaqueSelfHandle(unittest.TestCase):
    """Slice 8.0pre.3: opaque_self_handle params with `ctype` emit
    typed C signatures (e.g. `struct nx_task *task`) instead of the
    default `void *`.  Wire shape unchanged."""

    def test_no_ctype_emits_void_ptr(self):
        idl = copy.deepcopy(SAMPLE_IDL)
        idl["ops"].append({
            "name": "lookup", "op_id": 3,
            "params": [{"name": "h", "type": "opaque_self_handle"}],
            "returns": {"type": "int_status"},
        })
        h = gi.render_iface_header(idl, "demo.json")
        self.assertIn("void *h", h)

    def test_ctype_emits_typed_pointer(self):
        idl = copy.deepcopy(SAMPLE_IDL)
        idl["ops"].append({
            "name": "lookup", "op_id": 3,
            "params": [{"name": "task", "type": "opaque_self_handle",
                        "ctype": "struct nx_task"}],
            "returns": {"type": "int_status"},
        })
        h = gi.render_iface_header(idl, "demo.json")
        self.assertIn("struct nx_task *task", h)

    def test_ctype_with_direction_out_emits_double_pointer(self):
        idl = copy.deepcopy(SAMPLE_IDL)
        idl["ops"].append({
            "name": "alloc", "op_id": 3,
            "params": [{"name": "out_task", "type": "opaque_self_handle",
                        "ctype": "struct nx_task", "direction": "out"}],
            "returns": {"type": "int_status", "out_param": "out_task"},
        })
        h = gi.render_iface_header(idl, "demo.json")
        self.assertIn("struct nx_task **out_task", h)

    def test_ctype_struct_participates_in_forward_decl_set(self):
        idl = copy.deepcopy(SAMPLE_IDL)
        idl["ops"].append({
            "name": "lookup", "op_id": 3,
            "params": [{"name": "task", "type": "opaque_self_handle",
                        "ctype": "struct nx_task"}],
            "returns": {"type": "int_status"},
        })
        h = gi.render_iface_header(idl, "demo.json")
        # No includes: → forward decl emits.
        self.assertIn("struct nx_task;", h)


class TestTypedVoidPtrReturn(unittest.TestCase):
    """Slice 8.0pre.3: void_ptr returns with `ctype` emit typed C
    return signatures (e.g. `struct nx_task *`) instead of the default
    `void *`.  Wire shape unchanged."""

    def test_no_ctype_emits_void_ptr(self):
        idl = copy.deepcopy(SAMPLE_IDL)
        idl["ops"].append({
            "name": "alloc", "op_id": 3,
            "params": [], "returns": {"type": "void_ptr"},
        })
        h = gi.render_iface_header(idl, "demo.json")
        self.assertIn("void *(*alloc)(void *self)", h)

    def test_ctype_emits_typed_return(self):
        idl = copy.deepcopy(SAMPLE_IDL)
        idl["ops"].append({
            "name": "pick_next", "op_id": 3,
            "params": [],
            "returns": {"type": "void_ptr", "ctype": "struct nx_task"},
        })
        h = gi.render_iface_header(idl, "demo.json")
        self.assertIn("struct nx_task *(*pick_next)(void *self)", h)

    def test_returned_ctype_struct_participates_in_forward_decl_set(self):
        idl = copy.deepcopy(SAMPLE_IDL)
        idl["ops"].append({
            "name": "pick_next", "op_id": 3,
            "params": [],
            "returns": {"type": "void_ptr", "ctype": "struct nx_task"},
        })
        h = gi.render_iface_header(idl, "demo.json")
        self.assertIn("struct nx_task;", h)


class TestU32Return(unittest.TestCase):
    """Slice 8.0pre.3: `u32` return type (and `u64`) added for scalar
    unsigned-int returns like mm.max_order; the asymmetric u32→uint32_t
    convention applies on the return side too."""

    def test_u32_return_emits_uint32_t(self):
        idl = copy.deepcopy(SAMPLE_IDL)
        idl["ops"].append({
            "name": "max_order", "op_id": 3,
            "params": [], "returns": {"type": "u32"},
        })
        h = gi.render_iface_header(idl, "demo.json")
        self.assertIn("uint32_t (*max_order)(void *self)", h)

    def test_u32_return_in_reply_struct_uses_uint32_t(self):
        idl = copy.deepcopy(SAMPLE_IDL)
        idl["ops"].append({
            "name": "max_order", "op_id": 3,
            "params": [], "returns": {"type": "u32"},
        })
        m = gi.render_msg_header(idl, "demo.json")
        self.assertIn("struct nx_demo_reply_max_order {", m)
        # Reply struct's rc field carries the u32 value.
        idx = m.index("struct nx_demo_reply_max_order {")
        end = m.index("};", idx)
        self.assertIn("uint32_t rc;", m[idx:end])
