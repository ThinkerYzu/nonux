"""Unit tests for tools/verify-registry.py."""

import io
import json
import pathlib
import sys
import tempfile
import textwrap
import unittest
from contextlib import redirect_stderr, redirect_stdout

from tools.tests._helpers import load_tool

vr = load_tool("verify_registry", "verify-registry.py")


def _write_component(comp_dir: pathlib.Path,
                     *, name: str, manifest: dict, src: str) -> None:
    comp_dir.mkdir(parents=True, exist_ok=True)
    (comp_dir / "manifest.json").write_text(json.dumps(manifest))
    (comp_dir / f"{name}.c").write_text(src)


def _run(*argv: str) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = vr.main(list(argv))
    return rc, out.getvalue(), err.getvalue()


class TestListCommand(unittest.TestCase):

    def test_list_shows_every_rule_with_status(self):
        rc, out, _ = _run("--list")
        self.assertEqual(rc, 0)
        for tag in ("R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9"):
            self.assertIn(tag, out)
        self.assertIn("[machine]", out)
        self.assertIn("[ai-verified:", out)


class TestEmptyTree(unittest.TestCase):

    def test_missing_components_dir_passes(self):
        with tempfile.TemporaryDirectory() as d:
            rc, out, err = _run(str(pathlib.Path(d) / "components"))
            self.assertEqual(rc, 0)
            self.assertIn("0 finding(s)", out)
            self.assertEqual(err, "")

    def test_empty_components_dir_passes(self):
        with tempfile.TemporaryDirectory() as d:
            (pathlib.Path(d) / "components").mkdir()
            rc, out, _ = _run(str(pathlib.Path(d) / "components"))
            self.assertEqual(rc, 0)
            self.assertIn("0 finding(s)", out)


class TestR2(unittest.TestCase):
    """Slot fields in struct must match manifest requires/optional."""

    def _mk_tree(self, manifest: dict, src: str) -> pathlib.Path:
        tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(tmp,
                                                            ignore_errors=True))
        comp_dir = tmp / "components" / manifest["name"]
        _write_component(comp_dir, name=manifest["name"],
                         manifest=manifest, src=src)
        return tmp / "components"

    def test_matching_struct_and_manifest_passes(self):
        src = textwrap.dedent("""\
            struct example_state {
                struct nx_slot *timer;
                struct nx_slot *stats;
            };
        """)
        comp = self._mk_tree(
            {"name": "example", "version": "0.1.0",
             "requires": {"timer": {}}, "optional": {"stats": {}}},
            src)
        rc, out, err = _run(str(comp), "--rule", "R2")
        self.assertEqual(rc, 0, f"stderr was: {err}")

    def test_struct_field_without_manifest_entry_fails(self):
        src = textwrap.dedent("""\
            struct example_state {
                struct nx_slot *timer;
                struct nx_slot *rogue;
            };
        """)
        comp = self._mk_tree(
            {"name": "example", "version": "0.1.0",
             "requires": {"timer": {}}},
            src)
        rc, _, err = _run(str(comp), "--rule", "R2")
        self.assertEqual(rc, 2)
        self.assertIn("rogue", err)
        self.assertIn("R2", err)

    def test_manifest_entry_without_struct_field_fails(self):
        src = textwrap.dedent("""\
            struct example_state {
                struct nx_slot *timer;
            };
        """)
        comp = self._mk_tree(
            {"name": "example", "version": "0.1.0",
             "requires": {"timer": {}, "stats": {}}},
            src)
        rc, _, err = _run(str(comp), "--rule", "R2")
        self.assertEqual(rc, 2)
        self.assertIn("stats", err)

    def test_optional_deps_treated_like_requires(self):
        src = textwrap.dedent("""\
            struct example_state {
                struct nx_slot *opt;
            };
        """)
        comp = self._mk_tree(
            {"name": "example", "version": "0.1.0",
             "optional": {"opt": {}}},
            src)
        rc, _, err = _run(str(comp), "--rule", "R2")
        self.assertEqual(rc, 0, f"stderr: {err}")

    def test_column_0_struct_nx_slot_ptr_is_not_flagged(self):
        """Guard against false positives for file-scope / function-scope
        `struct nx_slot *foo;` declarations that aren't struct fields."""
        src = textwrap.dedent("""\
            struct nx_slot *global_placeholder;
            static void helper(void) {
            struct nx_slot *local;
            (void)local;
            }
        """)
        comp = self._mk_tree(
            {"name": "example", "version": "0.1.0"},
            src)
        rc, out, err = _run(str(comp), "--rule", "R2")
        self.assertEqual(rc, 0, f"stderr: {err}, out: {out}")


