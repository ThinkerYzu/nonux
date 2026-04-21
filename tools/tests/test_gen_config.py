"""Unit tests for tools/gen-config.py."""

import json
import pathlib
import tempfile
import unittest

from tools.tests._helpers import load_tool

gc = load_tool("gen_config", "gen-config.py")


class TestManifestMode(unittest.TestCase):

    def _write_and_run(self, manifest: dict) -> str:
        with tempfile.TemporaryDirectory() as d:
            dpath = pathlib.Path(d)
            mpath = dpath / "m.json"
            mpath.write_text(json.dumps(manifest))
            outdir = dpath / "out"
            rc = gc.main(["manifest", str(mpath), str(outdir)])
            self.assertEqual(rc, 0)
            return (outdir / f"{manifest['name']}_deps.h").read_text()

    def test_struct_field_per_dep(self):
        out = self._write_and_run({
            "name": "x",
            "version": "0.1.0",
            "requires": {"a": {}},
            "optional": {"b": {}},
        })
        self.assertIn("struct nx_slot *a;", out)
        self.assertIn("struct nx_slot *b;", out)

    def test_defaults_propagate_to_table(self):
        out = self._write_and_run({
            "name": "x", "version": "0.1.0",
            "requires": {"a": {}},
        })
        self.assertIn(".required = true", out)
        self.assertIn(".mode = NX_CONN_ASYNC", out)
        self.assertIn(".stateful = false", out)
        self.assertIn(".policy = NX_PAUSE_QUEUE", out)

    def test_explicit_overrides(self):
        out = self._write_and_run({
            "name": "x", "version": "0.1.0",
            "requires": {
                "a": {"mode": "sync", "stateful": True, "policy": "redirect"}
            },
        })
        self.assertIn(".mode = NX_CONN_SYNC", out)
        self.assertIn(".stateful = true", out)
        self.assertIn(".policy = NX_PAUSE_REDIRECT", out)

    def test_deterministic_output(self):
        manifest = {
            "name": "x", "version": "0.1.0",
            "requires": {"zebra": {}, "apple": {}, "mango": {}},
            "optional": {"banana": {}},
        }
        self.assertEqual(self._write_and_run(manifest),
                         self._write_and_run(manifest))

    def test_sorted_order_alphabetical_within_kind(self):
        out = self._write_and_run({
            "name": "x", "version": "0.1.0",
            "requires": {"zebra": {}, "apple": {}},
            "optional": {"yak": {}, "badger": {}},
        })
        # Requires first (sorted), then optionals (sorted).
        idx = [out.index(f"struct nx_slot *{n};")
               for n in ("apple", "zebra", "badger", "yak")]
        self.assertEqual(idx, sorted(idx))

    def test_no_deps_uses_placeholder_and_no_macro(self):
        out = self._write_and_run({"name": "trivial", "version": "0.1.0"})
        self.assertIn("_nx_no_deps", out)
        self.assertNotIn("DEPS_TABLE", out)

    def test_duplicate_across_requires_and_optional_errors(self):
        with tempfile.TemporaryDirectory() as d:
            dpath = pathlib.Path(d)
            m = dpath / "m.json"
            m.write_text(json.dumps({
                "name": "x", "version": "0.1.0",
                "requires": {"a": {}}, "optional": {"a": {}},
            }))
            rc = gc.main(["manifest", str(m), str(dpath / "out")])
            self.assertEqual(rc, 2)

    def test_invalid_mode_errors(self):
        with tempfile.TemporaryDirectory() as d:
            dpath = pathlib.Path(d)
            m = dpath / "m.json"
            m.write_text(json.dumps({
                "name": "x", "version": "0.1.0",
                "requires": {"a": {"mode": "bogus"}},
            }))
            rc = gc.main(["manifest", str(m), str(dpath / "out")])
            self.assertEqual(rc, 2)


