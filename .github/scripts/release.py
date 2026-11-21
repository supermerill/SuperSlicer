"""Prepare/resume draft RC releases and publish exact CI outputs. No local PAT files.

prepare runs only for the push that creates a version tag; it never creates, moves or
deletes a tag. Only prepare/upload/verify access GitHub; stage is local. Unit tests use
a fake API. All commands fail closed on a moved tag or a published release.
"""

import argparse
import hashlib
import json
import mimetypes
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
import zipfile


PLATFORMS = ("windows", "macos-intel", "macos-arm", "linux-gtk3", "linux-gtk2")
MANIFEST = "release-manifest.json"
FOOTER_START = "<!-- superslicer-release-footer:start -->"
FOOTER_END = "<!-- superslicer-release-footer:end -->"
VERSION_RE = re.compile(r"\d+\.\d+\.\d+\.\d+(?:-[A-Za-z0-9]+(?:[.-][A-Za-z0-9]+)*)?(?:\+[A-Za-z0-9]+(?:[.-][A-Za-z0-9]+)*)?")
# Release tags: four numeric fields and an optional suffix, no "v" prefix and no "+..." metadata.
TAG_RE = re.compile(r"[0-9]+(?:[.][0-9]+){3}(?:-[A-Za-z0-9]+(?:[.-][A-Za-z0-9]+)*)?")
# Jobs of release_rc.yml for each platform, in pipeline order, for the summary.
PLATFORM_JOBS = {
    "windows": ("windows_signing", "windows_build", "windows_package", "publish_windows"),
    "macos-intel": ("macos_intel", "publish_macos_intel"),
    "macos-arm": ("macos_arm", "publish_macos_arm"),
    "linux-gtk3": ("linux_gtk3", "publish_linux_gtk3"),
    "linux-gtk2": ("linux_gtk2", "publish_linux_gtk2"),
}


class ReleaseError(Exception):
    pass


class ApiError(ReleaseError):
    def __init__(self, status, method, path):
        self.status = status
        # Do not print request headers, tokens or response bodies.
        super().__init__(f"GitHub {method} {path}: HTTP {status}")


def annotation(level, message):
    text = str(message).replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    print(f"::{level}::{text}")


def validate_identity(product, version, sha):
    if not re.fullmatch(r"[A-Za-z0-9_-]+", product):
        raise ReleaseError("Invalid product name")
    if not VERSION_RE.fullmatch(version) or "UNKNOWN" in version:
        raise ReleaseError("Invalid RC version (expected numeric version and optional suffix, without UNKNOWN)")
    if not re.fullmatch(r"[0-9a-f]{40}", sha):
        raise ReleaseError("Expected a complete, lowercase commit SHA")


def read_version(path):
    content = Path(path).read_text(encoding="utf-8-sig")

    def get(name):
        matches = re.findall(rf'^\s*set\(\s*{name}\s+"([^"\r\n]*)"\s*\)', content, re.MULTILINE)
        if len(matches) != 1:
            raise ReleaseError(f"Expected exactly one literal {name} in {path}")
        return matches[0]

    product = get("SLIC3R_APP_KEY")
    version = get("SLIC3R_RC_VERSION_DOTS") + get("SLIC3R_VERSION_TAG").replace("+UNKNOWN", "")
    validate_identity(product, version, "0" * 40)
    return product, version


def asset_names(product, version):
    validate_identity(product, version, "0" * 40)
    return {
        "windows": [f"{product}_{version}_win64.zip", f"{product}_{version}_win64.msi"],
        "macos-intel": [f"{product}_{version}_macos.dmg"],
        "macos-arm": [f"{product}_{version}_macos_arm.dmg"],
        "linux-gtk3": [f"{product}-ubuntu_22.04-{version}.AppImage", f"{product}_{version}_linux64.tgz"],
        "linux-gtk2": [f"{product}-ubuntu_22.04-gtk2-{version}.AppImage"],
    }