class TestR4(unittest.TestCase):
    """Retain/release call counts must match."""

    def _mk(self, src: str) -> pathlib.Path:
        tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(tmp,
                                                            ignore_errors=True))
        comp_dir = tmp / "components" / "example"
        _write_component(
            comp_dir, name="example",
            manifest={"name": "example", "version": "0.1.0"},
            src=src)
        return tmp / "components"

    def test_matched_retain_release_passes(self):
        src = """
        static void do_it(void) {
            nx_slot_ref_retain(self, &cap, NULL, NULL);
            nx_slot_ref_release(self, target);
        }
        """
        rc, out, err = _run(str(self._mk(src)), "--rule", "R4")
        self.assertEqual(rc, 0, f"stderr: {err}")

    def test_retain_without_release_fails(self):
        src = """
        static void do_it(void) {
            nx_slot_ref_retain(self, &cap_a, NULL, NULL);
            nx_slot_ref_retain(self, &cap_b, NULL, NULL);
            nx_slot_ref_release(self, target_a);
            /* missing release for cap_b */
        }
        """
        rc, _, err = _run(str(self._mk(src)), "--rule", "R4")
        self.assertEqual(rc, 2)
        self.assertIn("nx_slot_ref_retain with no matching", err)
        self.assertIn("R4", err)

    def test_release_without_retain_fails(self):
        src = """
        static void do_it(void) {
            nx_slot_ref_release(self, orphan);
        }
        """
        rc, _, err = _run(str(self._mk(src)), "--rule", "R4")
        self.assertEqual(rc, 2)
        self.assertIn("nx_slot_ref_release with no matching", err)

    def test_zero_retains_and_zero_releases_passes(self):
        src = "/* no cap lifecycle at all */\n"
        rc, _, err = _run(str(self._mk(src)), "--rule", "R4")
        self.assertEqual(rc, 0, f"stderr: {err}")


class TestInvalidInputs(unittest.TestCase):

    def test_invalid_rule_name_exits_2(self):
        rc, out, err = _run("--rule", "R99")
        self.assertEqual(rc, 2)
        self.assertIn("unknown rule", err)

    def test_malformed_manifest_is_a_finding(self):
        tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(tmp,
                                                            ignore_errors=True))
        comp_dir = tmp / "components" / "example"
        comp_dir.mkdir(parents=True)
        (comp_dir / "manifest.json").write_text("{ not valid json")
        rc, _, err = _run(str(tmp / "components"))
        self.assertEqual(rc, 2)
        self.assertIn("invalid or unreadable manifest.json", err)


class TestSummaryOutput(unittest.TestCase):

    def test_summary_reports_ran_and_ai_verified(self):
        with tempfile.TemporaryDirectory() as d:
            rc, out, _ = _run(str(pathlib.Path(d) / "components"))
            self.assertEqual(rc, 0)
            # R2+R4+R9 ran as machine checks; others go to Layer-2 AI rubric.
            self.assertIn("ran R2,R4,R9", out)
            self.assertIn("ai-verified R1,R3,R5,R6,R7,R8", out)

    def test_filter_to_single_rule_changes_summary(self):
        with tempfile.TemporaryDirectory() as d:
            rc, out, _ = _run(str(pathlib.Path(d) / "components"),
                              "--rule", "R2")
            # When the user asks for R2 only, no other rule is ai-verified
            # — they just weren't requested. Summary should reflect that.
            self.assertIn("ran R2;", out)
            self.assertIn("ai-verified none", out)


