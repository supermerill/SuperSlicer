"""Offline Chocolatey publication and local packing tests; never installs or pushes."""

import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET
import zipfile


SCRIPTS = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("chocolatey_release", SCRIPTS / "chocolatey.py")
chocolatey = importlib.util.module_from_spec(SPEC)
with mock.patch.object(sys, "path", [str(SCRIPTS), *sys.path]):
    SPEC.loader.exec_module(chocolatey)
VERSION = "2.7.63.0"
PACKAGE_VERSION = "2.7.63"
NAME = f"SuperSlicer_{VERSION}_win64.msi"
URL = f"https://github.com/supermerill/SuperSlicer/releases/download/{VERSION}/{NAME}"
NAMESPACE = {"n": "http://schemas.microsoft.com/packaging/2015/06/nuspec.xsd"}


class ChocolateyTests(unittest.TestCase):
    def setUp(self):
        self.api = mock.Mock(spec=chocolatey.GitHub)
        self.api.repository = chocolatey.REPOSITORY
        self.item = {"id": 42, "tag_name": VERSION, "draft": False, "prerelease": False,
                     "published_at": "2026-11-13T00:00:00Z"}
        self.api.request.return_value = self.item
        self.bytes = b"MSI fixture, not a real installer"
        self.asset = {"name": NAME, "state": "uploaded", "size": len(self.bytes),
                      "browser_download_url": URL, "digest": "sha256:" + hashlib.sha256(self.bytes).hexdigest()}
        self.api.assets.return_value = {NAME: self.asset}
        self.exists = mock.Mock(return_value=False)
        approved_temp = Path(os.environ.get("LOCALAPPDATA", tempfile.gettempdir())) / "Temp/opencode"
        self.folder = tempfile.TemporaryDirectory(dir=approved_temp if approved_temp.is_dir() else None)
        self.root = Path(self.folder.name)
        self.stdout = io.StringIO()
        self.redirect = contextlib.redirect_stdout(self.stdout)
        self.redirect.__enter__()

    def tearDown(self):
        self.redirect.__exit__(None, None, None)
        self.folder.cleanup()

    def prepare(self, tag=VERSION):
        return chocolatey.prepare(self.api, tag, self.exists)

    def payload(self):
        metadata, _ = self.prepare()
        msi = self.root / NAME
        msi.write_bytes(self.bytes)
        destination = self.root / "package"
        chocolatey.stage(metadata, msi, destination)
        return metadata, msi, destination

    def test_chocolatey_normalizes_only_package_version_not_source_tag(self):
        for source, expected in ((VERSION, PACKAGE_VERSION), ("2.7.63.12", "2.7.63.12"),
                                 ("02.007.063.000", "2.7.63"), ("02.007.063.012", "2.7.63.12")):
            self.assertEqual(expected, chocolatey.package_version(source))
        metadata, outputs = self.prepare()
        self.assertEqual(VERSION, metadata["source_version"])
        self.assertEqual(URL, metadata["msi_url"])
        self.assertEqual(PACKAGE_VERSION, metadata["package_version"])
        self.assertEqual("superslicer.2.7.63.nupkg", outputs["package_file"])
        self.exists.assert_called_once_with(PACKAGE_VERSION)

    def test_invalid_prerelease_and_nightly_tags_fail_before_api(self):
        for tag in ("2.7.63.0-beta", "2.7.63.0+UNKNOWN", "2.7.63.0-2026-11-13", "nightly",
                    "v2.7.63.0", "2.7.63", "../2.7.63.0", "2.7.63.0\nsubmit=true"):
            with self.subTest(tag=tag), self.assertRaises(chocolatey.ReleaseError):
                self.prepare(tag)
        self.api.request.assert_not_called()

    def test_forks_drafts_prereleases_and_wrong_tags_are_rejected(self):
        self.api.repository = "example/SuperSlicer"
        with self.assertRaisesRegex(chocolatey.ReleaseError, "upstream"):
            self.prepare()
        self.api.repository = chocolatey.REPOSITORY
        original = copy.deepcopy(self.item)
        for field, value in (("draft", True), ("draft", None), ("prerelease", True), ("prerelease", None),
                             ("published_at", None), ("tag_name", "2.7.62.0")):
            self.api.request.return_value = {**original, field: value}
            with self.subTest(field=field, value=value), self.assertRaisesRegex(chocolatey.ReleaseError, "published, non-prerelease"):
                self.prepare()
        self.exists.assert_not_called()

    def test_missing_empty_incomplete_or_misnamed_msi_is_rejected(self):
        for assets in ({}, {"SuperSlicer_2.7.63.0.msi": self.asset},
                       {NAME: {**self.asset, "size": 0}}, {NAME: {**self.asset, "state": "starter"}}):
            self.api.assets.return_value = assets
            with self.subTest(assets=assets), self.assertRaisesRegex(chocolatey.ReleaseError, "Missing or incomplete"):
                self.prepare()

    def test_url_and_digest_are_validated_before_checking_feed(self):
        for changes in ({"browser_download_url": "https://example.com/installer.msi"},
                        {"browser_download_url": URL.replace(VERSION, "2.7.62.0")},
                        {"digest": "sha256:not-a-hash"}):
            self.api.assets.return_value = {NAME: {**self.asset, **changes}}
            with self.subTest(changes=changes), self.assertRaises(chocolatey.ReleaseError):
                self.prepare()
        self.exists.assert_not_called()

    def test_visible_existing_version_is_skipped_not_overwritten(self):
        self.exists.return_value = True
        _, outputs = self.prepare()
        self.assertEqual("false", outputs["submit"])
        self.assertIn("::warning::", self.stdout.getvalue())
        self.assertTrue(all(call.args[0] == "GET" for call in self.api.request.call_args_list))

    def test_rendered_package_has_pinned_url_hash_and_no_msi(self):
        metadata, msi, destination = self.payload()
        self.assertEqual({"superslicer.nuspec", "tools/chocolateyInstall.ps1", "tools/VERIFICATION.txt"},
                         {p.relative_to(destination).as_posix() for p in destination.rglob("*") if p.is_file()})
        nuspec = ET.parse(destination / "superslicer.nuspec")
        self.assertEqual("superslicer", nuspec.find("n:metadata/n:id", NAMESPACE).text)
        self.assertEqual(PACKAGE_VERSION, nuspec.find("n:metadata/n:version", NAMESPACE).text)
        script = (destination / "tools/chocolateyInstall.ps1").read_text()
        self.assertIn(URL, script)
        self.assertIn(chocolatey.sha256(msi).upper(), script)
        self.assertNotIn("ChocolateyPackageVersion", script)
        self.assertNotIn("@MSI_URL@", script)
        self.assertIn(VERSION, (destination / "tools/VERIFICATION.txt").read_text())
        self.assertEqual("https://cdn.jsdelivr.net/gh/supermerill/SuperSlicer@2.7.63.0/resources/icons/SuperSlicer.png",
                         nuspec.find("n:metadata/n:iconUrl", NAMESPACE).text)

    def test_tampered_size_hash_or_metadata_is_rejected(self):
        metadata, _ = self.prepare()
        msi = self.root / NAME
        msi.write_bytes(self.bytes + b"tamper")
        with self.assertRaisesRegex(chocolatey.ReleaseError, "size"):
            chocolatey.stage(metadata, msi, self.root / "package")
        msi.write_bytes(b"x" * len(self.bytes))
        with self.assertRaisesRegex(chocolatey.ReleaseError, "digest"):
            chocolatey.stage(metadata, msi, self.root / "package")
        msi.write_bytes(self.bytes)
        for changes in ({"msi_url": "https://example.com/installer.msi"}, {"package_version": VERSION},
                        {"msi_name": "../installer.msi"}, {"repository": "example/SuperSlicer"}):
            with self.subTest(changes=changes), self.assertRaises(chocolatey.ReleaseError):
                chocolatey.stage({**metadata, **changes}, msi, self.root / "package")

    def test_without_github_digest_locally_computed_checksum_is_still_pinned(self):
        self.asset["digest"] = None
        _, msi, destination = self.payload()
        self.assertIn(chocolatey.sha256(msi).upper(), (destination / "tools/chocolateyInstall.ps1").read_text())

    def test_nonempty_staging_directory_is_not_overwritten(self):
        metadata, msi, destination = self.payload()
        with self.assertRaisesRegex(chocolatey.ReleaseError, "not empty"):
            chocolatey.stage(metadata, msi, destination)

    def test_cli_writes_metadata_and_normalized_outputs_without_key(self):
        metadata, outputs = self.prepare()
        output = self.root / "outputs"
        metadata_path = self.root / "metadata.json"
        with mock.patch.object(chocolatey, "GitHub", return_value=self.api), \
                mock.patch.object(chocolatey, "prepare", return_value=(metadata, outputs)):
            chocolatey.main(["prepare", "--tag", VERSION, "--metadata", str(metadata_path), "--output", str(output)])
        self.assertEqual(metadata, json.loads(metadata_path.read_text()))
        self.assertIn("package_file=superslicer.2.7.63.nupkg\n", output.read_text())
        self.assertNotIn("CHOCO_API_KEY", metadata_path.read_text() + output.read_text())

    @unittest.skipUnless(os.name == "nt" and shutil.which("powershell"), "Requires Windows PowerShell")
    def test_install_script_with_mock_helpers_never_runs_msi(self):
        _, _, destination = self.payload()
        script = SCRIPTS.parent / "chocolatey/tests/Test-InstallScript.ps1"
        result = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script),
                                 "-PackageDirectory", str(destination), "-WorkDirectory", str(self.root / "mock-choco")],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    @unittest.skipUnless(os.name == "nt" and shutil.which("powershell"), "Requires Windows PowerShell")
    def test_packer_rejects_unsigned_download_before_rendering(self):
        metadata, _ = self.prepare()
        msi = self.root / NAME
        msi.write_bytes(self.bytes)
        metadata_path = self.root / "release.json"
        metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
        destination = self.root / "package"
        command = r"""
$ErrorActionPreference = 'Stop'
function Invoke-WebRequest {
    param([switch]$UseBasicParsing, $Uri, $OutFile)
    Copy-Item -LiteralPath $env:CHOCO_FIXTURE_MSI -Destination $OutFile
}
try {
    & $env:CHOCO_FIXTURE_PACKER -MetadataPath $env:CHOCO_FIXTURE_METADATA -WorkDirectory $env:CHOCO_FIXTURE_WORK -OutputDirectory $env:CHOCO_FIXTURE_OUTPUT
    throw 'Unexpected successful packing of an unsigned fixture'
} catch {
    if ($_.Exception.Message -notlike '*publicly trusted MSI signature*') { throw }
}
exit 0
"""
        environment = {**os.environ, "CHOCO_FIXTURE_MSI": str(msi), "CHOCO_FIXTURE_METADATA": str(metadata_path),
                       "CHOCO_FIXTURE_PACKER": str(SCRIPTS.parent / "chocolatey/Pack-Release.ps1"),
                       "CHOCO_FIXTURE_WORK": str(self.root / "download"), "CHOCO_FIXTURE_OUTPUT": str(destination)}
        result = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", command],
                                env=environment, capture_output=True, text=True, timeout=60)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertFalse(destination.exists())

    @unittest.skipUnless(os.name == "nt" and shutil.which("choco"), "Requires Chocolatey CLI for local packing only")
    def test_choco_pack_produces_normalized_script_only_nupkg(self):
        _, _, destination = self.payload()
        result = subprocess.run(["choco", "pack", str(destination / "superslicer.nuspec"), "--output-directory", str(destination),
                                 "--no-progress"], capture_output=True, text=True, timeout=60)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        with zipfile.ZipFile(destination / "superslicer.2.7.63.nupkg") as archive:
            names = set(archive.namelist())
            self.assertIn("tools/chocolateyInstall.ps1", names)
            self.assertIn("tools/VERIFICATION.txt", names)
            self.assertFalse(any(name.lower().endswith((".msi", ".exe", ".dll")) for name in names))
            self.assertFalse(any("tests/" in name or name.endswith("README.md") for name in names))