class GitHub:
    def __init__(self, repository, token):
        if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
            raise ReleaseError("Invalid GitHub repository")
        if not token:
            raise ReleaseError("GH_TOKEN is required")
        self.repository = repository
        self.token = token

    def request(self, method, path, data=None, file=None):
        base = "https://uploads.github.com" if file is not None else "https://api.github.com"
        headers = {
            "Authorization": f"Bearer {self.token}",
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
            "User-Agent": "SuperSlicer-RC-release",
        }
        payload = None
        if file is not None:
            # Stream large DMG/AppImage payloads instead of reading them into memory.
            headers["Content-Type"] = mimetypes.guess_type(file.name)[0] or "application/octet-stream"
            headers["Content-Length"] = str(file.stat().st_size)
            payload = file.open("rb")
        elif data is not None:
            headers["Content-Type"] = "application/json"
            payload = json.dumps(data).encode("utf-8")
        request = urllib.request.Request(base + f"/repos/{self.repository}" + path,
                                         data=payload, headers=headers, method=method)
        try:
            with urllib.request.urlopen(request, timeout=300) as response:
                raw = response.read()
                return json.loads(raw) if raw else None
        except urllib.error.HTTPError as error:
            raise ApiError(error.code, method, path) from None
        except urllib.error.URLError:
            raise ReleaseError(f"GitHub {method} {path}: network error") from None
        finally:
            if file is not None:
                payload.close()

    def pages(self, path):
        for page in range(1, 10001):
            batch = self.request("GET", f"{path}?per_page=100&page={page}")
            if not isinstance(batch, list):
                raise ReleaseError(f"Expected a GitHub list at {path}")
            yield from batch
            if len(batch) < 100:
                return
        raise ReleaseError("GitHub pagination limit exceeded")

    def tag_sha(self, tag):
        obj = self.request("GET", "/git/ref/tags/" + urllib.parse.quote(tag, safe=""))["object"]
        visited = set()
        while obj["type"] == "tag":
            if obj["sha"] in visited or len(visited) >= 16:
                raise ReleaseError("Annotated tag cycle or excessive nesting")
            visited.add(obj["sha"])
            obj = self.request("GET", f'/git/tags/{obj["sha"]}')["object"]
        if obj["type"] != "commit":
            raise ReleaseError("Release tag does not identify a commit")
        return obj["sha"]

    def find_release(self, tag):
        # Listing authenticated releases includes drafts; do not rely on /releases/tags.
        matches = [release for release in self.pages("/releases") if release["tag_name"] == tag]
        if len(matches) > 1:
            raise ReleaseError(f"Multiple releases for tag {tag}")
        return matches[0] if matches else None

    def assets(self, release_id):
        result = {}
        for asset in self.pages(f"/releases/{release_id}/assets"):
            if asset["name"] in result:
                raise ReleaseError(f'Duplicate release asset: {asset["name"]}')
            result[asset["name"]] = asset
        return result


def require_draft(release, tag):
    if release["tag_name"] != tag:
        raise ReleaseError("Release ID does not belong to the expected tag")
    if not release.get("draft"):
        raise ReleaseError(f"Release {tag} is already public; refusing to modify it")


def require_tag(api, tag, sha):
    try:
        actual = api.tag_sha(tag)
    except ApiError as error:
        if error.status != 404:
            raise
        # Never create it: POST /releases would also create a missing tag implicitly.
        raise ReleaseError(f"Tag {tag} does not exist (or was deleted); push the version tag, never let CI create it") from None
    if actual != sha:
        raise ReleaseError(f"Tag {tag} points to a different commit; increment the version, never move the tag")


def admit_tag_push(environ, event):
    """Return the version tag created by this push, or fail before any GitHub access."""
    if environ.get("GITHUB_EVENT_NAME") != "push":
        raise ReleaseError("The release orchestrator only runs for a pushed version tag")
    ref = environ.get("GITHUB_REF", "")
    tag = ref[len("refs/tags/"):] if ref.startswith("refs/tags/") else ""
    if not tag or environ.get("GITHUB_REF_NAME") != tag or not TAG_RE.fullmatch(tag):
        raise ReleaseError(f"Not a release tag: {ref!r} (expected refs/tags/<n.n.n.n[-suffix]>)")
    if not isinstance(event, dict) or event.get("ref") != ref:
        raise ReleaseError("The push event does not match the workflow ref")
    if event.get("created") is not True or event.get("deleted") is not False or event.get("forced") is not False:
        raise ReleaseError("Only the push that creates a tag can prepare a release (not a deletion or forced update)")
    return tag


