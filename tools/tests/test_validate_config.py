"""Unit tests for tools/validate-config.py."""

import json
import pathlib
import tempfile
import unittest

try:
    import jsonschema  # noqa: F401 — presence check only
    HAVE_JSONSCHEMA = True
except ImportError:
    HAVE_JSONSCHEMA = False

from tools.tests._helpers import load_tool


@unittest.skipUnless(HAVE_JSONSCHEMA, "jsonschema not installed")
class TestVersionMatching(unittest.TestCase):

    def setUp(self):
        self.vc = load_tool("validate_config", "validate-config.py")

    def test_satisfies_greater_or_equal(self):
        self.assertTrue(self.vc.satisfies(">=0.1.0", "0.1.0"))
        self.assertTrue(self.vc.satisfies(">=0.1.0", "0.1.1"))
        self.assertTrue(self.vc.satisfies(">=0.1.0", "0.2.0"))
        self.assertTrue(self.vc.satisfies(">=0.1.0", "1.0.0"))
        self.assertFalse(self.vc.satisfies(">=0.2.0", "0.1.9"))
        self.assertFalse(self.vc.satisfies(">=1.0.0", "0.9.9"))

    def test_unknown_operator_rejected(self):
        with self.assertRaises(self.vc.ValidationError):
            self.vc.satisfies(">0.1.0", "0.1.0")
        with self.assertRaises(self.vc.ValidationError):
            self.vc.satisfies("^0.1.0", "0.1.0")
        with self.assertRaises(self.vc.ValidationError):
            self.vc.satisfies("0.1.0", "0.1.0")

    def test_bad_semver_rejected(self):
        with self.assertRaises(self.vc.ValidationError):
            self.vc.parse_semver("0.1")
        with self.assertRaises(self.vc.ValidationError):
            self.vc.parse_semver("v0.1.0")


@unittest.skipUnless(HAVE_JSONSCHEMA, "jsonschema not installed")
class TestCycleDetection(unittest.TestCase):

    def setUp(self):
        self.vc = load_tool("validate_config", "validate-config.py")

    def test_acyclic_returns_none(self):
        self.assertIsNone(self.vc.find_cycle(
            {"a": ["b"], "b": ["c"], "c": []}))

    def test_self_loop_detected(self):
        cycle = self.vc.find_cycle({"a": ["a"]})
        self.assertEqual(cycle, ["a", "a"])

    def test_two_node_cycle_detected(self):
        cycle = self.vc.find_cycle({"a": ["b"], "b": ["a"]})
        self.assertIsNotNone(cycle)
        self.assertEqual(cycle[0], cycle[-1])  # closed cycle
        self.assertEqual(set(cycle[:-1]), {"a", "b"})

    def test_three_node_cycle_detected(self):
        cycle = self.vc.find_cycle(
            {"a": ["b"], "b": ["c"], "c": ["a"]})
        self.assertIsNotNone(cycle)
        self.assertEqual(set(cycle[:-1]), {"a", "b", "c"})


