"""Offline WinGet guards; no installer download, PAT, fork writes or external PRs."""

import contextlib
import copy
import importlib.util
import io
import os
from pathlib import Path
import re
import sys
import tempfile
import types
import unittest
from unittest import mock


SCRIPTS = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("winget_preflight", SCRIPTS / "winget.py")
winget = importlib.util.module_from_spec(SPEC)
with mock.patch.object(sys, "path", [str(SCRIPTS), *sys.path]):
    SPEC.loader.exec_module(winget)
VERSION = "2.7.63.0"
NAME = f"SuperSlicer_{VERSION}_win64.msi"
URL = f"https://github.com/supermerill/SuperSlicer/releases/download/{VERSION}/{NAME}"


class WinGetTests(unittest.TestCase):
    def setUp(self):
        self.api = mock.Mock(spec=winget.GitHub)
        self.api.repository = winget.REPOSITORY
        self.item = {"id": 42, "tag_name": VERSION, "draft": False, "prerelease": False,
                     "published_at": "2026-11-13T00:00:00Z"}
        self.api.request.return_value = self.item
        self.asset = {"name": NAME, "state": "uploaded", "size": 100,
                      "browser_download_url": URL}
        self.api.assets.return_value = {NAME: self.asset}
        self.catalog = mock.Mock(spec=winget.GitHub)
        self.catalog.request.return_value = [{"name": "2.7.62.0", "type": "dir"}]
        self.stdout = io.StringIO()
        self.redirect = contextlib.redirect_stdout(self.stdout)
        self.redirect.__enter__()

    def tearDown(self):
        self.redirect.__exit__(None, None, None)

    def prepare(self, tag=VERSION):
        return winget.prepare(self.api, self.catalog, tag)

    def test_stable_release_selects_only_the_exact_msi(self):
        self.api.assets.return_value["portable.zip"] = {"name": "portable.zip"}
        result = self.prepare()
        self.assertEqual("true", result["submit"])
        self.assertEqual(VERSION, result["version"])
        self.assertEqual(VERSION, result["release_tag"])
        expression = re.compile(result["installers_regex"])
        self.assertIsNotNone(expression.fullmatch(NAME))
        for name in ("portable.zip", NAME + ".zip", "SuperSlicer_2.7.6300_win64.msi", "Other_2.7.63.0_win64.msi"):
            self.assertIsNone(expression.fullmatch(name))
        self.api.request.assert_called_once_with("GET", f"/releases/tags/{VERSION}")
        self.api.assets.assert_called_once_with(42)

    def test_beta_nightly_and_malformed_tags_fail_before_api(self):
        for tag in ("2.7.63.0-beta", "2.7.63.0+UNKNOWN", "2.7.63.0-2026-11-13", "nightly", "v2.7.63.0",
                    "2.7.63", "../2.7.63.0", "2.7.63.0\nsubmit=true"):
            with self.subTest(tag=tag), self.assertRaises(winget.ReleaseError):
                self.prepare(tag)
        self.api.request.assert_not_called()
        self.catalog.request.assert_not_called()

    def test_other_repository_is_rejected(self):
        self.api.repository = "example/SuperSlicer"
        with self.assertRaisesRegex(winget.ReleaseError, "upstream"):
            self.prepare()
        self.api.request.assert_not_called()

    def test_draft_prerelease_unpublished_and_missing_flags_fail(self):
        original = copy.deepcopy(self.item)
        for field, value in (("draft", True), ("prerelease", True), ("published_at", None),
                             ("draft", None), ("prerelease", None)):
            self.item.clear()
            self.item.update(original)
            if value is None and field != "published_at":
                del self.item[field]
            else:
                self.item[field] = value
            with self.subTest(field=field, value=value), self.assertRaisesRegex(winget.ReleaseError, "published, non-prerelease"):
                self.prepare()
        self.api.assets.assert_not_called()

    def test_wrong_release_response_tag_fails(self):
        self.item["tag_name"] = "2.7.62.0"
        with self.assertRaisesRegex(winget.ReleaseError, "different release tag"):
            self.prepare()

    def test_null_release_flags_are_not_treated_as_false(self):
        for field in ("draft", "prerelease"):
            item = {**self.item, field: None}
            self.api.request.return_value = item
            with self.subTest(field=field), self.assertRaisesRegex(winget.ReleaseError, "published, non-prerelease"):
                self.prepare()

    def test_missing_wrong_named_empty_and_incomplete_msi_fail(self):
        for assets in ({}, {"SuperSlicer_2.7.63.0.msi": self.asset},
                       {NAME: {**self.asset, "size": 0}}, {NAME: {**self.asset, "state": "starter"}}):
            self.api.assets.return_value = assets
            with self.subTest(assets=assets), self.assertRaisesRegex(winget.ReleaseError, "Missing or incomplete"):
                self.prepare()
        self.catalog.request.assert_not_called()

    def test_msi_url_must_be_the_public_upstream_release_asset(self):
        for url in ("https://example.com/installer.msi", URL.replace(VERSION, "2.7.62.0"),
                    URL.replace("github.com", "github.com.example.com"), None):
            self.asset["browser_download_url"] = url
            with self.subTest(url=url), self.assertRaisesRegex(winget.ReleaseError, "direct asset"):
                self.prepare()

    def test_missing_initial_package_explains_manual_bootstrap(self):
        self.catalog.request.side_effect = winget.ApiError(404, "GET", winget.CATALOG_PATH)
        with self.assertRaisesRegex(winget.ReleaseError, "first version manually"):
            self.prepare()

    def test_catalog_api_errors_are_not_treated_as_absent_package(self):
        self.catalog.request.side_effect = winget.ApiError(403, "GET", winget.CATALOG_PATH)
        with self.assertRaises(winget.ApiError) as caught:
            self.prepare()
        self.assertEqual(403, caught.exception.status)

    def test_catalog_needs_an_existing_version_directory(self):
        for versions in ([], {"message": "unexpected response"}, [{"name": "README.md", "type": "file"}]):
            self.catalog.request.return_value = versions
            with self.subTest(versions=versions), self.assertRaisesRegex(winget.ReleaseError, "no baseline"):
                self.prepare()

    def test_already_merged_version_is_skipped_without_writes(self):
        self.catalog.request.return_value.append({"name": VERSION, "type": "dir"})
        self.assertEqual("false", self.prepare()["submit"])
        self.assertIn("::warning::", self.stdout.getvalue())
        for api in (self.api, self.catalog):
            self.assertTrue(all(call.args[0] == "GET" for call in api.request.call_args_list))

    def test_cli_writes_validated_outputs_without_credentials(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "outputs"
            with mock.patch.dict(os.environ, {"GH_TOKEN": "test-only-placeholder", "GITHUB_REPOSITORY": winget.REPOSITORY}), \
                    mock.patch.object(winget, "GitHub", side_effect=[self.api, self.catalog]):
                winget.main(["--tag", VERSION, "--output", str(output)])
            text = output.read_text(encoding="utf-8")
        self.assertIn(f"version={VERSION}\n", text)
        self.assertIn("submit=true\n", text)
        self.assertNotIn("test-only-placeholder", text + self.stdout.getvalue())


try:
    import yaml
except ImportError:
    yaml = None


@unittest.skipIf(yaml is None, "Install PyYAML to validate workflow contracts")
class WinGetWorkflowTests(unittest.TestCase):
    def setUp(self):
        path = SCRIPTS.parent / "workflows/release_winget.yml"
        self.workflow = yaml.load(path.read_text(encoding="utf-8"), Loader=yaml.BaseLoader)
        self.job = self.workflow["jobs"]["publish"]

    def test_only_stable_publication_or_explicit_retry_triggers(self):
        self.assertEqual({"release", "workflow_dispatch"}, set(self.workflow["on"]))
        self.assertEqual(["released"], self.workflow["on"]["release"]["types"])
        self.assertEqual("true", self.workflow["on"]["workflow_dispatch"]["inputs"]["release_tag"]["required"])
        self.assertEqual("false", self.workflow["concurrency"]["cancel-in-progress"])
        self.assertNotIn("release_tag", self.workflow["concurrency"]["group"])

    def test_enable_upstream_stability_and_manual_default_branch_guards(self):
        expression = self.job["if"].replace("&&", " and ").replace("||", " or ").replace("!", " not ").strip()
        for enabled in ("true", "false"):
            for repository in (winget.REPOSITORY, "example/SuperSlicer"):
                for event in ("release", "workflow_dispatch", "push"):
                    for ref in ("refs/heads/master_27", "refs/heads/rc", "refs/tags/2.7.63.0"):
                        for draft, prerelease in ((False, False), (True, False), (False, True)):
                            result = eval(expression, {"__builtins__": {}, "format": lambda text, value: text.format(value)}, {
                                "vars": types.SimpleNamespace(WINGET_ENABLED=enabled),
                                "github": types.SimpleNamespace(repository=repository, event_name=event, ref=ref,
                                    event=types.SimpleNamespace(release=types.SimpleNamespace(draft=draft, prerelease=prerelease),
                                                               repository=types.SimpleNamespace(default_branch="master_27"))),
                            })
                            expected = enabled == "true" and repository == winget.REPOSITORY and (
                                (event == "release" and not draft and not prerelease) or
                                (event == "workflow_dispatch" and ref == "refs/heads/master_27"))
                            self.assertEqual(expected, result)

    def test_read_only_token_checkout_and_no_shell_interpolation(self):
        self.assertEqual({"contents": "read"}, self.workflow["permissions"])
        self.assertNotIn("permissions", self.job)
        checkout = self.job["steps"][0]
        self.assertEqual("${{ github.sha }}", checkout["with"]["ref"])
        self.assertEqual("false", checkout["with"]["persist-credentials"])
        for step in self.job["steps"]:
            self.assertNotIn("${{", step.get("run", ""))

    def test_submission_is_pinned_exact_version_only_and_keeps_history(self):
        action = next(step for step in self.job["steps"] if step.get("id") == "winget")
        self.assertRegex(action["uses"], r"^vedantmgoyal9/winget-releaser@[0-9a-f]{40}$")
        self.assertEqual("steps.prepare.outputs.submit == 'true'", action["if"])
        inputs = action["with"]
        self.assertEqual(winget.IDENTIFIER, inputs["identifier"])
        self.assertEqual(winget.REPOSITORY, inputs["release-repository"])
        self.assertEqual("${{ steps.prepare.outputs.version }}", inputs["version"])
        self.assertEqual("${{ steps.prepare.outputs.release_tag }}", inputs["release-tag"])
        self.assertEqual("${{ steps.prepare.outputs.installers_regex }}", inputs["installers-regex"])
        self.assertEqual("0", inputs["max-versions-to-keep"])
        self.assertEqual("${{ secrets.WINGET_TOKEN }}", inputs["token"])


if __name__ == "__main__":
    unittest.main()
