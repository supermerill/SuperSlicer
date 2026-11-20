"""Offline tests for the Windows nightly version and single MSI filename contract."""

import argparse
import contextlib
from datetime import date
import fnmatch
import importlib.util
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET


SCRIPTS = Path(__file__).resolve().parents[1]
ROOT = SCRIPTS.parent.parent
SPEC = importlib.util.spec_from_file_location("nightly_version", SCRIPTS / "nightly_version.py")
nightly = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(nightly)
DAY = date(2026, 11, 14)


def fixture(version="2.7.63.0", tag="-beta+UNKNOWN", product="SuperSlicer"):
    return (f'set(SLIC3R_APP_KEY "{product}")\n'
            f'set(SLIC3R_RC_VERSION_DOTS "{version}")\n'
            f'set(SLIC3R_RC_VERSION "{version.replace(".", ",")}")\n'
            f'set(SLIC3R_VERSION_TAG "{tag}")\n'
            'set(SLIC3R_BUILD_ID "${SLIC3R_APP_KEY}_${SLIC3R_RC_VERSION_DOTS}${SLIC3R_VERSION_TAG}")\n')


class NightlyVersionTests(unittest.TestCase):
    def test_numeric_version_preserved_and_marker_replaced(self):
        content, values = nightly.prepare(fixture(), DAY)
        self.assertEqual("2.7.63.0", values["InstallerVersion"])
        self.assertEqual("SuperSlicer_2.7.63.0-beta-2026-11-14.msi", values["MSI_FILE_NAME"])
        self.assertEqual("2026-11-14", values["NightlyDate"])
        self.assertEqual("-Beta-Nightly", values["ReleaseSuffix"])
        self.assertIn('set(SLIC3R_VERSION_TAG "-beta-2026-11-14")', content)
        self.assertIn('set(SLIC3R_RC_VERSION_DOTS "2.7.63.0")', content)
        self.assertIn('set(SLIC3R_RC_VERSION "2,7,63,0")', content)
        self.assertNotIn("+UNKNOWN", content)

    def test_revision_changes_do_not_collide_or_get_rewritten(self):
        filenames = []
        for version in ("2.7.63.0", "2.7.63.1", "2.7.63.12", "12.34.567.89"):
            with self.subTest(version=version):
                _, values = nightly.prepare(fixture(version), DAY)
                self.assertEqual(version, values["InstallerVersion"])
                self.assertEqual(f"SuperSlicer_{version}-beta-2026-11-14.msi", values["MSI_FILE_NAME"])
                filenames.append(values["MSI_FILE_NAME"])
        self.assertEqual(len(filenames), len(set(filenames)))

    def test_next_day_changes_date_not_numeric_version(self):
        first_content, first = nightly.prepare(fixture(), DAY)
        second_content, second = nightly.prepare(fixture(), date(2026, 11, 15))
        self.assertEqual(first["InstallerVersion"], second["InstallerVersion"])
        self.assertNotEqual(first["MSI_FILE_NAME"], second["MSI_FILE_NAME"])
        self.assertIn("-2026-11-14", first_content)
        self.assertIn("-2026-11-15", second_content)

    def test_optional_suffix_and_beta_number_are_retained(self):
        for tag, expected in (("+UNKNOWN", ""), ("-beta2+UNKNOWN", "-beta2"), ("-rc.1+UNKNOWN", "-rc.1")):
            _, values = nightly.prepare(fixture(tag=tag), DAY)
            self.assertEqual(f"SuperSlicer_2.7.63.0{expected}-2026-11-14.msi", values["MSI_FILE_NAME"])

    def test_invalid_or_missing_nightly_marker_is_rejected(self):
        for tag in ("", "-beta", "-beta-2026-11-14", "-beta+UNKNOWN+UNKNOWN", "../+UNKNOWN", "-beta+OTHER"):
            with self.subTest(tag=tag), self.assertRaises(nightly.NightlyVersionError):
                nightly.prepare(fixture(tag=tag), DAY)

    def test_invalid_numeric_versions_are_rejected(self):
        for version in ("", "2.7.63", "2.7.63.0.1", "2.7.63.beta", "2.7.63.-1", "2.7.63.0-beta", "2.7.63.0 "):
            with self.subTest(version=version), self.assertRaises(nightly.NightlyVersionError):
                nightly.prepare(fixture(version), DAY)

    def test_msi_and_resource_numeric_bounds_are_checked(self):
        _, values = nightly.prepare(fixture("255.255.65535.65535"), DAY)
        self.assertEqual("255.255.65535.65535", values["InstallerVersion"])
        for version in ("256.7.63.0", "2.256.63.0", "2.7.65536.0", "2.7.63.65536"):
            with self.subTest(version=version), self.assertRaisesRegex(nightly.NightlyVersionError, "limits"):
                nightly.prepare(fixture(version), DAY)

    def test_exe_resource_and_msi_version_must_match(self):
        for resource in ("2,7,63,1", "2,7,63", "invalid"):
            content = fixture().replace('"2,7,63,0"', f'"{resource}"')
            with self.subTest(resource=resource), self.assertRaisesRegex(nightly.NightlyVersionError, "disagree"):
                nightly.prepare(content, DAY)

    def test_missing_and_duplicate_literals_fail_closed(self):
        for name in ("SLIC3R_APP_KEY", "SLIC3R_RC_VERSION_DOTS", "SLIC3R_RC_VERSION", "SLIC3R_VERSION_TAG"):
            lines = fixture().splitlines(keepends=True)
            selected = next(line for line in lines if line.startswith(f"set({name} "))
            for content in (fixture().replace(selected, ""), fixture() + selected):
                with self.subTest(name=name), self.assertRaisesRegex(nightly.NightlyVersionError, "exactly one"):
                    nightly.prepare(content, DAY)

    def test_unsafe_product_names_cannot_escape_filename(self):
        for product in ("../SuperSlicer", "SuperSlicer/path", "SuperSlicer%PATH%", "SuperSlicer\nINJECTED=1"):
            with self.subTest(product=product), self.assertRaises(nightly.NightlyVersionError):
                nightly.prepare(fixture(product=product), DAY)

    def test_iso_date_validation(self):
        self.assertEqual(date(2024, 2, 29), nightly.parse_date("2024-02-29"))
        for value in ("2026-02-29", "2026-11-31", "20261114", "26-11-14", "2026-11-14\nINJECTED=1"):
            with self.subTest(value=value), self.assertRaises(argparse.ArgumentTypeError):
                nightly.parse_date(value)

    def test_dated_filename_still_matches_signpath_configuration(self):
        configuration = ET.parse(ROOT / ".github/signpath/artifact-configuration-msi.xml")
        msi = configuration.find(".//{http://signpath.io/artifact-configuration/v1}msi-file")
        _, values = nightly.prepare(fixture(), DAY)
        self.assertTrue(fnmatch.fnmatchcase(values["MSI_FILE_NAME"], msi.attrib["path"]))

    def test_same_version_upgrade_settings_are_preserved(self):
        template = ET.parse(ROOT / "src/platform/msw/winInstaller.wxs.in")
        namespace = {"w": "http://schemas.microsoft.com/wix/2006/wi"}
        product = template.find("w:Product", namespace)
        self.assertEqual("*", product.attrib["Id"])
        self.assertEqual("$(var.InstallerVersion)", product.attrib["Version"])
        self.assertEqual("yes", product.find("w:MajorUpgrade", namespace).attrib["AllowSameVersionUpgrades"])


