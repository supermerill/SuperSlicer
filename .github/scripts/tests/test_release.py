"""Offline tests: no real GitHub writes, tags, releases, tokens or build runs."""

import contextlib
import copy
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import types
import unittest
from unittest import mock
import urllib.parse
import zipfile

SPEC = importlib.util.spec_from_file_location("rc_release", Path(__file__).parents[1] / "release.py")
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)
ROOT = Path(__file__).resolve().parents[2]
SHA = "a" * 40
OTHER_SHA = "b" * 40
VERSION = "2.7.63.0-beta"
PRODUCT = "SuperSlicer"
FOOTER = "Free code signing provided by SignPath.io. [Policy]({{CODE_SIGNING_POLICY_URL}})"


class FakeGitHub(release.GitHub):
    def __init__(self):
        super().__init__("example/SuperSlicer", "unused-fake-token")
        self.refs = {}
        self.tag_objects = {}
        self.releases = []
        self.files = {}
        self.calls = []
        self.hook = None

    def add_draft(self, tag=VERSION, sha=SHA, body="Manual notes", draft=True):
        self.refs[tag] = {"type": "commit", "sha": sha}
        item = {"id": len(self.releases) + 1, "tag_name": tag, "draft": draft, "body": body}
        self.releases.append(item)
        self.files[item["id"]] = []
        return item

    def add_asset(self, item, name, data=b"existing file", state="uploaded"):
        asset = {"id": len(self.files[item["id"]]) + 1, "name": name, "size": len(data), "state": state,
                 "digest": "sha256:" + release.hashlib.sha256(data).hexdigest()}
        self.files[item["id"]].append(asset)
        return asset

    def request(self, method, path, data=None, file=None):
        self.calls.append((method, path, copy.deepcopy(data), file.name if file else None))
        if self.hook:
            self.hook(method, path, data, file)
        parsed = urllib.parse.urlsplit(path)
        query = urllib.parse.parse_qs(parsed.query)
        endpoint = parsed.path
        result = None
        if method == "GET" and endpoint.startswith("/git/ref/tags/"):
            tag = urllib.parse.unquote(endpoint[len("/git/ref/tags/"):])
            if tag not in self.refs:
                raise release.ApiError(404, method, path)
            result = {"object": self.refs[tag]}
        elif method == "GET" and endpoint.startswith("/git/tags/"):
            result = {"object": self.tag_objects[endpoint.rsplit("/", 1)[1]]}
        elif method == "POST" and endpoint == "/git/refs":
            tag = data["ref"][len("refs/tags/"):]
            if tag in self.refs:
                raise release.ApiError(422, method, path)
            self.refs[tag] = {"type": "commit", "sha": data["sha"]}
            result = {"object": self.refs[tag]}
        elif method == "GET" and endpoint == "/releases":
            start = (int(query["page"][0]) - 1) * 100
            result = self.releases[start:start + 100]
        elif method == "POST" and endpoint == "/releases":
            if any(item["tag_name"] == data["tag_name"] for item in self.releases):
                raise release.ApiError(422, method, path)
            item = {**data, "id": len(self.releases) + 1}
            self.releases.append(item)
            self.files[item["id"]] = []
            result = item
        elif endpoint.startswith("/releases/"):
            pieces = endpoint.split("/")
            identifier = int(pieces[2])
            item = next(item for item in self.releases if item["id"] == identifier)
            if len(pieces) == 3:
                if method == "PATCH":
                    item.update(data)
                result = item
            elif pieces[3] == "assets":
                if method == "GET":
                    start = (int(query["page"][0]) - 1) * 100
                    result = self.files[identifier][start:start + 100]
                elif method == "POST":
                    name = query["name"][0]
                    if any(asset["name"] == name for asset in self.files[identifier]):
                        raise release.ApiError(422, method, path)
                    result = self.add_asset(item, name, file.read_bytes())
        if result is None:
            raise AssertionError(f"Unhandled fake request: {method} {path}")
        return copy.deepcopy(result)

    def writes(self):
        return [call for call in self.calls if call[0] != "GET"]


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.api = FakeGitHub()
        self.folder = tempfile.TemporaryDirectory()
        self.root = Path(self.folder.name)
        self.output = io.StringIO()
        self.redirect = contextlib.redirect_stdout(self.output)
        self.redirect.__enter__()

    def tearDown(self):
        self.redirect.__exit__(None, None, None)
        self.folder.cleanup()

    def prepare(self):
        return release.prepare(self.api, PRODUCT, VERSION, SHA, FOOTER)

    def payload(self, platform):
        build = self.root / "build"
        build.mkdir(exist_ok=True)
        if platform == "windows":
            (build / "package/mesa").mkdir(parents=True)
            (build / "package/superslicer.exe").write_bytes(b"verified application fixture")
            (build / "package/mesa/opengl32.dll").write_bytes(b"upstream fixture")
            (build / "SuperSlicer_2.7.63.0.msi").write_bytes(b"verified installer fixture")
        elif platform.startswith("macos-"):
            arch = "intel" if platform == "macos-intel" else "arm"
            (build / f"SuperSlicer-macOS-{arch}.dmg").write_bytes(b"disk image fixture")
        else:
            toolkit = "GTK2" if platform == "linux-gtk2" else "GTK3"
            (build / f"SuperSlicer-linux-x64-{toolkit}.AppImage").write_bytes(b"AppImage fixture")
            (build / f"SuperSlicer-linux-x64-{toolkit}.tgz").write_bytes(b"archive fixture")
        destination = self.root / platform
        release.stage(platform, PRODUCT, VERSION, SHA, build, destination)
        return destination

    def test_version_strips_unknown_without_date_or_numeric_corruption(self):
        product, version = release.read_version(ROOT.parent / "version.inc")
        self.assertEqual(PRODUCT, product)
        self.assertNotIn("UNKNOWN", version)
        file = self.root / "version.inc"
        file.write_text('set(SLIC3R_APP_KEY "SuperSlicer")\nset(SLIC3R_RC_VERSION_DOTS "2.7.63.12")\nset(SLIC3R_VERSION_TAG "-beta2+UNKNOWN")\n')
        self.assertEqual((PRODUCT, "2.7.63.12-beta2"), release.read_version(file))

    def test_all_seven_names_stable_compatible_with_updater(self):
        names = release.asset_names(PRODUCT, VERSION)
        self.assertEqual(7, sum(map(len, names.values())))
        self.assertIn("win", names["windows"][0])
        self.assertTrue(names["windows"][1].endswith(".msi"))
        self.assertIn("macos", names["macos-intel"][0])
        self.assertNotIn("arm", names["macos-intel"][0])
        self.assertIn("macos_arm", names["macos-arm"][0])
        self.assertIn("ubuntu", names["linux-gtk3"][0])
        self.assertIn("gtk2", names["linux-gtk2"][0])
        self.assertEqual(names, release.asset_names(PRODUCT, VERSION))

    def test_new_tag_and_draft_created_at_exact_commit(self):
        result = self.prepare()
        self.assertEqual(SHA, self.api.tag_sha(VERSION))
        self.assertTrue(self.api.releases[0]["draft"])
        self.assertTrue(self.api.releases[0]["prerelease"])
        self.assertEqual(SHA, self.api.releases[0]["target_commitish"])
        self.assertEqual(VERSION, result["tag"])
        self.assertTrue(all(result[platform.replace("-", "_")] == "true" for platform in release.PLATFORMS))
        self.assertIn(f"/blob/{VERSION}/doc/", self.api.releases[0]["body"])

    def test_tag_conflict_fails_without_writes(self):
        self.api.refs[VERSION] = {"type": "commit", "sha": OTHER_SHA}
        with self.assertRaisesRegex(release.ReleaseError, "different commit"):
            self.prepare()
        self.assertEqual([], self.api.writes())

    def test_duplicate_releases_and_assets_fail_without_writes(self):
        draft = self.api.add_draft()
        self.api.add_draft()
        with self.assertRaisesRegex(release.ReleaseError, "Multiple releases"):
            self.prepare()
        self.api.releases.pop()
        name = release.asset_names(PRODUCT, VERSION)["macos-arm"][0]
        self.api.add_asset(draft, name)
        self.api.add_asset(draft, name)
        with self.assertRaisesRegex(release.ReleaseError, "Duplicate release asset"):
            self.prepare()
        self.assertEqual([], self.api.writes())

    def test_annotated_tag_resolves_to_matching_commit(self):
        self.api.refs[VERSION] = {"type": "tag", "sha": "c" * 40}
        self.api.tag_objects["c" * 40] = {"type": "tag", "sha": "d" * 40}
        self.api.tag_objects["d" * 40] = {"type": "commit", "sha": SHA}
        self.prepare()
        self.assertFalse(any(call[1] == "/git/refs" for call in self.api.writes()))

    def test_tag_cycle_fails(self):
        self.api.refs[VERSION] = {"type": "tag", "sha": "c" * 40}
        self.api.tag_objects["c" * 40] = {"type": "tag", "sha": "c" * 40}
        with self.assertRaisesRegex(release.ReleaseError, "cycle"):
            self.prepare()

    def test_public_release_fails_before_any_write(self):
        self.api.add_draft(draft=False)
        with self.assertRaisesRegex(release.ReleaseError, "already public"):
            self.prepare()
        self.assertEqual([], self.api.writes())

    def test_existing_draft_missing_its_tag_fails(self):
        self.api.add_draft()
        del self.api.refs[VERSION]
        with self.assertRaisesRegex(release.ReleaseError, "lost its tag"):
            self.prepare()
        self.assertEqual([], self.api.writes())

    def test_full_platform_skipped_partial_platform_rebuilt(self):
        draft = self.api.add_draft()
        names = release.asset_names(PRODUCT, VERSION)
        self.api.add_asset(draft, names["windows"][0])
        self.api.add_asset(draft, names["macos-intel"][0])
        result = self.prepare()
        self.assertEqual("true", result["windows"])
        self.assertEqual("false", result["macos_intel"])
        self.assertIn("::warning::macos-intel", self.output.getvalue())

    def test_complete_release_skips_all_five_builds(self):
        draft = self.api.add_draft()
        for files in release.asset_names(PRODUCT, VERSION).values():
            for name in files:
                self.api.add_asset(draft, name)
        result = self.prepare()
        self.assertTrue(all(result[platform.replace("-", "_")] == "false" for platform in release.PLATFORMS))

    def test_incomplete_asset_is_explicit_error_not_overwritten(self):
        draft = self.api.add_draft()
        self.api.add_asset(draft, release.asset_names(PRODUCT, VERSION)["windows"][0], b"", state="starter")
        with self.assertRaisesRegex(release.ReleaseError, "Incomplete/empty"):
            self.prepare()
        self.assertEqual([], self.api.writes())

    def test_repeated_prepare_preserves_notes_and_footer_is_not_duplicated(self):
        draft = self.api.add_draft(body="User-written notes\n")
        self.prepare()
        body = draft["body"]
        writes = len(self.api.writes())
        self.prepare()
        self.assertEqual(body, draft["body"])
        self.assertEqual(writes, len(self.api.writes()))
        self.assertTrue(body.startswith("User-written notes"))
        self.assertEqual(1, body.count(release.FOOTER_START))

    def test_managed_footer_update_preserves_text_on_both_sides(self):
        body = f"Manual prefix\n{release.FOOTER_START}\nold\n{release.FOOTER_END}\nManual suffix"
        result = release.managed_body(body, FOOTER, self.api.repository, VERSION)
        self.assertTrue(result.startswith("Manual prefix\n"))
        self.assertTrue(result.endswith("\nManual suffix"))
        self.assertNotIn("\nold\n", result)

    def test_prepare_preserves_notes_changed_during_asset_listing(self):
        draft = self.api.add_draft()
        def edit_notes(method, path, data, file):
            if method == "GET" and "/assets?" in path:
                draft["body"] = "Notes edited during preparation\n"
        self.api.hook = edit_notes
        self.prepare()
        self.assertTrue(draft["body"].startswith("Notes edited during preparation\n"))

    def test_prepare_rejects_publication_during_asset_listing(self):
        draft = self.api.add_draft()
        def publish(method, path, data, file):
            if method == "GET" and "/assets?" in path:
                draft["draft"] = False
        self.api.hook = publish
        with self.assertRaisesRegex(release.ReleaseError, "already public"):
            self.prepare()
        self.assertEqual([], self.api.writes())

    def test_malformed_footer_fails(self):
        with self.assertRaisesRegex(release.ReleaseError, "Malformed"):
            release.managed_body(release.FOOTER_START, FOOTER, self.api.repository, VERSION)

    def test_pagination_for_drafts_and_assets(self):
        for index in range(100):
            self.api.add_draft(tag=f"old-{index}")
        draft = self.api.add_draft()
        for index in range(100):
            self.api.add_asset(draft, f"other-{index}.txt")
        self.api.add_asset(draft, release.asset_names(PRODUCT, VERSION)["macos-arm"][0])
        result = self.prepare()
        self.assertEqual(str(draft["id"]), result["release_id"])
        self.assertEqual("false", result["macos_arm"])

    def test_concurrent_tag_and_release_creation_reuses_matching_draft(self):
        def race(method, path, data, file):
            if method == "POST" and path == "/git/refs":
                self.api.refs[VERSION] = {"type": "commit", "sha": SHA}
            if method == "POST" and path == "/releases":
                self.api.add_draft()
        self.api.hook = race
        result = self.prepare()
        self.assertEqual("1", result["release_id"])
        self.assertEqual(1, len(self.api.releases))

    def test_all_platforms_stage_exact_raw_files_and_manifest(self):
        for platform in release.PLATFORMS:
            with self.subTest(platform=platform):
                directory = self.payload(platform)
                names = release.validate_payload(platform, PRODUCT, VERSION, SHA, directory)
                self.assertEqual(release.asset_names(PRODUCT, VERSION)[platform], names)
                if platform == "windows":
                    with zipfile.ZipFile(directory / names[0]) as zipped:
                        self.assertIn(f"SuperSlicer_{VERSION}_win64/mesa/opengl32.dll", zipped.namelist())
                    self.assertEqual(b"verified installer fixture", (directory / names[1]).read_bytes())

    def test_payload_tampering_missing_files_and_wrong_commit_fail(self):
        directory = self.payload("macos-arm")
        path = directory / release.asset_names(PRODUCT, VERSION)["macos-arm"][0]
        path.write_bytes(b"tampered")
        with self.assertRaisesRegex(release.ReleaseError, "hash mismatch"):
            release.validate_payload("macos-arm", PRODUCT, VERSION, SHA, directory)
        with self.assertRaisesRegex(release.ReleaseError, "does not match"):
            release.validate_payload("macos-arm", PRODUCT, VERSION, OTHER_SHA, directory)
        path.unlink()
        with self.assertRaisesRegex(release.ReleaseError, "Unexpected or missing"):
            release.validate_payload("macos-arm", PRODUCT, VERSION, SHA, directory)

    def test_staging_rejects_missing_empty_files_and_nonempty_destination(self):
        build = self.root / "missing-build"
        build.mkdir()
        destination = self.root / "stage"
        with self.assertRaisesRegex(release.ReleaseError, "Missing or empty"):
            release.stage("macos-arm", PRODUCT, VERSION, SHA, build, destination)
        (build / "SuperSlicer-macOS-arm.dmg").write_bytes(b"")
        with self.assertRaisesRegex(release.ReleaseError, "Missing or empty"):
            release.stage("macos-arm", PRODUCT, VERSION, SHA, build, destination)
        (destination / "preserve.txt").write_text("keep")
        with self.assertRaisesRegex(release.ReleaseError, "not empty"):
            release.stage("macos-arm", PRODUCT, VERSION, SHA, build, destination)
        self.assertEqual("keep", (destination / "preserve.txt").read_text())

    def test_payload_extra_file_is_rejected(self):
        directory = self.payload("macos-intel")
        (directory / "unexpected.txt").write_text("not part of release")
        with self.assertRaisesRegex(release.ReleaseError, "Unexpected or missing"):
            release.validate_payload("macos-intel", PRODUCT, VERSION, SHA, directory)

    def test_manifest_path_traversal_is_rejected_before_open(self):
        directory = self.payload("macos-intel")
        path = directory / release.MANIFEST
        manifest = json.loads(path.read_text())
        manifest["files"][0]["name"] = "../outside"
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(release.ReleaseError, "Unexpected or missing"):
            release.validate_payload("macos-intel", PRODUCT, VERSION, SHA, directory)

    def test_upload_only_missing_asset_never_overwrites_existing(self):
        draft = self.api.add_draft()
        names = release.asset_names(PRODUCT, VERSION)["windows"]
        first = copy.deepcopy(self.api.add_asset(draft, names[0]))
        directory = self.payload("windows")
        release.upload(self.api, "windows", PRODUCT, VERSION, SHA, draft["id"], directory)
        self.assertEqual(first, self.api.files[draft["id"]][0])
        uploaded = [call[3] for call in self.api.writes() if call[3]]
        self.assertEqual([names[1]], uploaded)
        self.assertTrue(all(call[0] != "DELETE" for call in self.api.calls))

    def test_repeated_upload_does_not_upload_again(self):
        draft = self.api.add_draft()
        directory = self.payload("linux-gtk3")
        for _ in range(2):
            release.upload(self.api, "linux-gtk3", PRODUCT, VERSION, SHA, draft["id"], directory)
        self.assertEqual(2, len([call for call in self.api.writes() if call[3]]))

    def test_upload_rejects_public_release_or_moved_tag(self):
        draft = self.api.add_draft(draft=False)
        directory = self.payload("macos-arm")
        with self.assertRaisesRegex(release.ReleaseError, "already public"):
            release.upload(self.api, "macos-arm", PRODUCT, VERSION, SHA, draft["id"], directory)
        draft["draft"] = True
        self.api.refs[VERSION]["sha"] = OTHER_SHA
        with self.assertRaisesRegex(release.ReleaseError, "different commit"):
            release.upload(self.api, "macos-arm", PRODUCT, VERSION, SHA, draft["id"], directory)
        self.assertEqual([], self.api.writes())

    def test_upload_rejects_wrong_release_id_before_writing(self):
        draft = self.api.add_draft(tag="another-version")
        self.api.refs[VERSION] = {"type": "commit", "sha": SHA}
        directory = self.payload("macos-arm")
        with self.assertRaisesRegex(release.ReleaseError, "expected tag"):
            release.upload(self.api, "macos-arm", PRODUCT, VERSION, SHA, draft["id"], directory)
        self.assertEqual([], self.api.writes())

    def test_upload_response_digest_mismatch_fails_without_deleting(self):
        draft = self.api.add_draft()
        directory = self.payload("macos-arm")
        original_request = self.api.request
        def wrong_digest(method, path, data=None, file=None):
            result = original_request(method, path, data, file)
            if file is not None:
                result["digest"] = "sha256:" + "0" * 64
            return result
        self.api.request = wrong_digest
        with self.assertRaisesRegex(release.ReleaseError, "digest mismatch"):
            release.upload(self.api, "macos-arm", PRODUCT, VERSION, SHA, draft["id"], directory)
        self.assertFalse(any(call[0] == "DELETE" for call in self.api.calls))

    def test_upload_rechecks_after_asset_listing(self):
        draft = self.api.add_draft()
        directory = self.payload("macos-arm")
        def publish(method, path, data, file):
            if method == "GET" and "/assets?" in path:
                draft["draft"] = False
        self.api.hook = publish
        with self.assertRaisesRegex(release.ReleaseError, "already public"):
            release.upload(self.api, "macos-arm", PRODUCT, VERSION, SHA, draft["id"], directory)
        self.assertEqual([], self.api.writes())

    def test_concurrent_asset_upload_does_not_clobber(self):
        draft = self.api.add_draft()
        directory = self.payload("macos-arm")
        def race(method, path, data, file):
            if method == "POST" and "/assets?" in path:
                self.api.add_asset(draft, file.name)
        self.api.hook = race
        release.upload(self.api, "macos-arm", PRODUCT, VERSION, SHA, draft["id"], directory)
        self.assertEqual(1, len(self.api.files[draft["id"]]))
        self.assertIn("uploaded concurrently", self.output.getvalue())

    def test_verify_reports_all_missing_platforms_even_if_one_job_failed(self):
        draft = self.api.add_draft()
        self.api.add_asset(draft, release.asset_names(PRODUCT, VERSION)["macos-arm"][0])
        with self.assertRaisesRegex(release.ReleaseError, "Draft is incomplete"):
            release.verify(self.api, PRODUCT, VERSION, SHA, draft["id"],
                           {"windows": {"result": "failure"}, "macos_arm": {"result": "success"}})
        self.assertIn("failure", self.output.getvalue())
        self.assertIn("macos-arm | Complete", self.output.getvalue())

    def test_verify_full_draft_does_not_publish(self):
        draft = self.api.add_draft()
        for files in release.asset_names(PRODUCT, VERSION).values():
            for name in files:
                self.api.add_asset(draft, name)
        release.verify(self.api, PRODUCT, VERSION, SHA, draft["id"])
        self.assertTrue(draft["draft"])
        self.assertEqual([], self.api.writes())

    def test_prepare_cli_rejects_wrong_branch_and_commit_before_api(self):
        with mock.patch.dict(os.environ, {"GITHUB_REF": "refs/heads/master", "GITHUB_SHA": SHA}):
            with self.assertRaisesRegex(release.ReleaseError, "branch rc"):
                release.main(["prepare", "--source-sha", SHA])
        with mock.patch.dict(os.environ, {"GITHUB_REF": "refs/heads/rc", "GITHUB_SHA": OTHER_SHA}):
            with self.assertRaisesRegex(release.ReleaseError, "triggering"):
                release.main(["prepare", "--source-sha", SHA])

    def test_invalid_versions_and_paths_fail(self):
        for version in ("2.7.63.0+UNKNOWN", "../2.7.63", "2.7.63", "2.7.63.0\nINJECTED=1"):
            with self.subTest(version=version), self.assertRaises(release.ReleaseError):
                release.asset_names(PRODUCT, version)
        with self.assertRaises(release.ReleaseError):
            release.validate_identity("../../SuperSlicer", VERSION, SHA)


