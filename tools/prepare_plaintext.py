#!/usr/bin/env python3
"""Unpack the frozen Daijirin4 -> Sankoku8 -> JMdict dictionary for releases.

Every entry carries its Jiten global frequency rank (format 3, 0 = unranked).
Use --source /path/to/plaintext-dicts for offline builds. Otherwise use the
CI checkout in .refs/plaintext-dicts, or authenticated gh for the pinned commit.
No conversion, latest-branch fetch, or dictionary update happens during builds:
the frozen file is checked against its pin, then gets the key filter
(tools/convert_dictionary.py add_key_filter, deterministic: an index of what is
already there) and is checked against that pin ("key_filter").
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
FILENAME = "plaintext.vjdict"



def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def load_manifest():
    return json.loads((ROOT / "docs/plaintext-dictionaries.json").read_text())


def validate(path, manifest, frozen=False):
    """The file as released (with the key filter when the manifest pins one), or with
    frozen the archive's file as committed."""
    pin = manifest if frozen or "key_filter" not in manifest else manifest["key_filter"]
    if path.stat().st_size != pin["bytes"] or digest(path) != pin["sha256"]:
        raise ValueError("Frozen plaintext dictionary checksum failed")
    with path.open("rb") as stream:
        if stream.read(8) != b"VJDICT3\0":
            raise ValueError("Expected a version 3 disk-backed dictionary (with frequency ranks)")


def prepare(output, source=None):
    output = Path(output)
    manifest = load_manifest()
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.is_file():
        try:
            validate(output, manifest)
        except ValueError:
            pass
        else:
            output.with_suffix(".json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
            return output
    checkout = source or ROOT / ".refs/plaintext-dicts"
    archive = Path(checkout) if Path(checkout).is_file() else Path(checkout) / manifest["archive"]
    with tempfile.TemporaryDirectory(prefix="vjo-plaintext-", dir=output.parent) as tmp:
        tmp = Path(tmp)
        if not archive.is_file():
            if source is not None:
                raise FileNotFoundError(archive)
            archive = tmp / "download.zip"
            endpoint = (f"repos/{manifest['repository']}/contents/{manifest['archive']}"
                        f"?ref={manifest['revision']}")
            # gh's terminal renderer can corrupt arbitrary binary output. The
            # object response supplies a short-lived URL for this exact blob.
            metadata = json.loads(subprocess.check_output(
                ["gh", "api", endpoint, "-H", "Accept: application/vnd.github.object+json"], text=True))
            with urllib.request.urlopen(metadata["download_url"], timeout=60) as src, archive.open("wb") as dst:
                shutil.copyfileobj(src, dst)
        if archive.stat().st_size != manifest["archive_bytes"] or digest(archive) != manifest["archive_sha256"]:
            raise ValueError("Frozen plaintext archive checksum failed")
        with zipfile.ZipFile(archive) as zipped:
            if zipped.namelist() != [FILENAME] or zipped.getinfo(FILENAME).file_size != manifest["bytes"]:
                raise ValueError("Unexpected frozen plaintext archive contents")
            extracted = tmp / FILENAME
            with zipped.open(FILENAME) as src, extracted.open("wb") as dst:
                shutil.copyfileobj(src, dst)
        validate(extracted, manifest, frozen=True)
        if "key_filter" in manifest:
            # Deferred import: the converter is only needed for this step.
            from convert_dictionary import add_index_hints, add_key_filter
            if manifest["key_filter"].get("hinted_postings"):
                add_index_hints(extracted)
            add_key_filter(extracted)
        validate(extracted, manifest)
        os.replace(extracted, output)
    output.with_suffix(".json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    return output


def stage(directory, source=None):
    directory = Path(directory)
    dictionary = prepare(directory / "ux0/data/VitaJPOverlay/dictionaries" / FILENAME, source)
    docs = directory / "docs"
    docs.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(dictionary.with_suffix(".json"), docs / "plaintext-dictionaries.json")
    dictionary.with_suffix(".json").unlink()
    for name in ("plaintext-attribution.txt", "JMdict-LICENSE.html"):
        shutil.copyfile(ROOT / "docs" / name, docs / name)
    return dictionary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--output", type=Path, default=Path(FILENAME))
    parser.add_argument("--source", type=Path, help="private repository checkout or frozen archive")
    parser.add_argument("--stage", type=Path, help="stage dictionary and attribution in a release directory")
    args = parser.parse_args()
    path = stage(args.stage, args.source) if args.stage else prepare(args.output, args.source)
    print(f"Verified frozen dictionary: {path}")


if __name__ == "__main__":
    main()
