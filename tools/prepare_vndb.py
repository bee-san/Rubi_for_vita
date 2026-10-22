#!/usr/bin/env python3
"""Prepare the frozen PSP/Vita VNDB names dictionary offline, with pinned checksums.

The unchanged Yomitan ZIP is vendored in third_party/vndb-names. Conversion uses
Rubi's bounded, disk-backed format 2, with the key filter and no frequency ranks.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import tempfile

from prepare_plaintext import digest

ROOT = Path(__file__).resolve().parents[1]
FILENAME = "vndb-names.vjdict"
DOCS = ("vndb-names.json", "vndb-attribution.txt")
LICENSES = ("VNDB-ODbL-1.0.txt", "VNDB-DbCL-1.0.txt")


def load_manifest():
    return json.loads((ROOT / "docs/vndb-names.json").read_text(encoding="utf-8"))


def validate(path, manifest):
    if path.stat().st_size != manifest["bytes"] or digest(path) != manifest["sha256"]:
        raise ValueError("Frozen VNDB names dictionary checksum failed")


def prepare(output, source=None):
    output = Path(output)
    pin = load_manifest()
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.is_file():
        try:
            validate(output, pin)
            return output
        except ValueError:
            pass
    archive = Path(source) if source is not None else ROOT / pin["archive"]
    if archive.stat().st_size != pin["archive_bytes"] or digest(archive) != pin["archive_sha256"]:
        raise ValueError("Frozen VNDB names archive checksum failed")
    # Deferred: release --verify on the Vita installer's computer needs no converter.
    from convert_dictionary import convert
    with tempfile.TemporaryDirectory(prefix="vjo-vndb-", dir=output.parent) as tmp:
        converted = Path(tmp) / FILENAME
        convert([archive], converted, max_definition_bytes=pin["max_definition_bytes"])
        # The dictionary's existing display-title slot (core/local_dict.h), not a new format.
        with converted.open("r+b") as stream:
            stream.seek(192)
            stream.write(pin["title"].encode("utf-8").ljust(64, b"\0"))
        validate(converted, pin)
        os.replace(converted, output)
    return output


def stage(directory, source=None):
    directory = Path(directory)
    dictionary = prepare(directory / "ux0/data/VitaJPOverlay/dictionaries" / FILENAME, source)
    for name in DOCS:
        dest = directory / "docs" / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / "docs" / name, dest)
    for name in LICENSES:
        dest = directory / "third_party/licenses" / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / "third_party/licenses" / name, dest)
    return dictionary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--output", type=Path, default=Path(FILENAME))
    parser.add_argument("--source", type=Path, help="the pinned combined Yomitan ZIP")
    parser.add_argument("--stage", type=Path, help="stage dictionary, provenance and licences in a release")
    args = parser.parse_args()
    path = stage(args.stage, args.source) if args.stage else prepare(args.output, args.source)
    print(f"Verified frozen names dictionary: {path}")


if __name__ == "__main__":
    main()