class HttpBoundaryTests(unittest.TestCase):
    """Exercise the real HTTP adapter with urlopen mocked; no network or credentials."""

    def setUp(self):
        self.api = release.GitHub("example/SuperSlicer", "test-only-placeholder")

    def response(self, payload):
        response = mock.MagicMock()
        response.__enter__.return_value.read.return_value = json.dumps(payload).encode()
        return response

    def test_json_request_uses_api_endpoint_and_token_header(self):
        response = self.response({"id": 42})
        with mock.patch.object(release.urllib.request, "urlopen", return_value=response) as urlopen:
            self.assertEqual({"id": 42}, self.api.request("POST", "/releases", {"draft": True}))
        request = urlopen.call_args.args[0]
        self.assertEqual("https://api.github.com/repos/example/SuperSlicer/releases", request.full_url)
        self.assertEqual("POST", request.method)
        self.assertEqual({"draft": True}, json.loads(request.data))
        self.assertEqual("Bearer test-only-placeholder", request.get_header("Authorization"))
        self.assertNotIn("test-only-placeholder", request.full_url)

    def test_upload_streams_file_with_content_length_and_closes_it(self):
        with tempfile.TemporaryDirectory() as folder:
            file = Path(folder) / "fixture.dmg"
            file.write_bytes(b"disk image fixture")
            def send(request, timeout):
                self.assertEqual("https://uploads.github.com/repos/example/SuperSlicer/releases/1/assets?name=fixture.dmg", request.full_url)
                self.assertNotIsInstance(request.data, bytes)
                self.assertFalse(request.data.closed)
                self.assertEqual(str(file.stat().st_size), request.get_header("Content-length"))
                self.assertEqual(file.read_bytes(), request.data.read())
                return self.response({"state": "uploaded"})
            with mock.patch.object(release.urllib.request, "urlopen", side_effect=send) as urlopen:
                self.api.request("POST", "/releases/1/assets?name=fixture.dmg", file=file)
            self.assertTrue(urlopen.call_args.args[0].data.closed)

    def test_http_failure_redacts_response_and_closes_upload(self):
        with tempfile.TemporaryDirectory() as folder:
            file = Path(folder) / "fixture.msi"
            file.write_bytes(b"installer fixture")
            error = release.urllib.error.HTTPError("https://api.github.com", 403,
                                                   "test-only-placeholder", {}, io.BytesIO(b"private response"))
            try:
                with mock.patch.object(release.urllib.request, "urlopen", side_effect=error) as urlopen:
                    with self.assertRaises(release.ApiError) as caught:
                        self.api.request("POST", "/releases/1/assets?name=fixture.msi", file=file)
                self.assertEqual(403, caught.exception.status)
                self.assertNotIn("test-only-placeholder", str(caught.exception))
                self.assertNotIn("private response", str(caught.exception))
                self.assertTrue(urlopen.call_args.args[0].data.closed)
            finally:
                error.close()

    def test_network_failure_redacts_reason(self):
        error = release.urllib.error.URLError("private diagnostic with test-only-placeholder")
        with mock.patch.object(release.urllib.request, "urlopen", side_effect=error):
            with self.assertRaisesRegex(release.ReleaseError, "network error") as caught:
                self.api.request("GET", "/releases")
        self.assertNotIn("test-only-placeholder", str(caught.exception))


