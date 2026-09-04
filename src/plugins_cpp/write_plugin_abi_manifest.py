"""Build version.ini from immutable ABI metadata; never import/load plugin code."""
from __future__ import annotations
import argparse
import configparser
from pathlib import Path
import re

BEGIN = b"SLIC3R_ABI_MANIFEST_V1_BEGIN\n"
END = b"SLIC3R_ABI_MANIFEST_V1_END\n\0"
MAX_RECORD = 1024 * 1024

def read_binary_abi(path: Path) -> dict[str, tuple[int, int]]:
    data = path.read_bytes()
    if data.count(BEGIN) != 1:
        raise ValueError("Expected exactly one ABI metadata record")
    start = data.index(BEGIN) + len(BEGIN)
    stop = data.find(END, start, start + MAX_RECORD)
    if stop < 0:
        raise ValueError("Truncated or oversized ABI metadata record")
    result = {}
    for line in data[start:stop].decode("ascii").splitlines():
        match = re.fullmatch(r"([A-Za-z0-9_/]+\.h)=([0-9]+)[uUlL]*\.([0-9]+)[uUlL]*", line)
        if not match:
            raise ValueError(f"Invalid ABI record entry: {line}")
        name, major, minor = match.groups()
        if name in result or max(int(major), int(minor)) > 65535:
            raise ValueError(f"Duplicate or invalid ABI entry: {name}")
        result[name] = (int(major), int(minor))
    return result

def read_ini_abi(path: Path) -> dict[str, tuple[int, int]]:
    config = configparser.ConfigParser(interpolation=None)
    config.optionxform = str
    with path.open(encoding="utf-8") as stream:
        config.read_file(stream)
    result = {}
    for name, value in config["abi"].items():
        if not re.fullmatch(r"[0-9]+\.[0-9]+", value):
            raise ValueError(f"Invalid ABI version: {name}")
        version = tuple(map(int, value.split(".")))
        if max(version) > 65535:
            raise ValueError(f"ABI version out of range: {name}")
        result[name] = version
    return result

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, type=Path)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--abi", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    versions = read_binary_abi(args.binary) if args.binary else {}
    if args.abi:
        for name, version in read_ini_abi(args.abi).items():
            old = versions.get(name)
            if old and old != (0, 0) and version != (0, 0) and old[0] != version[0]:
                raise ValueError(f"Conflicting ABI major: {name}")
            versions[name] = max(old or (0, 0), version)
    if versions.get("slic3r_plugin_types.h", (0, 0))[0] == 0:
        raise ValueError("Missing mandatory vtable ABI")
    config = configparser.ConfigParser(interpolation=None)
    config.optionxform = str
    with args.base.open(encoding="utf-8") as stream:
        config.read_file(stream)
    config["abi"] = {name: f"{v[0]}.{v[1]}" for name, v in sorted(versions.items()) if v != (0, 0)}
    # Write only once every input has been validated, then atomically publish.
    temporary = args.output.with_suffix(args.output.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        config.write(stream)
    temporary.replace(args.output)

if __name__ == "__main__":
    main()