def resolve_commit(revision):
    result = subprocess.run(["git", "rev-parse", "--verify", "--quiet", f"{revision}^{{commit}}"],
                            capture_output=True, text=True)
    sha = result.stdout.strip()
    if result.returncode != 0 or not re.fullmatch(r"[0-9a-f]{40}", sha):
        raise ReleaseError("Cannot resolve the tagged revision to a commit in this checkout")
    return sha


def usable_asset(asset):
    return asset.get("state") == "uploaded" and asset.get("size", 0) > 0


def inspect_assets(api, release_id, names):
    assets = api.assets(release_id)
    expected = {name for files in names.values() for name in files}
    for name in expected & assets.keys():
        if not usable_asset(assets[name]):
            raise ReleaseError(f"Incomplete/empty asset {name}; remove that failed upload manually before retrying")
    return assets


def managed_body(body, footer, repository, tag):
    policy_url = f"https://github.com/{repository}/blob/{urllib.parse.quote(tag, safe='')}/doc/Code%20signing%20policy.md"
    footer = footer.replace("{{CODE_SIGNING_POLICY_URL}}", policy_url).strip()
    if not footer:
        raise ReleaseError("Release footer must not be empty")
    if FOOTER_START in footer or FOOTER_END in footer:
        raise ReleaseError("Footer template must not contain managed delimiters")
    block = f"{FOOTER_START}\n{footer}\n{FOOTER_END}"
    body = body or ""
    if body.count(FOOTER_START) != body.count(FOOTER_END) or body.count(FOOTER_START) > 1:
        raise ReleaseError("Malformed managed footer in existing draft; preserve notes and repair manually")
    if FOOTER_START in body:
        start, end = body.index(FOOTER_START), body.index(FOOTER_END)
        if end < start:
            raise ReleaseError("Malformed managed footer order")
        return body[:start] + block + body[end + len(FOOTER_END):]
    return body.rstrip() + ("\n\n" if body.strip() else "") + block + "\n"