try:
    import yaml
except ImportError:
    yaml = None


@unittest.skipIf(yaml is None, "Install PyYAML to validate workflow contracts")
class WorkflowContractTests(unittest.TestCase):
    def workflow(self, name):
        return yaml.load((ROOT / "workflows" / name).read_text(encoding="utf-8"), Loader=yaml.BaseLoader)

    def test_windows_msvc_workers_share_a_clean_dependency_cache(self):
        script = "windows/Invoke-MsvcBuild.ps1"
        self.assertTrue((ROOT / script).is_file())
        keys = set()
        for name, target in (("ccpp_win.yml", "RelWithDebInfo"), ("ccpp_win_rc.yml", "Release"),
                             ("ccpp_win_debug.yml", "Debug")):
            with self.subTest(workflow=name):
                workflow = self.workflow(name)
                for job, build_target in (("build_dep", "Dependencies"), ("build", target)):
                    steps = workflow["jobs"][job]["steps"]
                    by_name = {step.get("name"): step for step in steps}
                    cache = by_name["dep cache"]
                    keys.add(cache["with"]["key"])
                    self.assertIn("hashFiles(", cache["with"]["key"])
                    self.assertIn(f".github/{script}", cache["with"]["key"])
                    self.assertEqual("deps/build/destdir", cache["with"]["path"])
                    self.assertNotIn("restore-keys", cache["with"])
                    self.assertFalse(any("setup-perl" in step.get("uses", "") for step in steps))
                    checkout = next(step for step in steps if step.get("uses", "").startswith("actions/checkout@"))
                    self.assertNotEqual("actions/checkout@v3", checkout["uses"])
                    if job == "build_dep":
                        build = by_name["Build MSVC dependencies"]
                        self.assertEqual("actions/cache@v4", cache["uses"])
                        self.assertIn("cache-hit != 'true'", build["if"])
                    else:
                        build = by_name["Configure and build SuperSlicer"]
                        self.assertEqual("actions/cache/restore@v4", cache["uses"])
                        self.assertEqual("true", cache["with"]["fail-on-cache-miss"])
                    self.assertLess(steps.index(cache), steps.index(build))
                    self.assertEqual("powershell", build["shell"])
                    self.assertEqual(f"./.github/{script} -Target {build_target}", build["run"])
        self.assertEqual(1, len(keys))

    def test_windows_debug_keeps_symbols_and_uploads_only_a_ci_artifact(self):
        workflow = self.workflow("ccpp_win_debug.yml")
        self.assertEqual({"push": {"branches": ["debug_win"]}}, workflow["on"])
        self.assertEqual({"contents": "read"}, workflow["permissions"])
        self.assertEqual({"build_dep", "build"}, set(workflow["jobs"]))
        job = workflow["jobs"]["build"]
        self.assertEqual("build_dep", job["needs"])
        steps = job["steps"]
        commands = "\n".join(step.get("run", "") for step in steps)
        self.assertIn(r"src\Debug\*.pdb", commands)
        self.assertIn(r"package\mesa", commands)
        self.assertIn("/P:Configuration=Debug gettext_po_to_mo.vcxproj", commands)
        self.assertIn("/P:Configuration=Debug gettext_make_pot.vcxproj", commands)
        self.assertNotIn("system32", commands.lower())
        self.assertFalse(any("signpath" in step.get("uses", "").lower() for step in steps))
        upload = next(step for step in steps if step.get("uses", "").startswith("actions/upload-artifact@"))
        self.assertEqual("build/package/", upload["with"]["path"])
        self.assertEqual("error", upload["with"]["if-no-files-found"])

    def test_single_rc_trigger_and_pinned_checkouts(self):
        master = self.workflow("release_rc.yml")
        self.assertEqual(["rc"], master["on"]["push"]["branches"])
        self.assertEqual("false", master["concurrency"]["cancel-in-progress"])
        for name in ("ccpp_win_rc.yml", "ccpp_mac_rc.yml", "ccpp_mac_arm_rc.yml", "ccpp_ubuntu_gtk3_rc.yml", "ccpp_ubuntu_gtk2_rc.yml"):
            workflow = self.workflow(name)
            self.assertIn("workflow_call", workflow["on"])
            self.assertNotIn("rc", (workflow["on"].get("push") or {}).get("branches", []))
            for job_name, job in workflow["jobs"].items():
                if job_name == "publish":
                    self.assertIn("artifact_id", job["with"])
                    continue
                permissions = job.get("permissions", workflow.get("permissions"))
                self.assertEqual("read", permissions["contents"])
                for step in job.get("steps", []):
                    if step.get("uses", "").startswith("actions/checkout"):
                        self.assertEqual("${{ inputs.source_sha || github.sha }}", step["with"]["ref"])
                        self.assertEqual("false", step["with"]["persist-credentials"])

    def test_windows_publication_requires_production_signing(self):
        workflow = self.workflow("ccpp_win_rc.yml")
        steps = workflow["jobs"]["build_dep"]["steps"]
        guard = next(step for step in steps if step.get("name") == "Require production signing for release assets")
        self.assertIn("inputs.release_id != ''", guard["if"])
        self.assertIn("env.SIGNPATH_ENABLED != 'true'", guard["if"])
        self.assertIn("throw", guard["run"])
        build = workflow["jobs"]["build"]["steps"]
        names = [step.get("name") for step in build]
        self.assertLess(names.index("verify and replace signed msi"), names.index("Stage signed release files and integrity manifest"))
        version = next(step["run"] for step in build if step.get("name") == "Generate installer version")
        self.assertNotIn("-replace", version)

    def test_rc_signing_opt_in_truth_table(self):
        workflow = self.workflow("ccpp_win_rc.yml")
        expression = workflow["env"]["SIGNPATH_ENABLED"][3:-2].replace("&&", " and ").replace("||", " or ")
        for enabled in ("false", "true"):
            for event in ("push", "workflow_dispatch"):
                for sign in (False, True):
                    for release_id in ("", "42"):
                        result = eval(expression, {"__builtins__": {}}, {
                            "vars": types.SimpleNamespace(SIGNPATH_ENABLED=enabled),
                            "github": types.SimpleNamespace(event_name=event),
                            "inputs": types.SimpleNamespace(sign=sign, release_id=release_id),
                        })
                        expected = enabled == "true" and (release_id != "" or (event == "workflow_dispatch" and sign))
                        self.assertEqual("true" if expected else "false", result)

    def test_nightly_still_manual_test_only_and_dated(self):
        nightly = self.workflow("ccpp_win.yml")
        expression = nightly["env"]["SIGNPATH_ENABLED"][3:-2].replace("&&", " and ").replace("||", " or ")
        for event in ("push", "workflow_dispatch"):
            for sign in (False, True):
                result = eval(expression, {"__builtins__": {}}, {
                    "github": types.SimpleNamespace(event_name=event),
                    "inputs": types.SimpleNamespace(sign=sign),
                })
                self.assertEqual("true" if event == "workflow_dispatch" and sign else "false", result)
        commands = "\n".join(step.get("run", "") for step in nightly["jobs"]["build"]["steps"])
        self.assertIn(".github/scripts/nightly_version.py", commands)
        self.assertIn("+UNKNOWN", (ROOT / "scripts/nightly_version.py").read_text(encoding="utf-8"))
        self.assertEqual("${{ vars.SIGNPATH_TEST_SIGNING_POLICY_SLUG }}", nightly["env"]["SIGNPATH_SIGNING_POLICY_SLUG"])

    def test_publisher_uses_exact_artifact_id_and_separate_write_permissions(self):
        publisher = self.workflow("release_upload.yml")
        job = publisher["jobs"]["upload"]
        self.assertEqual("write", job["permissions"]["contents"])
        download = next(step for step in job["steps"] if step.get("uses", "").startswith("actions/download-artifact"))
        self.assertEqual("${{ inputs.artifact_id }}", download["with"]["artifact-ids"])
        self.assertNotIn("pattern", download["with"])

    def test_summary_can_read_drafts_but_uses_read_only_command(self):
        summary = self.workflow("release_rc.yml")["jobs"]["summary"]
        self.assertEqual("write", summary["permissions"]["contents"])
        commands = [step["run"] for step in summary["steps"] if "run" in step]
        self.assertEqual(1, len(commands))
        self.assertIn("release.py verify", commands[0])


if __name__ == "__main__":
    unittest.main()