class TestR9(unittest.TestCase):
    """->iface_ops must not appear outside framework/dispatcher.c."""

    def _mk_repo(self, framework_files=None, component_files=None):
        """Return components_dir inside a temp repo with framework/ sibling."""
        tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(
            lambda: __import__("shutil").rmtree(tmp, ignore_errors=True))
        comp_dir = tmp / "components"
        comp_dir.mkdir()
        if framework_files:
            fw_dir = tmp / "framework"
            fw_dir.mkdir()
            for name, src in framework_files.items():
                (fw_dir / name).write_text(src)
        if component_files:
            for comp_name, src in component_files.items():
                d = comp_dir / comp_name
                d.mkdir()
                (d / "manifest.json").write_text(
                    json.dumps({"name": comp_name, "version": "0.1.0"}))
                (d / f"{comp_name}.c").write_text(src)
        return comp_dir

    def test_no_iface_ops_passes(self):
        comp_dir = self._mk_repo(
            framework_files={"other.c": "/* nothing here */\n"})
        rc, _, err = _run(str(comp_dir), "--rule", "R9")
        self.assertEqual(rc, 0, f"stderr: {err}")

    def test_iface_ops_in_dispatcher_passes(self):
        src = "const void *x = slot->active->descriptor->iface_ops;\n"
        comp_dir = self._mk_repo(framework_files={"dispatcher.c": src})
        rc, _, err = _run(str(comp_dir), "--rule", "R9")
        self.assertEqual(rc, 0, f"dispatcher.c should be exempt: {err}")

    def test_iface_ops_in_bootstrap_passes(self):
        src = "const void *x = slot->active->descriptor->iface_ops;\n"
        comp_dir = self._mk_repo(framework_files={"bootstrap.c": src})
        rc, _, err = _run(str(comp_dir), "--rule", "R9")
        self.assertEqual(rc, 0, f"bootstrap.c should be exempt: {err}")

    def test_iface_ops_in_hosted_guard_passes(self):
        src = textwrap.dedent("""\
            #if __STDC_HOSTED__
            const void *x = slot->active->descriptor->iface_ops;
            #endif
        """)
        comp_dir = self._mk_repo(framework_files={"vfs_call.c": src})
        rc, _, err = _run(str(comp_dir), "--rule", "R9")
        self.assertEqual(rc, 0, f"#if __STDC_HOSTED__ should be exempt: {err}")

    def test_iface_ops_in_hosted_guard_else_fails(self):
        src = textwrap.dedent("""\
            #if __STDC_HOSTED__
            /* host fast path */
            #else
            const void *x = slot->active->descriptor->iface_ops;
            #endif
        """)
        comp_dir = self._mk_repo(framework_files={"syscall.c": src})
        rc, _, err = _run(str(comp_dir), "--rule", "R9")
        self.assertEqual(rc, 2)
        self.assertIn("R9", err)

    def test_iface_ops_in_block_comment_passes(self):
        src = " * e.g. slot->active->descriptor->iface_ops directly.\n"
        comp_dir = self._mk_repo(framework_files={"syscall.c": src})
        rc, _, err = _run(str(comp_dir), "--rule", "R9")
        self.assertEqual(rc, 0, f"block comment line should be exempt: {err}")

    def test_iface_ops_bare_in_framework_fails(self):
        src = "const void *x = slot->active->descriptor->iface_ops;\n"
        comp_dir = self._mk_repo(framework_files={"syscall.c": src})
        rc, _, err = _run(str(comp_dir), "--rule", "R9")
        self.assertEqual(rc, 2)
        self.assertIn("R9", err)
        self.assertIn("iface_ops", err)

    def test_iface_ops_bare_in_component_fails(self):
        src = "const void *x = slot->active->descriptor->iface_ops;\n"
        comp_dir = self._mk_repo(component_files={"mycomp": src})
        rc, _, err = _run(str(comp_dir), "--rule", "R9")
        self.assertEqual(rc, 2)
        self.assertIn("R9", err)

    def test_no_framework_dir_passes(self):
        comp_dir = self._mk_repo()
        rc, _, err = _run(str(comp_dir), "--rule", "R9")
        self.assertEqual(rc, 0, f"missing framework/ should not fail: {err}")


if __name__ == "__main__":
    unittest.main()
