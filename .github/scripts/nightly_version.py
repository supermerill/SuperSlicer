"""Prepare one dated nightly identity without changing the numeric MSI version."""

import argparse
from datetime import date
from pathlib import Path
import re
import sys


class NightlyVersionError(Exception):
    pass


def prepare(content, build_date):
    def literal(name):
        matches = re.findall(rf'^\s*set\(\s*{name}\s+"([^"\r\n]*)"\s*\)', content, re.MULTILINE)
        if len(matches) != 1:
            raise NightlyVersionError(f"Expected exactly one literal {name}")
        return matches[0]

    product = literal("SLIC3R_APP_KEY")
    numeric = literal("SLIC3R_RC_VERSION_DOTS")
    resource_numeric = literal("SLIC3R_RC_VERSION")
    tag = literal("SLIC3R_VERSION_TAG")
    if not re.fullmatch(r"[A-Za-z0-9_-]+", product):
        raise NightlyVersionError("Invalid application key")
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+", numeric):
        raise NightlyVersionError("Expected a four-part numeric MSI version")
    parts = tuple(int(part) for part in numeric.split("."))
    # MSI uses the first three fields; VERSIONINFO's fourth field is a WORD.
    if any(value > limit for value, limit in zip(parts, (255, 255, 65535, 65535))):
        raise NightlyVersionError("Numeric version exceeds Windows MSI/resource limits")
    if (not re.fullmatch(r"[0-9]+(?:\s*,\s*[0-9]+){3}", resource_numeric) or
            tuple(int(part.strip()) for part in resource_numeric.split(",")) != parts):
        raise NightlyVersionError("SLIC3R_RC_VERSION and SLIC3R_RC_VERSION_DOTS disagree")
    if not re.fullmatch(r"(?:-[A-Za-z0-9]+(?:[.-][A-Za-z0-9]+)*)?\+UNKNOWN", tag):
        raise NightlyVersionError("Expected an optional version suffix followed by +UNKNOWN for a nightly")

    day = build_date.isoformat()
    dated_tag = tag.replace("+UNKNOWN", "-" + day)
    values = {"NightlyDate": day, "InstallerVersion": numeric, "ReleaseSuffix": "-Beta-Nightly",
              "MSI_FILE_NAME": f"{product}_{numeric}{dated_tag}.msi"}
    # Preserve the existing +UNKNOWN dating of EXE/DLL ProductVersion strings.
    return content.replace("+UNKNOWN", "-" + day), values


def parse_date(value):
    try:
        result = date.fromisoformat(value)
        if result.isoformat() == value:
            return result
    except ValueError:
        pass
    raise argparse.ArgumentTypeError("Expected a valid date in YYYY-MM-DD format")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version-file", type=Path, default=Path("version.inc"))
    parser.add_argument("--dated-version-file", type=Path, default=Path("version.date.inc"))
    parser.add_argument("--github-env", type=Path, required=True)
    parser.add_argument("--date", type=parse_date, help="Override the date for local tests")
    args = parser.parse_args(argv)
    paths = (args.version_file, args.dated_version_file, args.github_env)
    if len({path.resolve() for path in paths}) != len(paths):
        raise NightlyVersionError("Source, dated version and environment files must be distinct")
    content = args.version_file.read_text(encoding="utf-8-sig")
    # Read the clock only once, before compilation; MSI packaging may cross midnight.
    dated_content, values = prepare(content, args.date or date.today())
    args.dated_version_file.write_text(dated_content, encoding="utf-8")
    with args.github_env.open("a", encoding="utf-8") as output:
        for key, value in values.items():
            output.write(f"{key}={value}\n")
    print(f"Nightly MSI ProductVersion: {values['InstallerVersion']}")
    print(f"Nightly MSI file: {values['MSI_FILE_NAME']}")


if __name__ == "__main__":
    try:
        main()
    except (NightlyVersionError, OSError, ValueError) as error:
        message = str(error).replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
        print(f"::error::{message}")
        sys.exit(1)
