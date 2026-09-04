"""Check the optional changelog validator and publication without loading DLLs."""
from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "src/plugins_cpp"))
import package_plugin_changelog as changelog


class ChangelogPackagingTests(unittest.TestCase):
    def test_native_and_python_cmake_packages_refresh_changelog_only_changes(self):
        # A tiny standalone project exercises the production packaging helpers
        # without linking or rebuilding the slicer. All generated files are temporary.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "native.cpp").write_text(
                '#define SLIC3R_PLUGIN_EXPORTS\n'
                '#include "libslic3r/Api/plugin/c/slic3r_plugin.h"\n'
                '#include "libslic3r/Api/plugin/c/slic3r_plugin_register_version.h"\n', encoding="utf-8")
            (root / "plugin.py").write_text("# Packaging fixture\n", encoding="utf-8")
            (root / "default.ini").write_text("[installed]\n", encoding="utf-8")
            (root / "abi.ini").write_text("[abi]\nslic3r_plugin_types.h=1.0\n", encoding="utf-8")
            (root / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.20)
project(ChangelogPackaging LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(SLIC3R_VERSION_FULL "99.0.0")
set(SLIC3R_RC_VERSION_DOTS "99.0.0")
set(SLIC3R_BUILD_RESOURCES_DIR "${{CMAKE_BINARY_DIR}}/resources")
set(SLIC3R_GENERATED_DEFAULT_ACTIVATED_FILE "${{CMAKE_SOURCE_DIR}}/default.ini")
set(SLIC3R_PYTHON_ABI_MANIFEST "${{CMAKE_SOURCE_DIR}}/abi.ini")
add_custom_target(Slic3r)
add_custom_target(slic3r_python_api_generated)
include("{ROOT.as_posix()}/src/plugins_cpp/PluginPackage.cmake")
add_library(native SHARED native.cpp)
target_include_directories(native PRIVATE "{ROOT.as_posix()}/src")
slic3r_package_plugin(native native VERSION 1.0.0 CHANGELOG notes.json)
slic3r_package_python_plugin(script script plugin.py VERSION 1.0.0 CHANGELOG notes.json)
''', encoding="utf-8")
            env = {("Path" if key.upper() == "PATH" and os.name == "nt" else key): value
                   for key, value in os.environ.items()}
            env["MSBUILDDISABLENODEREUSE"] = "1"
            build = ["cmake", "--build", str(root / "build"), "--target", "native", "script", "--config", "Debug"]
            if os.name == "nt":
                build += ["--", "/m:8", "/nr:false", "/v:minimal"]
            for index, message in enumerate(("Initial notes", "Updated without a code change")):
                contents = json.dumps({"format_version": 1, "versions": {"1.0.0": [message]}}).encode()
                (root / "notes.json").write_bytes(contents)
                if index == 0:
                    configured = subprocess.run(["cmake", "-S", str(root), "-B", str(root / "build")],
                                                env=env, capture_output=True, text=True, timeout=900)
                    self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
                compiled = subprocess.run(build, env=env, capture_output=True, text=True, timeout=1800)
                self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
                for package in ("native", "script"):
                    with zipfile.ZipFile(root / "build/resources/plugins" / f"{package}_1.0.0_99.0.0.zip") as archive:
                        self.assertEqual(archive.read("changelog.json"), contents)
            (root / "notes.json").write_text("invalid", encoding="utf-8")
            for target in ("native", "script"):
                failed = subprocess.run(["cmake", "--build", str(root / "build"), "--target", target,
                                         "--config", "Debug"], env=env, capture_output=True, text=True, timeout=1800)
                self.assertNotEqual(failed.returncode, 0)
                with zipfile.ZipFile(root / "build/resources/plugins" / f"{target}_1.0.0_99.0.0.zip") as archive:
                    self.assertEqual(archive.read("changelog.json"), contents)

    def test_schema_and_duplicates(self):
        good = b'{"format_version":1,"versions":{"1.0.0":[],"2.7.63.1":["Notes"]}}'
        self.assertEqual(changelog.validate(good)["versions"]["1.0.0"], [])
        for bad in (
            b'{"format_version":2,"versions":{}}',
            b'{"format_version":1,"format_version":1,"versions":{}}',
            b'{"format_version":1,"versions":{"1.0.0":[],"1.0.0":[]}}',
            b'{"format_version":1,"versions":{"bad":[]}}',
            b'{"format_version":1,"versions":{"1.0.0":[12]}}',
            b'{"format_version":1,"versions":{"1.0.0":["\\ud800"]}}',
            b' ' * (changelog.MAX_SIZE + 1),
        ):
            with self.subTest(bad=bad[:100]), self.assertRaises((ValueError, UnicodeError)):
                changelog.validate(bad)

    def test_copy_update_and_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            source, output = (Path(directory) / name for name in ("source.json", "changelog.json"))
            command = [sys.executable, changelog.__file__, "--source", str(source), "--output", str(output)]
            for message in ("Initial", "Updated"):
                contents = json.dumps({"format_version": 1, "versions": {"1.0.0": [message]}}).encode()
                source.write_bytes(contents)
                subprocess.run(command, check=True, capture_output=True)
                self.assertEqual(output.read_bytes(), contents)
            source.write_text("invalid", encoding="utf-8")
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(str(source), result.stderr)
            self.assertEqual(output.read_bytes(), contents)

    def test_all_ten_release_files(self):
        native = list((ROOT / "src/plugins_cpp").glob("*/changelog.json"))
        python = list((ROOT / "src/plugins_python").rglob("*.changelog.json"))
        self.assertEqual(len(native), 4)
        self.assertEqual(len(python), 6)
        for path in native + python:
            self.assertTrue(changelog.validate(path.read_bytes())["versions"])


if __name__ == "__main__":
    unittest.main()
