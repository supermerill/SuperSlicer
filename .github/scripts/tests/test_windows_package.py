"""Offline tests for the Windows build-to-package artifact contract."""

import contextlib
from datetime import date
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import unittest
from unittest import mock

SCRIPTS = Path(__file__).resolve().parents[1]
ROOT = SCRIPTS.parent.parent
sys.path.insert(0, str(SCRIPTS))
SPEC = importlib.util.spec_from_file_location("windows_package", SCRIPTS / "windows_package.py")
package = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(package)
SHA = "a" * 40
DAY = date(2026, 11, 20)
VERSION = (ROOT / "version.inc").read_text(encoding="utf-8-sig")


class PackageContractTests(unittest.TestCase):
    def setUp(self):
        approved_temp = Path(os.environ.get("LOCALAPPDATA", tempfile.gettempdir())) / "Temp/opencode"
        self.folder = tempfile.TemporaryDirectory(dir=approved_temp if approved_temp.is_dir() else None)
        self.root = Path(self.folder.name)
        self.version = self.root / "version.inc"
        self.version.write_text(VERSION.replace('"2.7.63.0"', '"2.7.63.12"').replace('"2,7,63,0"', '"2,7,63,12"'),
                                encoding="utf-8")
        self.build = self.root / "build"
        files = {"superslicer.exe": b"launcher", "Slic3r.dll": b"core", "mesa/opengl32.dll": b"mesa",
                 "resources/profiles/Vendor.ini": b"[vendor]"}
        for name, content in files.items():
            path = self.build / "package" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        self.head = mock.patch.object(package, "head_sha", return_value=SHA)
        self.head.start()

    def tearDown(self):
        self.head.stop()
        self.folder.cleanup()

    def manifest(self, mode="rc", day=None):
        return package.write_manifest(self.build, self.version, mode, SHA, day)

    def edit(self, change):
        path = self.build / package.MANIFEST
        data = json.loads(path.read_text(encoding="utf-8"))
        change(data)
        path.write_text(json.dumps(data), encoding="utf-8")

    def test_rc_identity_has_no_date_and_keeps_numeric_version(self):
        manifest = self.manifest()
        self.assertEqual({"schema": 1, "source_sha": SHA, "mode": "rc", "configuration": "Release",
                          "product": "SuperSlicer", "product_version": "2.7.63.12",
                          "binary_version": "SuperSlicer_2.7.63.12-beta", "nightly_date": None,
                          "msi_build_name": "SuperSlicer_2.7.63.12.msi", "installer_display_suffix": ""},
                         {key: value for key, value in manifest.items() if key != "files"})
        self.assertEqual(["Slic3r.dll", "mesa/opengl32.dll", "resources/profiles/Vendor.ini", "superslicer.exe"],
                         [entry["path"] for entry in manifest["files"]])
        self.assertEqual(package.identity(self.version, "rc"), package.verify(self.build, self.version, "rc", SHA))

    def test_nightly_identity_reuses_the_build_date(self):
        manifest = self.manifest("nightly", DAY)
        self.assertEqual("RelWithDebInfo", manifest["configuration"])
        self.assertEqual("2026-11-20", manifest["nightly_date"])
        self.assertEqual("SuperSlicer_2.7.63.12-beta-2026-11-20.msi", manifest["msi_build_name"])
        self.assertEqual("SuperSlicer_2.7.63.12-beta-2026-11-20", manifest["binary_version"])
        self.assertEqual("-Beta-Nightly", manifest["installer_display_suffix"])
        # Packaging on another day keeps the recorded date.
        self.assertEqual("2026-11-20", package.verify(self.build, self.version, "nightly", SHA)["nightly_date"])

    def test_date_presence_must_match_the_mode(self):
        with self.assertRaisesRegex(package.PackageError, "build date"):
            package.identity(self.version, "rc", DAY)
        with self.assertRaisesRegex(package.PackageError, "build date"):
            package.identity(self.version, "nightly")
        with self.assertRaisesRegex(package.PackageError, "Unknown"):
            package.identity(self.version, "debug")

    def test_checkout_and_sha_must_be_the_requested_commit(self):
        with self.assertRaisesRegex(package.PackageError, "lowercase"):
            package.write_manifest(self.build, self.version, "rc", SHA.upper())
        with mock.patch.object(package, "head_sha", return_value="b" * 40), \
                self.assertRaisesRegex(package.PackageError, "checked-out"):
            self.manifest()
        self.manifest()
        with self.assertRaisesRegex(package.PackageError, "another commit"):
            with mock.patch.object(package, "head_sha", return_value="b" * 40):
                package.verify(self.build, self.version, "rc", "b" * 40)

    def test_mode_or_commit_identity_changes_are_rejected(self):
        self.manifest()
        with self.assertRaisesRegex(package.PackageError, "build date"):
            package.verify(self.build, self.version, "nightly", SHA)
        for key, value in (("msi_build_name", "SuperSlicer_2.7.63.0.msi"), ("binary_version", "SuperSlicer_2.7.63.12"),
                           ("configuration", "RelWithDebInfo"), ("installer_display_suffix", "-Beta-Nightly")):
            with self.subTest(key=key):
                self.manifest()
                self.edit(lambda data: data.__setitem__(key, value))
                with self.assertRaisesRegex(package.PackageError, key):
                    package.verify(self.build, self.version, "rc", SHA)

    def test_release_product_and_version_must_match_version_inc(self):
        self.manifest()
        package.verify(self.build, self.version, "rc", SHA, "SuperSlicer", "2.7.63.12-beta")
        with self.assertRaisesRegex(package.PackageError, "product"):
            package.verify(self.build, self.version, "rc", SHA, "Other", "2.7.63.12-beta")
        with self.assertRaisesRegex(package.PackageError, "release version"):
            package.verify(self.build, self.version, "rc", SHA, "SuperSlicer", "2.7.63.12")

    def test_modified_missing_and_extra_files_are_rejected(self):
        changes = {
            "modified": lambda: (self.build / "package/Slic3r.dll").write_bytes(b"other"),
            "missing": lambda: (self.build / "package/mesa/opengl32.dll").unlink(),
            "extra": lambda: (self.build / "package/extra.dll").write_bytes(b"extra"),
        }
        for name, change in changes.items():
            with self.subTest(change=name):
                self.manifest()
                change()
                with self.assertRaises(package.PackageError):
                    package.verify(self.build, self.version, "rc", SHA)
                (self.build / "package/Slic3r.dll").write_bytes(b"core")
                (self.build / "package/mesa/opengl32.dll").write_bytes(b"mesa")
                (self.build / "package/extra.dll").unlink(missing_ok=True)

    def test_artifact_root_must_contain_only_package_and_manifest(self):
        self.manifest()
        (self.build / "winInstaller.wxs").write_text("<Wix/>", encoding="utf-8")
        with self.assertRaisesRegex(package.PackageError, "Unexpected or missing files"):
            package.verify(self.build, self.version, "rc", SHA)
        (self.build / "winInstaller.wxs").unlink()
        (self.build / package.MANIFEST).unlink()
        with self.assertRaisesRegex(package.PackageError, "Unexpected or missing files"):
            package.verify(self.build, self.version, "rc", SHA)

    def test_unsafe_or_duplicate_manifest_paths_are_rejected(self):
        for path in ("../Slic3r.dll", "/Slic3r.dll", "mesa\\opengl32.dll", "C:/Slic3r.dll", "mesa//opengl32.dll",
                     "./Slic3r.dll", "mesa/", "", None):
            with self.subTest(path=path):
                self.manifest()
                self.edit(lambda data: data["files"][0].__setitem__("path", path))
                with self.assertRaisesRegex(package.PackageError, "Invalid file entry"):
                    package.verify(self.build, self.version, "rc", SHA)
        self.manifest()
        self.edit(lambda data: data["files"].append(dict(data["files"][0], path=data["files"][0]["path"].upper())))
        with self.assertRaisesRegex(package.PackageError, "Duplicate"):
            package.verify(self.build, self.version, "rc", SHA)

    def test_manifest_format_is_strict(self):
        for change in (lambda data: data.pop("files"), lambda data: data.__setitem__("schema", 2),
                       lambda data: data.__setitem__("schema", True), lambda data: data.__setitem__("extra", 1),
                       lambda data: data["files"][0].pop("sha256")):
            with self.subTest(change=change):
                self.manifest()
                self.edit(change)
                with self.assertRaises(package.PackageError):
                    package.verify(self.build, self.version, "rc", SHA)
        self.manifest()
        (self.build / package.MANIFEST).write_text("{", encoding="utf-8")
        with self.assertRaisesRegex(package.PackageError, "Unreadable"):
            package.verify(self.build, self.version, "rc", SHA)

    def test_empty_package_is_rejected(self):
        for path in sorted((self.build / "package").rglob("*"), reverse=True):
            path.unlink() if path.is_file() else path.rmdir()
        with self.assertRaisesRegex(package.PackageError, "empty"):
            self.manifest()

    def test_cli_verify_writes_installer_values_for_wix(self):
        self.manifest("nightly", DAY)
        environment = self.root / "github-env"
        arguments = ["verify", "--build-directory", str(self.build), "--version-file", str(self.version),
                     "--mode", "nightly", "--source-sha", SHA]
        with mock.patch.dict(os.environ, {"GITHUB_ENV": str(environment)}), contextlib.redirect_stdout(io.StringIO()):
            package.main(arguments)
        self.assertEqual(["InstallerVersion=2.7.63.12", "ReleaseSuffix=-Beta-Nightly",
                          "MSI_FILE_NAME=SuperSlicer_2.7.63.12-beta-2026-11-20.msi",
                          "BINARY_VERSION=SuperSlicer_2.7.63.12-beta-2026-11-20"],
                         environment.read_text(encoding="utf-8").splitlines())