def prepare(api, product, version, sha, footer):
    validate_identity(product, version, sha)
    tag = version
    # Check published releases before creating anything or modifying notes.
    release = api.find_release(tag)
    if release:
        require_draft(release, tag)
    # Checked right before the draft creation: POST /releases would recreate a deleted tag.
    require_tag(api, tag, sha)
    if not release:
        try:
            release = api.request("POST", "/releases", {
                "tag_name": tag, "target_commitish": sha, "name": version, "draft": True,
                "prerelease": "-" in version.split("+", 1)[0], "body": managed_body("", footer, api.repository, tag),
            })
        except ApiError as conflict:
            if conflict.status != 422:
                raise
            release = api.find_release(tag)
            if not release:
                raise
    require_draft(release, tag)
    require_tag(api, tag, sha)
    names = asset_names(product, version)
    assets = inspect_assets(api, release["id"], names)
    body = managed_body(release.get("body"), footer, api.repository, tag)
    if body != (release.get("body") or ""):
        # Recheck immediately before a write, including publication by a human.
        current = api.request("GET", f'/releases/{release["id"]}')
        require_draft(current, tag)
        require_tag(api, tag, sha)
        body = managed_body(current.get("body"), footer, api.repository, tag)
        api.request("PATCH", f'/releases/{release["id"]}', {"body": body})
    result = {"product": product, "version": version, "tag": tag, "source_sha": sha,
              "release_id": str(release["id"])}
    for platform, files in names.items():
        missing = [name for name in files if name not in assets]
        result[platform.replace("-", "_")] = "true" if missing else "false"
        if not missing:
            annotation("warning", f"{platform}: all release assets already present; skipping build")
        else:
            print(f"{platform}: missing {', '.join(missing)}")
    return result


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def stage(platform, product, version, sha, build, destination):
    validate_identity(product, version, sha)
    names = asset_names(product, version)[platform]
    if destination.exists() and any(destination.iterdir()):
        raise ReleaseError("Release staging directory is not empty")
    destination.mkdir(parents=True, exist_ok=True)
    if platform == "windows":
        package = build / "package"
        if not package.is_dir():
            raise ReleaseError("Windows package directory is missing")
        prefix = names[0][:-4]
        files = sorted(path for path in package.rglob("*") if path.is_file())
        if not files:
            raise ReleaseError("Windows package is empty")
        with zipfile.ZipFile(destination / names[0], "w", zipfile.ZIP_DEFLATED, allowZip64=True) as archive:
            for path in files:
                # No links/junctions to files outside the generated package.
                if path.is_symlink() or not path.resolve().is_relative_to(package.resolve()):
                    raise ReleaseError(f"Unexpected link outside Windows package: {path.name}")
                archive.write(path, prefix + "/" + path.relative_to(package).as_posix())
        numeric = version.split("-", 1)[0].split("+", 1)[0]
        sources = [(build / f"{product}_{numeric}.msi", names[1])]
    elif platform.startswith("macos-"):
        arch = "intel" if platform == "macos-intel" else "arm"
        sources = [(build / f"{product}-macOS-{arch}.dmg", names[0])]
    else:
        toolkit = "GTK3" if platform == "linux-gtk3" else "GTK2"
        sources = [(build / f"{product}-linux-x64-{toolkit}.AppImage", names[0])]
        if platform == "linux-gtk3":
            sources.append((build / f"{product}-linux-x64-GTK3.tgz", names[1]))
    for source, target in sources:
        if not source.is_file() or source.is_symlink() or source.stat().st_size == 0:
            raise ReleaseError(f"Missing or empty distribution file: {source.name}")
        shutil.copy2(source, destination / target)
    manifest = {"schema": 1, "platform": platform, "product": product, "version": version,
                "tag": version, "source_sha": sha, "files": []}
    for name in names:
        file = destination / name
        manifest["files"].append({"name": name, "size": file.stat().st_size, "sha256": sha256(file)})
    (destination / MANIFEST).write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def validate_payload(platform, product, version, sha, directory):
    validate_identity(product, version, sha)
    manifest = json.loads((directory / MANIFEST).read_text(encoding="utf-8"))
    expected = {"schema": 1, "platform": platform, "product": product,
                "version": version, "tag": version, "source_sha": sha}
    if any(manifest.get(key) != value for key, value in expected.items()):
        raise ReleaseError("CI asset manifest does not match this release/commit/platform")
    names = asset_names(product, version)[platform]
    entries = manifest.get("files", [])
    if sorted(entry["name"] for entry in entries) != sorted(names):
        raise ReleaseError("Unexpected or missing files in the CI manifest")
    if {path.name for path in directory.iterdir()} != set(names) | {MANIFEST}:
        raise ReleaseError("Unexpected or missing files in the downloaded CI artifact")
    for entry in entries:
        path = directory / entry["name"]
        if not path.is_file() or path.is_symlink() or path.stat().st_size == 0:
            raise ReleaseError(f"Missing/empty CI file: {entry['name']}")
        if path.stat().st_size != entry["size"] or sha256(path) != entry["sha256"]:
            raise ReleaseError(f"CI file hash mismatch: {entry['name']}")
    return names


def upload(api, platform, product, version, sha, release_id, directory):
    names = validate_payload(platform, product, version, sha, directory)
    expected = asset_names(product, version)
    for name in names:
        require_tag(api, version, sha)
        release = api.request("GET", f"/releases/{release_id}")
        require_draft(release, version)
        assets = inspect_assets(api, release_id, expected)
        if name in assets:
            annotation("warning", f"Asset {name} already exists; keeping it unchanged")
            continue
        file = directory / name
        require_tag(api, version, sha)
        require_draft(api.request("GET", f"/releases/{release_id}"), version)
        try:
            result = api.request("POST", f"/releases/{release_id}/assets?name=" + urllib.parse.quote(name, safe=""), file=file)
        except ApiError as conflict:
            if conflict.status != 422:
                raise
            # A concurrent uploader may have won; never delete or clobber it.
            assets = inspect_assets(api, release_id, expected)
            if name not in assets:
                raise
            annotation("warning", f"Asset {name} was uploaded concurrently; keeping it unchanged")
            continue
        if result.get("name") != name or not usable_asset(result) or result["size"] != file.stat().st_size:
            raise ReleaseError(f"Upload did not complete correctly: {name}")
        digest = result.get("digest")
        if digest and digest != "sha256:" + sha256(file):
            raise ReleaseError(f"GitHub upload digest mismatch: {name}")
        print(f"Uploaded {name}")


