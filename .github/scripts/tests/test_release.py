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

    def add_draft(self, tag=VERSION, sha=SHA, body="Manual notes", draft=True, recorded=SHA):
        """A draft for tag at sha; recorded is the commit in its managed footer (None: older draft)."""
        self.refs[tag] = {"type": "commit", "sha": sha}
        if recorded:
            body = release.managed_body(body, FOOTER, self.repository, tag, recorded)
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

    def test_draft_created_for_the_pushed_tag_at_exact_commit(self):
        self.api.refs[VERSION] = {"type": "commit", "sha": SHA}
        result = self.prepare()
        self.assertEqual([("POST", "/releases")], [call[:2] for call in self.api.writes()])
        self.assertTrue(self.api.releases[0]["draft"])
        self.assertTrue(self.api.releases[0]["prerelease"])
        self.assertEqual(SHA, self.api.releases[0]["target_commitish"])
        self.assertEqual(VERSION, result["tag"])
        self.assertTrue(all(result[platform.replace("-", "_")] == "true" for platform in release.PLATFORMS))
        self.assertIn(f"/blob/{VERSION}/doc/", self.api.releases[0]["body"])

    def test_missing_tag_is_never_created(self):
        with self.assertRaisesRegex(release.ReleaseError, "does not exist"):
            self.prepare()
        self.assertEqual([], self.api.writes())

    def test_tag_deleted_before_draft_creation_fails_without_writes(self):
        self.api.refs[VERSION] = {"type": "commit", "sha": SHA}
        def delete_tag(method, path, data, file):
            if method == "GET" and path.startswith("/releases?"):
                self.api.refs.pop(VERSION, None)
        self.api.hook = delete_tag
        # POST /releases would recreate the tag: it must not be sent.
        with self.assertRaisesRegex(release.ReleaseError, "does not exist"):
            self.prepare()
        self.assertEqual([], self.api.writes())

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
        with self.assertRaisesRegex(release.ReleaseError, "does not exist"):
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
        result = release.managed_body(body, FOOTER, self.api.repository, VERSION, SHA)
        self.assertTrue(result.startswith("Manual prefix\n"))
        self.assertTrue(result.endswith("\nManual suffix"))
        self.assertNotIn("\nold\n", result)

    def test_prepare_preserves_notes_changed_during_asset_listing(self):
        # Without a managed footer yet, so that preparation has to write it.
        draft = self.api.add_draft(recorded=None)
        def edit_notes(method, path, data, file):
            if method == "GET" and "/assets?" in path:
                draft["body"] = "Notes edited during preparation\n"
        self.api.hook = edit_notes
        self.prepare()
        self.assertTrue(draft["body"].startswith("Notes edited during preparation\n"))
        self.assertEqual(SHA, release.draft_commit(draft))
        self.assertIn("PATCH", [call[0] for call in self.api.writes()])

    def test_prepare_rejects_publication_during_asset_listing(self):
        # Without a managed footer yet, so that preparation has to write it.
        draft = self.api.add_draft(recorded=None)
        def publish(method, path, data, file):
            if method == "GET" and "/assets?" in path:
                draft["draft"] = False
        self.api.hook = publish
        with self.assertRaisesRegex(release.ReleaseError, "already public"):
            self.prepare()
        self.assertEqual([], self.api.writes())

    def test_new_draft_records_its_commit_in_the_managed_footer(self):
        self.api.refs[VERSION] = {"type": "commit", "sha": SHA}
        self.prepare()
        self.assertEqual(SHA, release.draft_commit(self.api.releases[0]))
        self.assertIn(f"<!-- superslicer-release-commit: {SHA} -->", self.api.releases[0]["body"])

    def test_draft_left_from_a_deleted_tag_of_another_commit_is_refused(self):
        # Tag deleted and pushed again on another commit, draft not deleted.
        draft = self.api.add_draft(sha=OTHER_SHA, recorded=SHA)
        self.api.add_asset(draft, release.asset_names(PRODUCT, VERSION)["macos-intel"][0])
        with self.assertRaisesRegex(release.ReleaseError, "created for commit aaaaaaaaaaaa, not bbbbbbbbbbbb"):
            release.prepare(self.api, PRODUCT, VERSION, OTHER_SHA, FOOTER)
        self.assertEqual([], self.api.writes())

    def test_older_draft_without_commit_is_adopted_only_while_empty(self):
        draft = self.api.add_draft(body="Notes\n", recorded=None)
        self.prepare()
        self.assertEqual(SHA, release.draft_commit(draft))
        self.assertTrue(draft["body"].startswith("Notes\n"))
        other = self.api.add_draft(tag="2.7.63.1-beta", body="Notes\n", recorded=None)
        self.api.add_asset(other, release.asset_names(PRODUCT, "2.7.63.1-beta")["linux-gtk2"][0])
        writes = len(self.api.writes())
        with self.assertRaisesRegex(release.ReleaseError, "records no commit and already has release files"):
            release.prepare(self.api, PRODUCT, "2.7.63.1-beta", SHA, FOOTER)
        self.assertEqual(writes, len(self.api.writes()))

    def test_older_draft_receiving_files_during_preparation_is_not_adopted(self):
        draft = self.api.add_draft(recorded=None)
        name = release.asset_names(PRODUCT, VERSION)["linux-gtk3"][0]
        def upload_during_recheck(method, path, data, file):
            if method == "GET" and path == f'/releases/{draft["id"]}':
                self.api.add_asset(draft, name)
        self.api.hook = upload_during_recheck
        with self.assertRaisesRegex(release.ReleaseError, "records no commit"):
            self.prepare()
        self.assertEqual([], self.api.writes())

    def test_malformed_or_duplicated_commit_records_are_refused(self):
        marker = f"<!-- superslicer-release-commit: {SHA} -->"
        for body in (f"{release.FOOTER_START}\n{marker}\n{marker}\n{release.FOOTER_END}",
                     f"{release.FOOTER_START}\n<!-- superslicer-release-commit: abc -->\n{release.FOOTER_END}",
                     f"{release.FOOTER_START}\n{marker}\n{release.FOOTER_END}\n{release.FOOTER_START}\n{release.FOOTER_END}"):
            with self.subTest(body=body), self.assertRaisesRegex(release.ReleaseError, "Malformed"):
                release.draft_commit({"body": body})
        self.assertIsNone(release.draft_commit({"body": "Notes only"}))

    def test_upload_and_verify_refuse_a_draft_of_another_commit(self):
        directory = self.payload("macos-arm")
        draft = self.api.add_draft(recorded=OTHER_SHA)
        with self.assertRaisesRegex(release.ReleaseError, "created for commit"):
            release.upload(self.api, "macos-arm", PRODUCT, VERSION, SHA, draft["id"], directory)
        with self.assertRaisesRegex(release.ReleaseError, "created for commit"):
            release.verify(self.api, PRODUCT, VERSION, SHA, draft["id"])
        legacy = self.api.add_draft(tag=VERSION + ".x", recorded=None)
        self.api.releases.remove(draft)
        legacy["tag_name"] = VERSION
        with self.assertRaisesRegex(release.ReleaseError, "records no commit"):
            release.upload(self.api, "macos-arm", PRODUCT, VERSION, SHA, legacy["id"], directory)
        self.assertEqual([], self.api.writes())

    def test_malformed_footer_fails(self):
        with self.assertRaisesRegex(release.ReleaseError, "Malformed"):
            release.managed_body(release.FOOTER_START, FOOTER, self.api.repository, VERSION, SHA)

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

    def test_concurrent_release_creation_reuses_matching_draft(self):
        self.api.refs[VERSION] = {"type": "commit", "sha": SHA}
        def race(method, path, data, file):
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
                           {"windows_signing": {"result": "failure"}, "macos_arm": {"result": "success"}})
        self.assertIn("windows_signing: failure", self.output.getvalue())
        self.assertIn("macos-arm | Complete", self.output.getvalue())

    def test_verify_full_draft_does_not_publish(self):
        draft = self.api.add_draft()
        for files in release.asset_names(PRODUCT, VERSION).values():
            for name in files:
                self.api.add_asset(draft, name)
        release.verify(self.api, PRODUCT, VERSION, SHA, draft["id"])
        self.assertTrue(draft["draft"])
        self.assertEqual([], self.api.writes())

    def run_prepare_cli(self, tag, version=VERSION, event=None, environ=None, commits=None):
        """main(["prepare"]) for a tag push; GitHub is the fake API, git is mocked."""
        version_file = self.root / "version.inc"
        numeric, _, suffix = version.partition("-")
        version_file.write_text(f'set(SLIC3R_APP_KEY "SuperSlicer")\nset(SLIC3R_RC_VERSION_DOTS "{numeric}")\n'
                                f'set(SLIC3R_VERSION_TAG "{"-" + suffix if suffix else ""}+UNKNOWN")\n', encoding="utf-8")
        footer = self.root / "footer.md"
        footer.write_text(FOOTER, encoding="utf-8")
        event_file = self.root / "event.json"
        payload = {"ref": f"refs/tags/{tag}", "created": True, "deleted": False, "forced": False}
        event_file.write_text(json.dumps({**payload, **(event or {})}), encoding="utf-8")
        environment = {"GITHUB_EVENT_NAME": "push", "GITHUB_REF": f"refs/tags/{tag}", "GITHUB_REF_NAME": tag,
                       "GITHUB_SHA": SHA, "GITHUB_EVENT_PATH": str(event_file), **(environ or {})}
        resolved = {"HEAD": SHA, SHA: SHA, **(commits or {})}
        output = self.root / "output"
        output.unlink(missing_ok=True)
        with mock.patch.dict(os.environ, environment), \
                mock.patch.object(release, "resolve_commit", side_effect=lambda revision: resolved[revision]), \
                mock.patch.object(release, "GitHub", return_value=self.api):
            release.main(["prepare", "--version-file", str(version_file), "--footer-file", str(footer),
                          "--output", str(output)])
        return dict(line.split("=", 1) for line in output.read_text(encoding="utf-8").splitlines())

    def test_prepare_cli_accepts_only_the_push_creating_a_matching_tag(self):
        for tag in ("2.7.63.0", "2.7.63.0-beta", "2.7.63.0-rc.1"):
            with self.subTest(tag=tag):
                self.api.refs[tag] = {"type": "commit", "sha": SHA}
                result = self.run_prepare_cli(tag, version=tag)
                self.assertEqual((tag, SHA), (result["tag"], result["source_sha"]))
        writes = len(self.api.writes())
        rejected = {
            "not a push": dict(environ={"GITHUB_EVENT_NAME": "workflow_dispatch"}),
            "branch rc": dict(environ={"GITHUB_REF": "refs/heads/rc", "GITHUB_REF_NAME": "rc"}),
            "ref name": dict(environ={"GITHUB_REF_NAME": "2.7.63.1-beta"}),
            "deleted": dict(event={"deleted": True}),
            "forced": dict(event={"forced": True}),
            "not created": dict(event={"created": False}),
            "event ref": dict(event={"ref": "refs/tags/2.7.63.1-beta"}),
            "other version": dict(version="2.7.63.0"),
            "commit is not HEAD": dict(commits={"HEAD": OTHER_SHA}),
        }
        for name, case in rejected.items():
            with self.subTest(case=name), self.assertRaises(release.ReleaseError):
                self.run_prepare_cli(VERSION, **case)
        for tag in ("v2.7.63.0", "2.7.63", "2.7.63.0.1", "2.7.63.0+build", "2.7.63.0-", "2.7.63.0-beta/x"):
            with self.subTest(tag=tag), self.assertRaisesRegex(release.ReleaseError, "Not a release tag"):
                self.run_prepare_cli(tag)
        self.assertEqual(writes, len(self.api.writes()))

    def test_summary_reports_the_first_job_that_did_not_succeed(self):
        results = {job: {"result": "success"} for jobs in release.PLATFORM_JOBS.values() for job in jobs}
        results.update({"windows_package": {"result": "failure"}, "publish_windows": {"result": "skipped"},
                        "macos_arm": {"result": "skipped"}, "publish_macos_arm": {"result": "skipped"}})
        self.assertEqual("windows_package: failure", release.job_state("windows", results))
        self.assertEqual("skipped", release.job_state("macos-arm", results))
        self.assertEqual("success", release.job_state("linux-gtk2", results))
        self.assertEqual("macos_intel: unknown", release.job_state("macos-intel", {}))

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
        for name, target in (("windows_build.yml", "$env:CONFIGURATION"), ("ccpp_win_debug.yml", "Debug")):
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

    WORKERS = {"macos_intel": ("ccpp_mac_rc.yml", "macos-intel"), "macos_arm": ("ccpp_mac_arm_rc.yml", "macos-arm"),
               "linux_gtk3": ("ccpp_ubuntu_gtk3_rc.yml", "linux-gtk3"), "linux_gtk2": ("ccpp_ubuntu_gtk2_rc.yml", "linux-gtk2")}

    def test_release_only_on_tags_and_branch_rc_only_builds(self):
        master = self.workflow("release_rc.yml")
        self.assertEqual({"push": {"tags": ["*.*.*.*"]}}, master["on"])
        self.assertIn("github.ref_name", master["concurrency"]["group"])
        self.assertEqual("false", master["concurrency"]["cancel-in-progress"])
        branch = self.workflow("ci_rc.yml")
        self.assertEqual({"push": {"branches": ["rc"]}}, branch["on"])
        self.assertEqual({"contents": "read"}, branch["permissions"])
        self.assertNotIn("secrets", json.dumps(branch))
        self.assertNotIn("write", json.dumps(branch))
        self.assertEqual("none", branch["jobs"]["windows_package"]["with"]["signing"])
        for job, (worker, _) in self.WORKERS.items():
            self.assertEqual({"source_sha": "${{ github.sha }}"}, branch["jobs"][job]["with"])
            self.assertEqual(f"./.github/workflows/{worker}", branch["jobs"][job]["uses"])
        # No other workflow reacts to a push on rc.
        for path in (ROOT / "workflows").glob("*.yml"):
            events = self.workflow(path.name)["on"]
            push = events.get("push") if isinstance(events, dict) else None
            branches = push.get("branches", []) if isinstance(push, dict) else []
            self.assertEqual(path.name == "ci_rc.yml", "rc" in branches, path.name)

    def test_standalone_windows_rc_is_unsigned_and_not_callable(self):
        workflow = self.workflow("ccpp_win_rc.yml")
        self.assertEqual({"push": {"branches": ["debug_win"]}, "workflow_dispatch": ""}, workflow["on"])
        self.assertNotIn("secrets", json.dumps(workflow))
        self.assertNotIn("SIGNPATH", json.dumps(workflow))
        self.assertEqual("none", workflow["jobs"]["package"]["with"]["signing"])
        self.assertEqual("${{ needs.build.outputs.artifact_id }}", workflow["jobs"]["package"]["with"]["artifact_id"])

    def test_platform_workers_stage_but_never_publish(self):
        for job, (name, platform) in self.WORKERS.items():
            with self.subTest(worker=name):
                workflow = self.workflow(name)
                call = workflow["on"]["workflow_call"]
                self.assertEqual({"source_sha", "stage_release", "product", "release_version"}, set(call["inputs"]))
                self.assertEqual("false", call["inputs"]["stage_release"]["default"])
                self.assertEqual("${{ jobs.build.outputs.release_artifact_id }}", call["outputs"]["release_artifact_id"]["value"])
                self.assertNotIn("release_id", json.dumps(workflow))
                self.assertNotIn("publish", workflow["jobs"])
                for job_name, item in workflow["jobs"].items():
                    self.assertEqual("read", item.get("permissions", workflow["permissions"])["contents"])
                    for step in item["steps"]:
                        if step.get("uses", "").startswith("actions/checkout"):
                            self.assertEqual("${{ inputs.source_sha || github.sha }}", step["with"]["ref"])
                            self.assertEqual("false", step["with"]["persist-credentials"])
                        if "release.py stage" in step.get("run", "") or step.get("id") == "release-artifact":
                            self.assertEqual("inputs.stage_release", step["if"])
                stage = next(step["run"] for step in workflow["jobs"]["build"]["steps"] if "release.py stage" in step.get("run", ""))
                self.assertIn(f"--platform {platform}", stage)

    def test_release_jobs_permissions_and_exact_publishers(self):
        jobs = self.workflow("release_rc.yml")["jobs"]
        self.assertEqual({"contents": "read"}, self.workflow("release_rc.yml")["permissions"])
        self.assertNotIn("inherit", json.dumps(jobs))
        writers = {name for name, job in jobs.items() if job.get("permissions", {}).get("contents") == "write"}
        publishers = {f"publish_{key}" for key in ("windows", "macos_intel", "macos_arm", "linux_gtk3", "linux_gtk2")}
        self.assertEqual({"prepare", "summary"} | publishers, writers)
        self.assertEqual("tests", jobs["prepare"]["needs"])
        self.assertIn("PyYAML", json.dumps(jobs["tests"]))
        self.assertNotIn("--source-sha", jobs["prepare"]["steps"][-1]["run"])
        builders = {"windows": "windows_package", **{key: key for key in self.WORKERS}}
        for key, builder in builders.items():
            platform = "windows" if key == "windows" else self.WORKERS[key][1]
            publisher = jobs[f"publish_{key}"]
            self.assertEqual(["prepare", builder], publisher["needs"])
            self.assertEqual("./.github/workflows/release_upload.yml", publisher["uses"])
            self.assertEqual(platform, publisher["with"]["platform"])
            self.assertEqual(f"${{{{ needs.{builder}.outputs.release_artifact_id }}}}", publisher["with"]["artifact_id"])
            self.assertEqual("${{ needs.prepare.outputs.release_id }}", publisher["with"]["release_id"])
        for key, (worker, _) in self.WORKERS.items():
            self.assertEqual(f"needs.prepare.outputs.{key} == 'true'", jobs[key]["if"])
            self.assertEqual("true", jobs[key]["with"]["stage_release"])
            self.assertNotIn("secrets", jobs[key])
        # The summary sees every job of every platform, as release.py names them.
        expected = {"prepare"} | {job for names in release.PLATFORM_JOBS.values() for job in names}
        self.assertEqual(expected, set(jobs["summary"]["needs"]))
        self.assertEqual(set(jobs) - {"tests", "summary"}, expected)

    def test_windows_release_requires_production_signing(self):
        jobs = self.workflow("release_rc.yml")["jobs"]
        check = jobs["windows_signing"]
        self.assertEqual("needs.prepare.outputs.windows == 'true'", check["if"])
        self.assertNotIn("secrets.", json.dumps(check))
        # No checkout and no action: the check reads only public variables.
        self.assertFalse(any("uses" in step for step in check["steps"]))
        self.assertIn('"$SIGNPATH_ENABLED" != \'true\'', check["steps"][0]["run"])
        self.assertEqual(["prepare", "windows_signing"], jobs["windows_build"]["needs"])
        self.assertNotIn("secrets", jobs["windows_build"])
        package = jobs["windows_package"]
        self.assertEqual("${{ needs.windows_build.outputs.artifact_id }}", package["with"]["artifact_id"])
        self.assertEqual(("production", "true"), (package["with"]["signing"], package["with"]["stage_release"]))
        self.assertEqual({"SIGNPATH_API_TOKEN": "${{ secrets.SIGNPATH_API_TOKEN }}"}, package["secrets"])
        # The package worker itself refuses signing requested by any other caller.
        check_inputs = next(step for step in self.workflow("windows_package.yml")["jobs"]["package"]["steps"]
                            if step.get("name") == "Check inputs")
        self.assertIn('/.github/workflows/release_rc.yml@refs/tags/', check_inputs["run"])
        self.assertIn('/.github/workflows/ccpp_win.yml@', check_inputs["run"])
        self.assertEqual("${{ github.workflow_ref }}", check_inputs["env"]["WORKFLOW_REF"])

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
        commands = "\n".join(step.get("run", "") for step in self.workflow("windows_build.yml")["jobs"]["build"]["steps"])
        self.assertIn(".github/scripts/nightly_version.py", commands)
        self.assertIn("+UNKNOWN", (ROOT / "scripts/nightly_version.py").read_text(encoding="utf-8"))
        mode = next(step["run"] for step in nightly["jobs"]["signing"]["steps"] if step.get("id") == "mode")
        self.assertIn("echo test || echo none", mode)
        self.assertEqual("${{ needs.signing.outputs.mode == 'test' && secrets.SIGNPATH_API_TOKEN || '' }}",
                         nightly["jobs"]["package"]["secrets"]["SIGNPATH_API_TOKEN"])

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