class InstallerSourceTests(unittest.TestCase):
    def setUp(self):
        self.template = (ROOT / "src/platform/msw/winInstaller.wxs.in").read_text(encoding="utf-8")

    def test_template_is_rendered_from_the_five_identity_values(self):
        rendered = package.render_wxs(self.template, VERSION)
        self.assertNotRegex(rendered, r"@[A-Za-z0-9_]+@")
        self.assertIn('UpgradeCode="a81705b0-978a-4a7d-bf67-8671e631dd4c"', rendered)
        self.assertIn('Name="SuperSlicer$(var.ReleaseSuffix)"', rendered)
        self.assertIn('Target="[INSTALLFOLDER]superslicer.exe"', rendered)
        self.assertIn('Id="superslicer.1Icon.exe"', rendered)
        self.assertIn('Version="$(var.InstallerVersion)"', rendered)
        self.assertIn(r'SourceFile="$(var.SourceDir)\superslicer.exe"', rendered)

    def test_unknown_placeholders_and_unexpected_values_are_rejected(self):
        with self.assertRaisesRegex(package.PackageError, "SLIC3R_VERSION"):
            package.render_wxs(self.template + "<!-- @SLIC3R_VERSION@ -->", VERSION)
        for name, value in (("SLIC3R_APP_KEY", 'Super"Slicer'), ("SLIC3R_GUID", "not-a-guid"),
                            ("SLIC3R_APP_CMD", "super slicer")):
            with self.subTest(name=name):
                content = re.sub(rf'set\({name} "[^"]*"\)', f'set({name} "{value}")', VERSION)
                with self.assertRaisesRegex(package.PackageError, name):
                    package.render_wxs(self.template, content)
        with self.assertRaisesRegex(package.PackageError, "exactly one"):
            package.render_wxs(self.template, VERSION + '\nset(SLIC3R_GUID "a81705b0-978a-4a7d-bf67-8671e631dd4c")\n')