def job_state(platform, job_results):
    """"success", "skipped", or the first job of the platform that did not succeed."""
    results = [(job, job_results.get(job, {}).get("result", "unknown")) for job in PLATFORM_JOBS[platform]]
    if all(result == "skipped" for _, result in results):
        return "skipped"
    failed = [f"{job}: {result}" for job, result in results if result != "success"]
    return failed[0] if failed else "success"


def verify(api, product, version, sha, release_id, job_results=None):
    validate_identity(product, version, sha)
    require_tag(api, version, sha)
    release = api.request("GET", f"/releases/{release_id}")
    require_draft(release, version)
    names = asset_names(product, version)
    assets = inspect_assets(api, release_id, names)
    lines = [f"## RC release `{version}`", f"Commit: `{sha}` — release remains **draft**.", "",
             "| Platform | Assets | Job |", "| --- | --- | --- |"]
    missing = []
    for platform, files in names.items():
        absent = [name for name in files if name not in assets]
        missing.extend(absent)
        job = job_state(platform, job_results or {})
        lines.append(f"| {platform} | {'Missing: ' + ', '.join(absent) if absent else 'Complete'} | {job} |")
    summary = "\n".join(lines) + "\n"
    print(summary)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as file:
            file.write(summary)
    if missing:
        raise ReleaseError("Draft is incomplete: " + ", ".join(missing))
    return summary


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "stage", "upload", "verify"))
    parser.add_argument("--source-sha")
    parser.add_argument("--version-file", type=Path, default=Path("version.inc"))
    parser.add_argument("--footer-file", type=Path, default=Path(".github/release-footer.md"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--platform", choices=PLATFORMS)
    parser.add_argument("--product")
    parser.add_argument("--version")
    parser.add_argument("--release-id", type=int)
    parser.add_argument("--build-directory", type=Path, default=Path("build"))
    parser.add_argument("--assets-directory", type=Path, default=Path("release-assets"))
    args = parser.parse_args(argv)
    if args.command == "prepare":
        # Identity of the tag push, checked before reading anything from GitHub.
        event = json.loads(Path(os.environ.get("GITHUB_EVENT_PATH", "")).read_text(encoding="utf-8"))
        tag = admit_tag_push(os.environ, event)
        product, version = read_version(args.version_file)
        if tag != version:
            raise ReleaseError(f"Tag {tag} does not match version.inc ({version})")
        args.source_sha = resolve_commit(os.environ.get("GITHUB_SHA", ""))
        if resolve_commit("HEAD") != args.source_sha:
            raise ReleaseError("The checkout is not the tagged commit")
    else:
        if not args.source_sha:
            parser.error("--source-sha is required")
        if not args.product or not args.version:
            parser.error("--product and --version are required")
        product, version = args.product, args.version
    if args.command in ("stage", "upload") and not args.platform:
        parser.error("--platform is required")
    if args.command in ("upload", "verify") and not args.release_id:
        parser.error("--release-id is required")
    if args.command == "stage":
        stage(args.platform, product, version, args.source_sha, args.build_directory, args.assets_directory)
        return
    api = GitHub(os.environ.get("GITHUB_REPOSITORY", ""), os.environ.get("GH_TOKEN", ""))
    if args.command == "prepare":
        result = prepare(api, product, version, args.source_sha, args.footer_file.read_text(encoding="utf-8"))
        if args.output:
            with args.output.open("a", encoding="utf-8") as file:
                for key, value in result.items():
                    file.write(f"{key}={value}\n")
    elif args.command == "upload":
        upload(api, args.platform, product, version, args.source_sha, args.release_id, args.assets_directory)
    else:
        verify(api, product, version, args.source_sha, args.release_id,
               json.loads(os.environ.get("RELEASE_JOB_RESULTS", "{}")))


if __name__ == "__main__":
    try:
        main()
    except (ReleaseError, OSError, ValueError, KeyError, TypeError) as error:
        annotation("error", error)
        sys.exit(1)
