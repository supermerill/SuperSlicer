"""Offline ABI packaging checks, independent of the platform DLL loader."""
import importlib.util
from pathlib import Path
import sys
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "src/plugins_cpp"))
import write_plugin_abi_manifest as manifest


class AbiPackagingTests(unittest.TestCase):
    def test_union_and_failed_generation_never_publish_partial_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary, base, abi, output = (root / name for name in ("plugin.dll", "base.ini", "abi.ini", "version.ini"))
            binary.write_bytes(manifest.BEGIN + b"slic3r_plugin_types.h=1.0\nslic3r_extrusion_entity.h=0.0\n" + manifest.END)
            base.write_text("[plugin]\npackage_version=1.0.0\nslicer_version=999.0.0\n", encoding="utf-8")
            abi.write_text("[abi]\nslic3r_extrusion_entity.h=1.0\n", encoding="utf-8")
            command = [sys.executable, manifest.__file__, "--base", str(base), "--binary", str(binary),
                       "--abi", str(abi), "--output", str(output)]
            subprocess.run(command, check=True, capture_output=True)
            self.assertEqual(manifest.read_ini_abi(output), {
                "slic3r_plugin_types.h": (1, 0), "slic3r_extrusion_entity.h": (1, 0)})
            previous = output.read_bytes()
            binary.write_bytes(b"missing metadata")
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)
            self.assertEqual(output.read_bytes(), previous)

    def test_record_validation(self):
        good = manifest.BEGIN + b"slic3r_plugin_types.h=1u.0u\n" + manifest.END
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "plugin.dll"
            binary.write_bytes(b"not an executable" + good)
            self.assertEqual(manifest.read_binary_abi(binary), {"slic3r_plugin_types.h": (1, 0)})
            for bad in (b"", good + good, good[:-1], manifest.BEGIN + b"a.h=1.0\na.h=1.0\n" + manifest.END):
                binary.write_bytes(bad)
                with self.assertRaises(ValueError):
                    manifest.read_binary_abi(binary)

    def test_python_header_coverage(self):
        generator = ROOT / "src/libslic3r/Api/plugin/python/generate_slic3r_api.py"
        spec = importlib.util.spec_from_file_location("abi_generator", generator)
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "abi.ini"
            output.write_text(module.generate_abi_manifest(), encoding="utf-8")
            versions = manifest.read_ini_abi(output)
            self.assertIn("slic3r_plugin_types.h", versions)
            for header in module.header_paths():
                if re.search(r"^#define SLIC3R_PLUGIN_API_\w+_MAJOR\s+\d+", header.read_text(encoding="utf-8"), re.MULTILINE):
                    self.assertIn(header.relative_to(module.HEADER_ROOT).as_posix(), versions)


if __name__ == "__main__":
    unittest.main()