try:
    import yaml
except ImportError:
    yaml = None


@unittest.skipIf(yaml is None, "Install PyYAML to validate workflow contracts")
class WindowsWorkerWorkflowTests(unittest.TestCase):
    def workflow(self, name):
        return yaml.load((ROOT / ".github/workflows" / name).read_text(encoding="utf-8"), Loader=yaml.BaseLoader)

    def test_build_worker_compiles_once_without_secrets_wix_or_signing(self):
        workflow = self.workflow("windows_build.yml")
        self.assertEqual({"workflow_call"}, set(workflow["on"]))
        self.assertNotIn("secrets", workflow["on"]["workflow_call"])
        self.assertEqual({"contents": "read"}, workflow["permissions"])
        jobs = json.dumps(workflow["jobs"]).lower()
        for forbidden in ("secrets.", "signpath", "heat.exe", "candle.exe", "light.exe", "prepare-binaries"):
            self.assertNotIn(forbidden, jobs)
        self.assertEqual("${{ inputs.mode == 'rc' && 'Release' || 'RelWithDebInfo' }}",
                         workflow["jobs"]["build"]["env"]["CONFIGURATION"])
        steps = workflow["jobs"]["build"]["steps"]
        names = [step.get("name") for step in steps]
        self.assertLess(names.index("copy required VC runtimes"), names.index("Record the unsigned package"))
        upload = steps[names.index("Upload the unsigned package for packaging")]
        self.assertEqual("build/package/\nbuild/windows-build.json\n", upload["with"]["path"])
        self.assertEqual("7", upload["with"]["retention-days"])
        self.assertEqual("${{ steps.upload.outputs.artifact-id }}", workflow["jobs"]["build"]["outputs"]["artifact_id"])
        for job in workflow["jobs"].values():
            checkout = next(step for step in job["steps"] if step.get("uses", "").startswith("actions/checkout@"))
            self.assertEqual("${{ inputs.source_sha }}", checkout["with"]["ref"])
            self.assertEqual("false", checkout["with"]["persist-credentials"])

    def test_package_worker_uses_the_exact_artifact_and_gates_every_signpath_step(self):
        workflow = self.workflow("windows_package.yml")
        self.assertEqual({"workflow_call"}, set(workflow["on"]))
        self.assertEqual("none", workflow["on"]["workflow_call"]["inputs"]["signing"]["default"])
        job = workflow["jobs"]["package"]
        self.assertEqual({"contents": "read", "actions": "read"}, job["permissions"])
        steps = job["steps"]
        names = [step.get("name") for step in steps]
        download = steps[names.index("Download the unsigned package from this run")]
        self.assertEqual("${{ inputs.artifact_id }}", download["with"]["artifact-ids"])
        self.assertNotIn("run-id", download["with"])
        self.assertNotIn("pattern", download["with"])
        for step in steps:
            uses_secret = "secrets." in json.dumps(step)
            if "signpath" in step.get("uses", "").lower() or uses_secret or step.get("name", "").startswith("upload unsigned"):
                self.assertEqual("inputs.signing != 'none'", step.get("if"), step.get("name"))
        order = ["Verify the package against this commit", "collect own binaries to sign",
                 "Check the binary version recorded at build time", "sign binaries",
                 "verify and copy signed binaries into package", "Upload artifact", "Prepare license file for MSI",
                 "Render the installer source", "Harvest files for MSI", "Build MSI installer", "sign msi",
                 "verify and replace signed msi", "Upload MSI artifact",
                 "Stage signed release files and integrity manifest", "Upload exact files for the draft publisher"]
        self.assertEqual(order, [name for name in names if name in order])
        build = steps[names.index("Build MSI installer")]["run"]
        self.assertIn("winInstaller.wxs files.wxs", build)
        self.assertNotIn(r"src\winInstaller.wxs", build)
        self.assertIn("--version=3.14.1", steps[names.index("Install WiX Toolset 3.14.1")]["run"])
        for name in ("Stage signed release files and integrity manifest", "Upload exact files for the draft publisher"):
            self.assertEqual("inputs.stage_release", steps[names.index(name)]["if"])
        self.assertEqual("${{ inputs.signing == 'test' && vars.SIGNPATH_TEST_CERTIFICATE_THUMBPRINT || '' }}",
                         job["env"]["SIGNPATH_TEST_CERTIFICATE_THUMBPRINT"])

    def test_package_worker_keeps_existing_artifact_names(self):
        prefix = self.workflow("windows_package.yml")["jobs"]["package"]["env"]["ARTIFACT_PREFIX"][3:-2]
        expression = prefix.replace("&&", " and ").replace("||", " or ")
        for mode, signing, expected in (("rc", "none", "rc"), ("rc", "production", "rc"),
                                        ("nightly", "none", "nightly"), ("nightly", "test", "test-signed_nightly")):
            inputs = type("Inputs", (), {"mode": mode, "signing": signing})
            self.assertEqual(expected, eval(expression, {"__builtins__": {}}, {"inputs": inputs}))


if __name__ == "__main__":
    unittest.main()
