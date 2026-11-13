"""Prepare a script-only Chocolatey package from a published stable upstream MSI."""

import argparse
import json
import os
from pathlib import Path
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

from release import GitHub, ReleaseError, annotation, asset_names, sha256, usable_asset


REPOSITORY = "supermerill/SuperSlicer"
FEED = "https://community.chocolatey.org/api/v2"
TEMPLATES = Path(__file__).resolve().parents[2] / "packaging/chocolatey"


def package_version(tag):
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+", tag):
        raise ReleaseError("Chocolatey automation accepts only stable four-part release tags, without beta/nightly suffixes")
    parts = [str(int(part)) for part in tag.split(".")]
    return ".".join(parts[:3] if parts[3] == "0" else parts)


def package_exists(version):
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:\.[0-9]+)?", version):
        raise ReleaseError("Invalid Chocolatey package version")
    url = f"{FEED}/Packages(Id='superslicer',Version='{version}')"
    request = urllib.request.Request(url, headers={"User-Agent": "SuperSlicer-Chocolatey-release"})
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            root = ET.fromstring(response.read())
    except urllib.error.HTTPError as error:
        code = error.code
        error.close()
        if code == 404:
            return False
        raise ReleaseError(f"Chocolatey version lookup failed: HTTP {code}") from None
    except (urllib.error.URLError, ET.ParseError):
        raise ReleaseError("Chocolatey version lookup failed; refusing to assume the version is absent") from None
    title = root.find("{http://www.w3.org/2005/Atom}title")
    found_version = root.find(".//{http://schemas.microsoft.com/ado/2007/08/dataservices}Version")
    if title is None or title.text != "superslicer" or found_version is None or not found_version.text:
        raise ReleaseError("Unexpected Chocolatey version lookup response")
    found = found_version.text
    if found.count(".") == 2:
        found += ".0"
    if package_version(found) != version:
        raise ReleaseError("Unexpected Chocolatey version lookup response")
    return True


def validate_metadata(metadata):
    tag = metadata["source_version"]
    version = package_version(tag)
    name = asset_names("SuperSlicer", tag)["windows"][1]
    url = f"https://github.com/{REPOSITORY}/releases/download/{tag}/{name}"
    if (metadata.get("schema") != 1 or metadata.get("repository") != REPOSITORY or
            metadata.get("package_version") != version or metadata.get("msi_name") != name or metadata.get("msi_url") != url):
        raise ReleaseError("Chocolatey metadata does not match the selected upstream release")
    if type(metadata.get("size")) is not int or metadata["size"] <= 0:
        raise ReleaseError("Invalid MSI size")
    digest = metadata.get("digest")
    if digest is not None and not re.fullmatch(r"sha256:[0-9a-f]{64}", digest):
        raise ReleaseError("Invalid GitHub MSI digest")


def prepare(api, tag, exists=package_exists):
    version = package_version(tag)
    if api.repository.lower() != REPOSITORY.lower():
        raise ReleaseError("Chocolatey publication is restricted to upstream SuperSlicer")
    item = api.request("GET", "/releases/tags/" + urllib.parse.quote(tag, safe=""))
    if (item.get("tag_name") != tag or item.get("draft") is not False or
            item.get("prerelease") is not False or not item.get("published_at")):
        raise ReleaseError("Chocolatey requires the exact published, non-prerelease release")
    name = asset_names("SuperSlicer", tag)["windows"][1]
    assets = api.assets(item["id"])
    if name not in assets or not usable_asset(assets[name]):
        raise ReleaseError(f"Missing or incomplete release MSI: {name}")
    asset = assets[name]
    metadata = {"schema": 1, "repository": REPOSITORY, "source_version": tag,
                "package_version": version, "msi_name": name, "msi_url": asset.get("browser_download_url"),
                "size": asset["size"], "digest": asset.get("digest")}
    validate_metadata(metadata)
    submit = not exists(version)
    if not submit:
        annotation("warning", f"Chocolatey superslicer {version} already exists; not pushing or replacing it")
    return metadata, {"submit": "true" if submit else "false", "package_version": version,
                      "package_file": f"superslicer.{version}.nupkg"}


def stage(metadata, msi, destination, templates=TEMPLATES):
    # The Windows wrapper verifies Authenticode and MSI properties before calling
    # this local renderer. Never put the MSI itself inside the public nupkg.
    validate_metadata(metadata)
    if not msi.is_file() or msi.is_symlink() or msi.stat().st_size != metadata["size"]:
        raise ReleaseError("Downloaded MSI does not match the expected release asset size")
    digest = sha256(msi)
    if metadata.get("digest") and metadata["digest"] != "sha256:" + digest:
        raise ReleaseError("Downloaded MSI does not match the GitHub asset digest")
    if destination.exists() and any(destination.iterdir()):
        raise ReleaseError("Chocolatey staging directory is not empty")
    replacements = {"@SOURCE_VERSION@": metadata["source_version"], "@PACKAGE_VERSION@": metadata["package_version"],
                    "@MSI_URL@": metadata["msi_url"], "@SHA256@": digest.upper()}
    for relative in ("superslicer.nuspec", "tools/chocolateyInstall.ps1"):
        text = (templates / relative).read_text(encoding="utf-8-sig")
        for token, value in replacements.items():
            text = text.replace(token, value)
        if re.search(r"@[A-Z_]+@", text):
            raise ReleaseError(f"Unresolved Chocolatey template token in {relative}")
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")
    (destination / "tools/VERIFICATION.txt").write_text(
        f"Upstream release: https://github.com/{REPOSITORY}/releases/tag/{metadata['source_version']}\n"
        f"Installer: {metadata['msi_url']}\nSHA256: {digest.upper()}\n"
        "The MSI is downloaded from the upstream release, not embedded or repacked.\n", encoding="utf-8")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "stage"))
    parser.add_argument("--tag")
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--msi", type=Path)
    parser.add_argument("--destination", type=Path)
    args = parser.parse_args(argv)
    if args.command == "prepare":
        if not args.tag or not args.output:
            parser.error("prepare requires --tag and --output")
        api = GitHub(os.environ.get("GITHUB_REPOSITORY", ""), os.environ.get("GH_TOKEN", ""))
        metadata, outputs = prepare(api, args.tag)
        args.metadata.parent.mkdir(parents=True, exist_ok=True)
        args.metadata.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
        with args.output.open("a", encoding="utf-8") as output:
            for key, value in outputs.items():
                output.write(f"{key}={value}\n")
    else:
        if not args.msi or not args.destination:
            parser.error("stage requires --msi and --destination")
        stage(json.loads(args.metadata.read_text(encoding="utf-8")), args.msi, args.destination)


if __name__ == "__main__":
    try:
        main()
    except (ReleaseError, OSError, ValueError, KeyError, TypeError) as error:
        annotation("error", error)
        sys.exit(1)
