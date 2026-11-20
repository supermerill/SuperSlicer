"""Hand a compiled Windows package from the build job to the packaging job.

The build job records the unsigned package folder in windows-build.json. The packaging
job checks out the same commit, recomputes the expected identity from version.inc,
verifies every transferred file and renders the WiX source from the repository
template. It never compiles a WiX source or runs a script taken from the artifact.
"""

import argparse
from datetime import date
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys
from xml.sax.saxutils import escape

import nightly_version
import release

MANIFEST = "windows-build.json"
CONFIGURATIONS = {"rc": "Release", "nightly": "RelWithDebInfo"}
FIELDS = {"schema", "source_sha", "mode", "configuration", "product", "product_version", "binary_version",
          "nightly_date", "msi_build_name", "installer_display_suffix", "files"}
# The only CMake placeholders of src/platform/msw/winInstaller.wxs.in, with their allowed values.
WIX_PLACEHOLDERS = {
    "SLIC3R_APP_KEY": r"[A-Za-z0-9_-]+",
    "SLIC3R_APP_NAME": r"[A-Za-z0-9 ._-]+",
    "SLIC3R_APP_CMD": r"[A-Za-z0-9_.-]+",
    "SLIC3R_APP_PROG_ID": r"[A-Za-z0-9_.]+",
    "SLIC3R_GUID": r"[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}",
}


class PackageError(Exception):
    pass


def literal(content, name):
    matches = re.findall(rf'^\s*set\(\s*{name}\s+"([^"\r\n]*)"\s*\)', content, re.MULTILINE)
    if len(matches) != 1:
        raise PackageError(f"Expected exactly one literal {name}")
    return matches[0]


def numeric_version(content):
    """The four-part MSI ProductVersion, read from SLIC3R_RC_VERSION_DOTS like the old RC workflow."""
    numeric = literal(content, "SLIC3R_RC_VERSION_DOTS")
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+", numeric):
        raise PackageError("Expected a four-part numeric SLIC3R_RC_VERSION_DOTS")
    parts = tuple(int(part) for part in numeric.split("."))
    # MSI uses the first three fields; VERSIONINFO's fourth field is a WORD.
    if any(value > limit for value, limit in zip(parts, (255, 255, 65535, 65535))):
        raise PackageError("Numeric version exceeds Windows MSI/resource limits")
    resource = literal(content, "SLIC3R_RC_VERSION")
    if (not re.fullmatch(r"[0-9]+(?:\s*,\s*[0-9]+){3}", resource)
            or tuple(int(part.strip()) for part in resource.split(",")) != parts):
        raise PackageError("SLIC3R_RC_VERSION and SLIC3R_RC_VERSION_DOTS disagree")
    return numeric


def identity(version_file, mode, nightly_date=None):
    """Expected names and versions of a build of this commit in the given mode."""
    if mode not in CONFIGURATIONS:
        raise PackageError(f"Unknown Windows build mode: {mode}")
    if (mode == "nightly") != (nightly_date is not None):
        raise PackageError("A nightly build needs its build date; an RC build has none")
    try:
        product, version = release.read_version(version_file)
    except release.ReleaseError as error:
        raise PackageError(str(error)) from None
    content = Path(version_file).read_text(encoding="utf-8-sig")
    if mode == "rc":
        numeric = numeric_version(content)
        binary, msi, suffix = f"{product}_{version}", f"{product}_{numeric}.msi", ""
    else:
        try:
            _, values = nightly_version.prepare(content, nightly_date)
        except nightly_version.NightlyVersionError as error:
            raise PackageError(str(error)) from None
        numeric, msi, suffix = values["InstallerVersion"], values["MSI_FILE_NAME"], values["ReleaseSuffix"]
        # Same SLIC3R_BUILD_ID as version.inc: <app key>_<numeric><dated suffix>.
        binary = msi[:-len(".msi")]
    return {"mode": mode, "configuration": CONFIGURATIONS[mode], "product": product,
            "product_version": numeric, "binary_version": binary,
            "nightly_date": nightly_date.isoformat() if nightly_date else None,
            "msi_build_name": msi, "installer_display_suffix": suffix}