@unittest.skipUnless(HAVE_JSONSCHEMA, "jsonschema not installed")
class TestEndToEnd(unittest.TestCase):
    """
    Drive the tool's main() over small on-disk fixture trees. Each test
    builds a fresh kernel.json + matching components/ layout in a temp
    dir so tests don't share state.
    """

    def setUp(self):
        self.vc = load_tool("validate_config", "validate-config.py")

    def _tree(self, tmp: pathlib.Path, kernel: dict,
              manifests: dict[str, dict]) -> tuple[pathlib.Path, pathlib.Path]:
        kpath = tmp / "kernel.json"
        kpath.write_text(json.dumps(kernel))
        components_dir = tmp / "components"
        components_dir.mkdir()
        for impl, manifest in manifests.items():
            comp_dir = components_dir / impl
            comp_dir.mkdir()
            (comp_dir / "manifest.json").write_text(json.dumps(manifest))
        return kpath, components_dir

    def test_valid_tree_exits_zero(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            kp, cd = self._tree(tmp,
                {"target": "t",
                 "components": {
                     "scheduler": {"impl": "sched_rr"},
                     "timer":     {"impl": "hpet"},
                 }},
                {"sched_rr": {
                    "name": "sched_rr", "version": "0.1.0",
                    "requires": {"timer": {"version": ">=0.1.0"}},
                 },
                 "hpet": {"name": "hpet", "version": "0.2.0"}})
            self.assertEqual(self.vc.main([str(kp), str(cd)]), 0)

    def test_missing_manifest_errors(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            kp, cd = self._tree(tmp,
                {"target": "t",
                 "components": {"scheduler": {"impl": "nope"}}},
                manifests={})
            self.assertEqual(self.vc.main([str(kp), str(cd)]), 2)

    def test_name_mismatch_errors(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            kp, cd = self._tree(tmp,
                {"target": "t",
                 "components": {"scheduler": {"impl": "sched_rr"}}},
                {"sched_rr": {
                    "name": "different_name", "version": "0.1.0",
                 }})
            self.assertEqual(self.vc.main([str(kp), str(cd)]), 2)

    def test_version_constraint_unsatisfied_errors(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            kp, cd = self._tree(tmp,
                {"target": "t",
                 "components": {
                     "scheduler": {"impl": "sched_rr"},
                     "timer":     {"impl": "hpet"},
                 }},
                {"sched_rr": {
                    "name": "sched_rr", "version": "0.1.0",
                    "requires": {"timer": {"version": ">=1.0.0"}},
                 },
                 "hpet": {"name": "hpet", "version": "0.2.0"}})
            self.assertEqual(self.vc.main([str(kp), str(cd)]), 2)

    def test_required_dep_slot_missing_from_kernel_errors(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            kp, cd = self._tree(tmp,
                {"target": "t",
                 "components": {"scheduler": {"impl": "sched_rr"}}},
                {"sched_rr": {
                    "name": "sched_rr", "version": "0.1.0",
                    "requires": {"timer": {}},
                 }})
            self.assertEqual(self.vc.main([str(kp), str(cd)]), 2)

    def test_optional_dep_slot_missing_is_ok(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            kp, cd = self._tree(tmp,
                {"target": "t",
                 "components": {"scheduler": {"impl": "sched_rr"}}},
                {"sched_rr": {
                    "name": "sched_rr", "version": "0.1.0",
                    "optional": {"stats": {}},
                 }})
            self.assertEqual(self.vc.main([str(kp), str(cd)]), 0)

    def test_dependency_cycle_errors(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            kp, cd = self._tree(tmp,
                {"target": "t",
                 "components": {
                     "a": {"impl": "a_impl"},
                     "b": {"impl": "b_impl"},
                 }},
                {"a_impl": {
                    "name": "a_impl", "version": "0.1.0",
                    "requires": {"b": {}},
                 },
                 "b_impl": {
                    "name": "b_impl", "version": "0.1.0",
                    "requires": {"a": {}},
                 }})
            self.assertEqual(self.vc.main([str(kp), str(cd)]), 2)

    def test_invalid_json_errors(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            kp = tmp / "kernel.json"
            kp.write_text("{ not valid json")
            cd = tmp / "components"
            cd.mkdir()
            self.assertEqual(self.vc.main([str(kp), str(cd)]), 2)

    def test_schema_violation_errors(self):
        with tempfile.TemporaryDirectory() as d:
            tmp = pathlib.Path(d)
            # `impl` in a slot binding must be a string.
            kp, cd = self._tree(tmp,
                {"target": "t",
                 "components": {"s": {"impl": 42}}},
                manifests={})
            self.assertEqual(self.vc.main([str(kp), str(cd)]), 2)


if __name__ == "__main__":
    unittest.main()