class TestKernelMode(unittest.TestCase):

    def _run(self, kernel: dict) -> tuple[str, str]:
        with tempfile.TemporaryDirectory() as d:
            dpath = pathlib.Path(d)
            kpath = dpath / "kernel.json"
            kpath.write_text(json.dumps(kernel))
            comp_dir = dpath / "components"
            comp_dir.mkdir()
            outdir = dpath / "gen"
            rc = gc.main(["kernel", str(kpath), str(comp_dir), str(outdir)])
            self.assertEqual(rc, 0)
            return ((outdir / "config.h").read_text(),
                    (outdir / "sources.mk").read_text())

    def test_minimal_kernel_emits_target_and_one_slot(self):
        config_h, sources_mk = self._run({
            "target": "aarch64-qemu-virt",
            "components": {"scheduler": {"impl": "sched_rr"}},
        })
        self.assertIn('#define NX_TARGET "aarch64-qemu-virt"', config_h)
        self.assertIn('#define NX_SLOT_SCHEDULER_IMPL "sched_rr"', config_h)
        self.assertIn("components/sched_rr/sched_rr.c", sources_mk)

    def test_dotted_slot_becomes_underscore_macro(self):
        config_h, _ = self._run({
            "target": "t",
            "components": {"char_device.serial": {"impl": "uart_pl011"}},
        })
        self.assertIn('#define NX_SLOT_CHAR_DEVICE_SERIAL_IMPL "uart_pl011"',
                      config_h)

    def test_config_values_emit_correct_literal_form(self):
        config_h, _ = self._run({
            "target": "t",
            "components": {
                "scheduler": {
                    "impl": "sched_rr",
                    "config": {
                        "time_quantum_ms": 10,
                        "base_addr":      "0x09000000",
                        "root_fs":        "ramfs",
                        "enabled":        True,
                    },
                },
            },
        })
        self.assertIn("#define NX_CONFIG_SCHED_RR_TIME_QUANTUM_MS 10", config_h)
        # Numeric-looking string passed through as a literal number.
        self.assertIn("#define NX_CONFIG_SCHED_RR_BASE_ADDR 0x09000000",
                      config_h)
        # Non-numeric string is quoted.
        self.assertIn('#define NX_CONFIG_SCHED_RR_ROOT_FS "ramfs"', config_h)
        # Bool maps to 1/0.
        self.assertIn("#define NX_CONFIG_SCHED_RR_ENABLED 1", config_h)

    def test_duplicate_impls_dedup_in_sources_mk(self):
        _, sources_mk = self._run({
            "target": "t",
            "components": {
                "s1": {"impl": "sched_rr"},
                "s2": {"impl": "sched_rr"},
            },
        })
        self.assertEqual(
            sources_mk.count("components/sched_rr/sched_rr.c"), 1)

    def test_deterministic_output_across_runs(self):
        k = {
            "target": "t",
            "components": {
                "vfs":       {"impl": "vfs_simple"},
                "scheduler": {"impl": "sched_rr",
                              "config": {"quantum": 5, "max": 10}},
            },
        }
        self.assertEqual(self._run(k), self._run(k))

    def test_missing_target_errors(self):
        with tempfile.TemporaryDirectory() as d:
            dpath = pathlib.Path(d)
            kp = dpath / "k.json"
            kp.write_text(json.dumps(
                {"components": {"s": {"impl": "x"}}}))
            rc = gc.main(["kernel", str(kp), str(dpath), str(dpath / "out")])
            self.assertEqual(rc, 2)

    def test_empty_components_errors(self):
        with tempfile.TemporaryDirectory() as d:
            dpath = pathlib.Path(d)
            kp = dpath / "k.json"
            kp.write_text(json.dumps({"target": "t", "components": {}}))
            rc = gc.main(["kernel", str(kp), str(dpath), str(dpath / "out")])
            self.assertEqual(rc, 2)


if __name__ == "__main__":
    unittest.main()