def head_sha():
    return subprocess.run(["git", "rev-parse", "HEAD"], check=True, capture_output=True, text=True).stdout.strip()


def require_checkout(source_sha):
    if not re.fullmatch(r"[0-9a-f]{40}", source_sha):
        raise PackageError("Expected a complete, lowercase commit SHA")
    if head_sha() != source_sha:
        raise PackageError("The checked-out commit is not the requested source commit")


def is_link(path):
    """Symbolic link, or any other Windows reparse point such as a directory junction."""
    return path.is_symlink() or bool(getattr(os.lstat(path), "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT)


def package_files(package):
    """Relative path, size and SHA-256 of every file in the package folder."""
    if not package.is_dir() or is_link(package):
        raise PackageError("Windows package directory is missing")
    root = package.resolve()
    entries = {}
    # Sorted: a linked directory is refused before anything below it is read.
    for path in sorted(package.rglob("*")):
        if is_link(path) or not path.resolve().is_relative_to(root):
            raise PackageError(f"Unexpected link in the Windows package: {path.name}")
        if path.is_dir():
            continue
        if not path.is_file():
            raise PackageError(f"Unexpected special file in the Windows package: {path.name}")
        relative = path.relative_to(package).as_posix()
        if relative.casefold() in entries:
            raise PackageError(f"Duplicate path in the Windows package: {relative}")
        entries[relative.casefold()] = {"path": relative, "size": path.stat().st_size, "sha256": release.sha256(path)}
    if not entries:
        raise PackageError("Windows package is empty")
    return sorted(entries.values(), key=lambda entry: entry["path"])


def write_manifest(build, version_file, mode, source_sha, nightly_date=None):
    require_checkout(source_sha)
    manifest = {"schema": 1, "source_sha": source_sha, **identity(version_file, mode, nightly_date),
                "files": package_files(build / "package")}
    (build / MANIFEST).write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def valid_relative_path(path):
    # A normalized relative POSIX path: no drive, root, backslash, empty, "." or ".." segment.
    if not isinstance(path, str) or not path or "\\" in path or ":" in path or path.startswith("/"):
        return False
    parts = PurePosixPath(path).parts
    return ".." not in parts and PurePosixPath(*parts).as_posix() == path


def verify(build, version_file, mode, source_sha, product=None, release_version=None):
    """Check a downloaded build artifact against this checkout before signing or packaging it."""
    require_checkout(source_sha)
    if {path.name for path in build.iterdir()} != {"package", MANIFEST}:
        raise PackageError("Unexpected or missing files in the Windows build artifact")
    manifest_path = build / MANIFEST
    if is_link(manifest_path) or not manifest_path.is_file():
        raise PackageError("The Windows build manifest is not a regular file")
    try:
        manifest = json.loads((build / MANIFEST).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        raise PackageError("Unreadable Windows build manifest") from None
    if not isinstance(manifest, dict) or set(manifest) != FIELDS or type(manifest["schema"]) is not int or manifest["schema"] != 1:
        raise PackageError("Unexpected Windows build manifest format")
    if manifest["source_sha"] != source_sha:
        raise PackageError("The Windows build artifact comes from another commit")
    day = None
    if mode == "nightly":
        try:
            day = nightly_version.parse_date(manifest["nightly_date"])
        except (argparse.ArgumentTypeError, TypeError):
            raise PackageError("Invalid nightly build date in the Windows build manifest") from None
    expected = identity(version_file, mode, day)
    for key, value in expected.items():
        if manifest[key] != value:
            raise PackageError(f"Windows build manifest field {key} does not match this commit and mode")
    if product is not None and product != expected["product"]:
        raise PackageError("The requested release product does not match version.inc")
    if release_version is not None and release_version != release.read_version(version_file)[1]:
        raise PackageError("The requested release version does not match version.inc")

    files = manifest["files"]
    if not isinstance(files, list) or any(not isinstance(entry, dict) or set(entry) != {"path", "size", "sha256"}
                                          or not valid_relative_path(entry["path"]) for entry in files):
        raise PackageError("Invalid file entry in the Windows build manifest")
    recorded = {entry["path"].casefold(): entry for entry in files}
    if len(recorded) != len(files):
        raise PackageError("Duplicate file entry in the Windows build manifest")
    actual = {entry["path"].casefold(): entry for entry in package_files(build / "package")}
    if recorded.keys() != actual.keys():
        raise PackageError("Missing or unexpected files in the Windows build artifact")
    for key, entry in recorded.items():
        if entry != actual[key]:
            raise PackageError(f"Windows build artifact file does not match the manifest: {entry['path']}")
    return expected


def render_wxs(template, version_content):
    """Fill the five identity placeholders of the WiX template; any other placeholder is an error."""
    values = {}
    for name, pattern in WIX_PLACEHOLDERS.items():
        value = literal(version_content, name)
        if not re.fullmatch(pattern, value):
            raise PackageError(f"Unexpected {name} value for the installer")
        values[name] = escape(value, {'"': "&quot;"})

    def substitute(match):
        if match.group(1) not in values:
            raise PackageError(f"Unexpected placeholder in the installer template: {match.group(1)}")
        return values[match.group(1)]

    return re.sub(r"@([A-Za-z0-9_]+)@", substitute, template)


def write_outputs(path, values):
    with open(path, "a", encoding="utf-8") as output:
        for key, value in values.items():
            output.write(f"{key}={value}\n")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("manifest", "verify", "wxs"))
    parser.add_argument("--build-directory", type=Path, default=Path("build"))
    parser.add_argument("--version-file", type=Path, default=Path("version.inc"))
    parser.add_argument("--mode", choices=tuple(CONFIGURATIONS))
    parser.add_argument("--source-sha")
    parser.add_argument("--nightly-date", type=nightly_version.parse_date)
    parser.add_argument("--product")
    parser.add_argument("--release-version")
    parser.add_argument("--template", type=Path, default=Path("src/platform/msw/winInstaller.wxs.in"))
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    if args.command in ("manifest", "verify") and (not args.mode or not args.source_sha):
        parser.error("--mode and --source-sha are required")
    if args.command == "manifest":
        manifest = write_manifest(args.build_directory, args.version_file, args.mode, args.source_sha, args.nightly_date)
        print(f"Recorded {len(manifest['files'])} package files for {manifest['msi_build_name']}")
    elif args.command == "verify":
        expected = verify(args.build_directory, args.version_file, args.mode, args.source_sha,
                          args.product, args.release_version)
        print(f"Verified the Windows build artifact for {expected['msi_build_name']}")
        if os.environ.get("GITHUB_ENV"):
            launcher = literal(args.version_file.read_text(encoding="utf-8-sig"), "SLIC3R_APP_CMD")
            if not re.fullmatch(WIX_PLACEHOLDERS["SLIC3R_APP_CMD"], launcher):
                raise PackageError("Unexpected SLIC3R_APP_CMD value for the launcher")
            write_outputs(os.environ["GITHUB_ENV"], {
                "InstallerVersion": expected["product_version"], "ReleaseSuffix": expected["installer_display_suffix"],
                "MSI_FILE_NAME": expected["msi_build_name"], "BINARY_VERSION": expected["binary_version"],
                "LAUNCHER": launcher + ".exe"})
    else:
        if not args.output:
            parser.error("--output is required")
        rendered = render_wxs(args.template.read_text(encoding="utf-8"), args.version_file.read_text(encoding="utf-8-sig"))
        args.output.write_text(rendered, encoding="utf-8")
        print(f"Rendered {args.output} from {args.template}")


if __name__ == "__main__":
    try:
        main()
    except (PackageError, OSError, ValueError, subprocess.CalledProcessError) as error:
        message = str(error).replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
        print(f"::error::{message}")
        sys.exit(1)
