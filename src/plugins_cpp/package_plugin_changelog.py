"""Validate optional release notes before copying them into a package.

This tool never runs plugin code. Validation completes before the destination
is replaced, so packaging fails without publishing malformed notes.
"""
import argparse
import json
from pathlib import Path
import re

MAX_SIZE = 1024 * 1024
VERSION = re.compile(r"[0-9]+(?:\.[0-9]+){1,3}(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?\Z")


def unique_object(pairs):
    """Reject duplicate keys instead of accepting json's last-value wins rule."""
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate changelog key: {key}")
        result[key] = value
    return result


def validate(contents):
    """Validate the full version table, including versions not built today."""
    if len(contents) > MAX_SIZE:
        raise ValueError("Changelog exceeds the 1 MiB limit")
    document = json.loads(contents.decode("utf-8-sig"), object_pairs_hook=unique_object)
    if (not isinstance(document, dict) or set(document) != {"format_version", "versions"}
            or type(document["format_version"]) is not int or document["format_version"] != 1
            or not isinstance(document["versions"], dict)):
        raise ValueError("Expected format_version = 1 and a versions object")
    for version, notes in document["versions"].items():
        if len(version) > 128 or not VERSION.fullmatch(version):
            raise ValueError(f"Invalid package version: {version}")
        if not isinstance(notes, list) or any(not isinstance(note, str) for note in notes):
            raise ValueError(f"Version '{version}' must contain an array of strings")
        for note in notes:
            note.encode("utf-8")  # Reject invalid Unicode surrogate escapes.
    return document


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        with args.source.open("rb") as stream:
            contents = stream.read(MAX_SIZE + 1)
        validate(contents)
        if not args.output.exists() or args.output.read_bytes() != contents:
            temporary = args.output.with_suffix(".json.tmp")
            temporary.write_bytes(contents)
            temporary.replace(args.output)
    except (ValueError, OSError, UnicodeError, RecursionError) as error:
        parser.exit(1, f"Changelog '{args.source}': {error}\n")


if __name__ == "__main__":
    main()