class NightlyVersionCliTests(unittest.TestCase):
    def setUp(self):
        approved_temp = Path(os.environ.get("LOCALAPPDATA", tempfile.gettempdir())) / "Temp/opencode"
        self.folder = tempfile.TemporaryDirectory(dir=approved_temp if approved_temp.is_dir() else None)
        self.root = Path(self.folder.name)
        self.source = self.root / "version.inc"
        self.source.write_text(fixture(), encoding="utf-8")
        self.dated = self.root / "version.date.inc"
        self.env = self.root / "github-env"
        self.arguments = ["--version-file", str(self.source), "--dated-version-file", str(self.dated), "--github-env", str(self.env)]
        self.redirect = contextlib.redirect_stdout(io.StringIO())
        self.redirect.__enter__()

    def tearDown(self):
        self.redirect.__exit__(None, None, None)
        self.folder.cleanup()

    def test_cli_preserves_source_and_appends_environment(self):
        original = self.source.read_bytes()
        self.env.write_text("EXISTING=value\n", encoding="utf-8")
        nightly.main([*self.arguments, "--date", "2026-11-14"])
        self.assertEqual(original, self.source.read_bytes())
        self.assertIn("-beta-2026-11-14", self.dated.read_text())
        lines = self.env.read_text().splitlines()
        self.assertEqual("EXISTING=value", lines[0])
        self.assertIn("InstallerVersion=2.7.63.0", lines)
        self.assertIn("MSI_FILE_NAME=SuperSlicer_2.7.63.0-beta-2026-11-14.msi", lines)
        self.assertIn("ReleaseSuffix=-Beta-Nightly", lines)

    def test_clock_read_once_for_binaries_and_installer(self):
        with mock.patch.object(nightly, "date", wraps=date) as clock:
            clock.today.side_effect = [DAY, date(2026, 11, 15)]
            nightly.main(self.arguments)
            clock.today.assert_called_once_with()
        self.assertIn("-beta-2026-11-14", self.dated.read_text())
        self.assertIn("MSI_FILE_NAME=SuperSlicer_2.7.63.0-beta-2026-11-14.msi", self.env.read_text())
        self.assertNotIn("2026-11-15", self.dated.read_text() + self.env.read_text())

    def test_invalid_source_writes_no_outputs_or_fallback(self):
        self.source.write_text(fixture("2.7.63"), encoding="utf-8")
        with self.assertRaises(nightly.NightlyVersionError):
            nightly.main([*self.arguments, "--date", "2026-11-14"])
        self.assertFalse(self.dated.exists())
        self.assertFalse(self.env.exists())

    def test_cli_cannot_overwrite_source_or_mix_output_files(self):
        original = self.source.read_bytes()
        for dated, env in ((self.source, self.env), (self.dated, self.source), (self.dated, self.dated)):
            with self.subTest(dated=dated, env=env), self.assertRaisesRegex(nightly.NightlyVersionError, "distinct"):
                nightly.main(["--version-file", str(self.source), "--dated-version-file", str(dated), "--github-env", str(env)])
        self.assertEqual(original, self.source.read_bytes())

    def test_invalid_date_rejected_before_writes(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            nightly.main([*self.arguments, "--date", "2026-02-29"])
        self.assertFalse(self.dated.exists())
        self.assertFalse(self.env.exists())


try:
    import yaml
except ImportError:
    yaml = None


@unittest.skipIf(yaml is None, "Install PyYAML to validate workflow contracts")
class NightlyWorkflowTests(unittest.TestCase):
    def setUp(self):
        load = lambda name: yaml.load((ROOT / ".github/workflows" / name).read_text(encoding="utf-8"), Loader=yaml.BaseLoader)
        self.workflow = load("ccpp_win.yml")
        self.steps = load("windows_build.yml")["jobs"]["build"]["steps"]
        self.package = load("windows_package.yml")["jobs"]["package"]
        self.package_steps = self.package["steps"]

    def step(self, name, steps=None):
        return next(step for step in (steps or self.steps) if step.get("name") == name)

    def test_nightly_uses_the_shared_workers_in_nightly_mode(self):
        jobs = self.workflow["jobs"]
        self.assertEqual("./.github/workflows/windows_build.yml", jobs["build"]["uses"])
        self.assertEqual("nightly", jobs["build"]["with"]["mode"])
        self.assertEqual("./.github/workflows/windows_package.yml", jobs["package"]["uses"])
        self.assertEqual("nightly", jobs["package"]["with"]["mode"])
        self.assertEqual("${{ needs.build.outputs.artifact_id }}", jobs["package"]["with"]["artifact_id"])
        self.assertEqual("${{ needs.signing.outputs.mode }}", jobs["package"]["with"]["signing"])

    def test_preparation_once_before_compilation_and_packaging(self):
        preparations = [step for step in self.steps if "nightly_version.py" in step.get("run", "")]
        self.assertEqual(1, len(preparations))
        self.assertEqual("inputs.mode == 'nightly'", preparations[0]["if"])
        self.assertLess(self.steps.index(preparations[0]), self.steps.index(self.step("Configure and build SuperSlicer")))
        setup = next(step for step in self.steps if step.get("uses", "").startswith("actions/setup-python@"))
        self.assertLess(self.steps.index(setup), self.steps.index(preparations[0]))
        self.assertEqual("3.11", setup["with"]["python-version"])
        # The recorded date reaches the manifest; packaging never reads the clock again.
        self.assertIn("$env:NightlyDate", self.step("Record the unsigned package")["run"])
        commands = "\n".join(step.get("run", "") for step in self.steps + self.package_steps)
        for old_expression in ("dateSuffix", "Get-Date", "[datetime]::Today", "Setting default version"):
            self.assertNotIn(old_expression, commands)

    def test_same_filename_used_for_linking_signing_collection_and_upload(self):
        build = self.step("Build MSI installer", self.package_steps)["run"]
        self.assertIn('-dInstallerVersion="%InstallerVersion%"', build)
        self.assertIn('-o "%MSI_FILE_NAME%"', build)
        collect = self.step("collect unsigned msi", self.package_steps)
        self.assertIn('"build/$env:MSI_FILE_NAME"', collect["run"])
        self.assertNotIn("MSI_FILE_NAME", collect.get("env", {}))
        upload = self.step("Upload MSI artifact", self.package_steps)["with"]
        self.assertEqual("build/${{ env.MSI_FILE_NAME }}", upload["path"])
        self.assertEqual("error", upload["if-no-files-found"])

    def test_unsigned_artifact_collection_precedes_signing_and_verified_upload(self):
        names = [step.get("name") for step in self.package_steps]
        self.assertLess(names.index("Build MSI installer"), names.index("collect unsigned msi"))
        self.assertLess(names.index("collect unsigned msi"), names.index("sign msi"))
        self.assertLess(names.index("verify and replace signed msi"), names.index("Check the installer contents"))
        self.assertLess(names.index("Check the installer contents"), names.index("Upload MSI artifact"))
        self.assertEqual("${{ inputs.signing == 'test' && vars.SIGNPATH_TEST_SIGNING_POLICY_SLUG || "
                         "inputs.signing == 'production' && vars.SIGNPATH_SIGNING_POLICY_SLUG || '' }}",
                         self.package["env"]["SIGNPATH_SIGNING_POLICY_SLUG"])
        check = self.step("check signing configuration", self.workflow["jobs"]["signing"]["steps"])
        self.assertEqual("${{ vars.SIGNPATH_TEST_SIGNING_POLICY_SLUG }}", check["env"]["SIGNPATH_SIGNING_POLICY_SLUG"])


if __name__ == "__main__":
    unittest.main()
