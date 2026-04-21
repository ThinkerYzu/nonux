"""Test helpers — path setup so tools/*.py can be imported by test modules."""

import importlib
import importlib.util
import pathlib
import sys

TOOLS_DIR = pathlib.Path(__file__).resolve().parents[1]
ROOT      = TOOLS_DIR.parent


def load_tool(module_name: str, filename: str):
    """
    Load a tools/*.py file as an importable module.
    `gen-config.py` and `validate-config.py` have hyphens in their
    filenames, which aren't legal Python module names — importlib's
    spec_from_file_location sidesteps that.

    The module is registered in sys.modules before exec so that
    stdlib machinery that looks up the module by name (notably
    `@dataclass`, which calls `sys.modules.get(cls.__module__)`)
    can find it.
    """
    path = TOOLS_DIR / filename
    spec = importlib.util.spec_from_file_location(module_name, path)
    if spec is None or spec.loader is None:
        raise ImportError(f"could not load {path}")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = mod
    spec.loader.exec_module(mod)
    return mod