class FeedTests(unittest.TestCase):
    def response(self, content):
        response = mock.MagicMock()
        response.__enter__.return_value.read.return_value = content
        return response

    def test_exact_version_found_without_credentials(self):
        xml = b'<entry xmlns="http://www.w3.org/2005/Atom" xmlns:d="http://schemas.microsoft.com/ado/2007/08/dataservices"><title>superslicer</title><d:Version>2.7.63</d:Version></entry>'
        with mock.patch.object(chocolatey.urllib.request, "urlopen", return_value=self.response(xml)) as urlopen:
            self.assertTrue(chocolatey.package_exists(PACKAGE_VERSION))
        request = urlopen.call_args.args[0]
        self.assertEqual(f"{chocolatey.FEED}/Packages(Id='superslicer',Version='2.7.63')", request.full_url)
        self.assertIsNone(request.get_header("Authorization"))

    def test_feed_four_part_zero_is_equivalent_to_normalized_package(self):
        xml = b'<entry xmlns="http://www.w3.org/2005/Atom" xmlns:d="http://schemas.microsoft.com/ado/2007/08/dataservices"><title>superslicer</title><d:Version>2.7.63.0</d:Version></entry>'
        with mock.patch.object(chocolatey.urllib.request, "urlopen", return_value=self.response(xml)):
            self.assertTrue(chocolatey.package_exists(PACKAGE_VERSION))

    def test_only_404_means_absent(self):
        for code in (404, 403, 429, 500):
            error = chocolatey.urllib.error.HTTPError("https://example.com", code, "test reason", {}, io.BytesIO())
            try:
                with mock.patch.object(chocolatey.urllib.request, "urlopen", side_effect=error):
                    if code == 404:
                        self.assertFalse(chocolatey.package_exists(PACKAGE_VERSION))
                    else:
                        with self.assertRaises(chocolatey.ReleaseError):
                            chocolatey.package_exists(PACKAGE_VERSION)
            finally:
                error.close()

    def test_network_or_unexpected_response_does_not_mean_absent(self):
        with mock.patch.object(chocolatey.urllib.request, "urlopen", side_effect=chocolatey.urllib.error.URLError("private reason")):
            with self.assertRaises(chocolatey.ReleaseError):
                chocolatey.package_exists(PACKAGE_VERSION)
        for content in (b"not XML", b"<html/>"):
            with mock.patch.object(chocolatey.urllib.request, "urlopen", return_value=self.response(content)):
                with self.assertRaises(chocolatey.ReleaseError):
                    chocolatey.package_exists(PACKAGE_VERSION)


