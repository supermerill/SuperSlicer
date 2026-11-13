"""Read-only preflight for WinGet updates; winget-releaser creates the external PR."""

import argparse
import os
from pathlib import Path
import re
import sys
import urllib.parse

from release import ApiError, GitHub, ReleaseError, annotation, asset_names, usable_asset


IDENTIFIER = "Supermerill.SuperSlicer"
REPOSITORY = "supermerill/SuperSlicer"
CATALOG_PATH = "/contents/manifests/s/Supermerill/SuperSlicer"


def prepare(api, catalog, tag):
    # This project's distribution policy is stable releases only. Do not strip a
    # beta suffix or date and accidentally publish it as a stable package version.
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+", tag):
        raise ReleaseError("This workflow accepts only a stable four-part release tag, e.g. 2.7.63.0; no beta/nightly suffix")
    if api.repository.lower() != REPOSITORY.lower():
        raise ReleaseError("WinGet publication is restricted to the upstream SuperSlicer repository")
    item = api.request("GET", "/releases/tags/" + urllib.parse.quote(tag, safe=""))
    if item.get("tag_name") != tag:
        raise ReleaseError("GitHub returned a different release tag")
    if item.get("draft") is not False or item.get("prerelease") is not False or not item.get("published_at"):
        raise ReleaseError("WinGet publication requires a published, non-prerelease release")

    name = asset_names("SuperSlicer", tag)["windows"][1]
    assets = api.assets(item["id"])
    if name not in assets or not usable_asset(assets[name]):
        raise ReleaseError(f"Missing or incomplete release MSI: {name}")
    expected_url = f"https://github.com/{REPOSITORY}/releases/download/{tag}/{name}"
    if assets[name].get("browser_download_url") != expected_url:
        raise ReleaseError("The MSI must be a direct asset of the selected upstream release")

    try:
        versions = catalog.request("GET", CATALOG_PATH)
    except ApiError as error:
        if error.status != 404:
            raise
        raise ReleaseError(f"{IDENTIFIER} is not in winget-pkgs yet. Submit the first version manually; see .github/winget/README.md") from None
    if not isinstance(versions, list) or not any(entry.get("type") == "dir" for entry in versions):
        raise ReleaseError("WinGet catalog contains no baseline version for this package")
    exists = any(entry.get("name") == tag and entry.get("type") == "dir" for entry in versions)
    if exists:
        annotation("warning", f"{IDENTIFIER} {tag} is already in winget-pkgs; skipping submission")
    else:
        print(f"Preparing WinGet update for {IDENTIFIER} {tag}: {name}")
    return {"version": tag, "release_tag": tag, "installers_regex": "^" + re.escape(name) + "$",
            "submit": "false" if exists else "true"}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    token = os.environ.get("GH_TOKEN", "")
    api = GitHub(os.environ.get("GITHUB_REPOSITORY", ""), token)
    catalog = GitHub("microsoft/winget-pkgs", token)
    result = prepare(api, catalog, args.tag)
    with args.output.open("a", encoding="utf-8") as output:
        for key, value in result.items():
            output.write(f"{key}={value}\n")


if __name__ == "__main__":
    try:
        main()
    except (ReleaseError, OSError, ValueError, KeyError, TypeError) as error:
        annotation("error", error)
        sys.exit(1)
