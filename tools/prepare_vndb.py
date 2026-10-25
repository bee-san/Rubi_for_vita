#!/usr/bin/env python3
"""Prepare the frozen PSP/Vita VNDB names dictionary offline, with pinned checksums.

The unchanged Yomitan ZIP is vendored in third_party/vndb-names. Conversion uses
Rubi's bounded, disk-backed format 2, with the key filter and no frequency ranks.

Beside it, vndb-games.vjgames: each game's own names, keyed by the title ID a console
reports, from the vendored VNDB-Names-Games.zip (docs/per-game-names.md). Each game is a
small format 1 dictionary; the pack is a header, a sorted table of title IDs and the
dictionaries. Keys among the word dictionary's most frequent words are left out
(third_party/vndb-names/games-common-words.txt).
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import tempfile
import zipfile

from prepare_plaintext import digest

ROOT = Path(__file__).resolve().parents[1]
FILENAME = "vndb-names.vjdict"
GAMES = "vndb-games.vjgames"
GAMES_MAGIC = b"VJGAMES1"
GAMES_HEADER = struct.Struct("<8sIIIIQQ")  # magic, version, header size, IDs, record size, records, file size
GAMES_HEADER_SIZE = 64
GAMES_RECORD = struct.Struct("<12sII")     # title ID (NUL padded), dictionary offset, dictionary length
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


def guard_words(path=None):
    path = Path(path) if path else ROOT / load_manifest()["per_game"]["guard"]
    return {line.split("\t")[0] for line in path.read_text(encoding="utf-8").splitlines()
            if line and not line.startswith("#")}


def build_games(archive, output, guard):
    """The pack from the per-game archive; returns its counts."""
    from convert_dictionary import convert
    with zipfile.ZipFile(archive) as z:
        entries = json.loads(z.read("games.json"))
    blobs, ids, dropped = [], [], 0
    with tempfile.TemporaryDirectory(prefix="vjo-games-") as tmp:
        for i, entry in enumerate(entries):
            terms = [t for t in entry["terms"] if t[0] not in guard]
            dropped += len(entry["terms"]) - len(terms)
            if not terms:
                continue
            source = Path(tmp) / f"{i}.zip"
            with zipfile.ZipFile(source, "w") as z:
                z.writestr("index.json", json.dumps({"title": entry["title"], "format": 3, "sequenced": True}))
                z.writestr("term_bank_1.json", json.dumps(terms, ensure_ascii=False))
            converted = Path(tmp) / f"{i}.vjdict"
            convert([source], converted, max_definition_bytes=7000, format_version=1)
            ids.extend((tid, len(blobs)) for tid in entry["title_ids"])
            blobs.append(converted.read_bytes())
    ids.sort()
    if len({tid for tid, _ in ids}) != len(ids):
        raise ValueError("a title ID names two game sets")
    offset = GAMES_HEADER_SIZE + GAMES_RECORD.size * len(ids)
    places = []
    for blob in blobs:
        offset = (offset + 7) & ~7
        places.append((offset, len(blob)))
        offset += len(blob)
    if offset > 0xFFFFFFFF:
        raise ValueError("the pack's offsets are 32-bit")
    with open(output, "wb") as out:
        out.write(GAMES_HEADER.pack(GAMES_MAGIC, 1, GAMES_HEADER_SIZE, len(ids), GAMES_RECORD.size,
                                    GAMES_HEADER_SIZE, offset).ljust(GAMES_HEADER_SIZE, b"\0"))
        for tid, index in ids:
            out.write(GAMES_RECORD.pack(tid.encode("ascii"), *places[index]))
        for (at, _), blob in zip(places, blobs):
            out.write(b"\0" * (at - out.tell()))
            out.write(blob)
    return {"game_sets": len(blobs), "title_ids": len(ids), "guarded_terms": dropped}


def validate_games(path, pin):
    if path.stat().st_size != pin["bytes"] or digest(path) != pin["sha256"]:
        raise ValueError("Frozen per-game names checksum failed")


def prepare_games(output, source=None):
    output = Path(output)
    pin = load_manifest()["per_game"]
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.is_file():
        try:
            validate_games(output, pin)
            return output
        except ValueError:
            pass
    archive = Path(source) if source is not None else ROOT / pin["archive"]
    if archive.stat().st_size != pin["archive_bytes"] or digest(archive) != pin["archive_sha256"]:
        raise ValueError("Frozen per-game names archive checksum failed")
    guard = ROOT / pin["guard"]
    if digest(guard) != pin["guard_sha256"]:
        raise ValueError("Per-game names guard list checksum failed")
    with tempfile.TemporaryDirectory(prefix="vjo-games-", dir=output.parent) as tmp:
        built = Path(tmp) / GAMES
        build_games(archive, built, guard_words(guard))
        validate_games(built, pin)
        os.replace(built, output)
    return output


def stage(directory, source=None):
    directory = Path(directory)
    prepare_games(directory / "ux0/data/VitaJPOverlay/dictionaries" / GAMES)
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