try:
    import yaml
except ImportError:
    yaml = None


@unittest.skipIf(yaml is None, "Install PyYAML for workflow contract tests")
class ChocolateyWorkflowTests(unittest.TestCase):
    def setUp(self):
        self.workflow = yaml.load((SCRIPTS.parent / "workflows/release_chocolatey.yml").read_text(), Loader=yaml.BaseLoader)
        self.job = self.workflow["jobs"]["publish"]

    def test_stable_only_opt_in_and_manual_default_branch(self):
        self.assertEqual({"release", "workflow_dispatch"}, set(self.workflow["on"]))
        self.assertEqual(["released"], self.workflow["on"]["release"]["types"])
        self.assertIn("vars.CHOCO_ENABLED == 'true'", self.job["if"])
        self.assertIn("github.repository == 'supermerill/SuperSlicer'", self.job["if"])
        self.assertIn("!github.event.release.draft && !github.event.release.prerelease", self.job["if"])
        self.assertIn("github.event.repository.default_branch", self.job["if"])
        self.assertEqual("false", self.workflow["concurrency"]["cancel-in-progress"])

    def test_checkout_and_credentials_are_restricted(self):
        self.assertEqual({"contents": "read"}, self.workflow["permissions"])
        checkout = self.job["steps"][0]
        self.assertEqual("${{ github.sha }}", checkout["with"]["ref"])
        self.assertEqual("false", checkout["with"]["persist-credentials"])
        secret_steps = [step for step in self.job["steps"] if "CHOCO_API_KEY" in step.get("env", {})]
        self.assertEqual(1, len(secret_steps))
        self.assertIn("choco push", secret_steps[0]["run"])
        self.assertIn("https://push.chocolatey.org/", secret_steps[0]["run"])
        for step in self.job["steps"]:
            self.assertNotIn("${{", step.get("run", ""))
            self.assertNotRegex(step.get("run", ""), r"--force|--debug|--verbose|choco apikey")

    def test_signature_and_msi_metadata_checked_before_render_and_pack(self):
        script = (SCRIPTS.parent / "chocolatey/Pack-Release.ps1").read_text()
        self.assertIn("$signature.Status -ne 'Valid'", script)
        self.assertIn("$installer.OpenDatabase($msi, 0)", script)
        self.assertIn("$properties['ProductVersion']", script)
        self.assertLess(script.index("Get-AuthenticodeSignature"), script.index("$renderer stage"))
        self.assertLess(script.index("$properties['ProductVersion']"), script.index("$renderer stage"))
        self.assertLess(script.index("$renderer stage"), script.index("choco pack"))
        names = [step.get("name") for step in self.job["steps"]]
        self.assertLess(names.index("Verify signed MSI and pack the script-only package"), names.index("Submit to Chocolatey Community Repository"))


if __name__ == "__main__":
    unittest.main()
