#!/usr/bin/env python3
"""The reference dictionaries for tools/yomitan_check, as headless Yomitan imports them.

Yomitan's importer runs in Node here (yomitan_lookup.mjs), where its zip.js reads stored
entries only (the reason Yomitan's own tests pack theirs at level 0), so each Yomitan ZIP
is rewritten with every member stored, its bytes unchanged. Rubi's bundled dictionary is
built from three of them (Daijirin4, Sankoku8 and JMdict Plain, bee-san/plaintext-dicts
sources/) with Jiten's global frequency list, and its names dictionary is the vendored
third_party/vndb-names ZIP.

Rubi ships no pitch accents, so the pitch reference is Kanjium's (mifunetoshiro/kanjium
data/source_files/raw/accents.txt, CC BY-SA 4.0), turned into a Yomitan pitch dictionary
the way toasted-nutbread/yomichan-pitch-accent-dictionary does: one term_meta row per
term and reading, a "(副)0"-style part of speech becoming the position's tag.

  python3 tools/yomitan_check/prepare_yomitan_dicts.py --sources DIR [--accents accents.txt] -o OUT

Standard library only. Nothing it writes is committed (the sources are private).
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import zipfile

SOURCES = ("Daijirin4-plain.zip", "Sankoku8-plain.zip", "JMdict-plain.zip", "jiten_freq_global.zip",
           "VNDB-Names-PSP-PSVita.zip")
PITCH_TITLE = "Kanjium Pitch Accents"


def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def store(source, output):
    """source's members, each stored (no compression), in output."""
    with zipfile.ZipFile(source) as src, zipfile.ZipFile(output, "w", zipfile.ZIP_STORED) as dst:
        for info in src.infolist():
            if not info.is_dir():
                dst.writestr(info.filename, src.read(info), zipfile.ZIP_STORED)


def pitch_rows(accents):
    """Kanjium's accents.txt (term, reading or "" for a kana term, positions) as term_meta rows."""
    rows = []
    for line in Path(accents).read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        term, reading, positions = line.split("\t")
        pitches = []
        for item in positions.split(","):
            match = re.fullmatch(r"(?:\(([^)]*)\))?(\d+)", item)
            if not match:
                raise ValueError(f"accents.txt: unexpected position {item!r} for {term}")
            pitch = {"position": int(match.group(2))}
            if match.group(1):
                pitch["tags"] = match.group(1).split(";")
            pitches.append(pitch)
        rows.append([term, "pitch", {"reading": reading or term, "pitches": pitches}])
    return rows


def pitch_dictionary(accents, output, revision):
    rows = pitch_rows(accents)
    index = {"title": PITCH_TITLE, "format": 3, "revision": revision, "sequenced": False,
             "author": "mifunetoshiro/kanjium", "attribution": "Kanjium (CC BY-SA 4.0)",
             "url": "https://github.com/mifunetoshiro/kanjium"}
    tags = [[t, "partOfSpeech", 0, t, 0] for t in ("副", "形動", "名", "代", "感")]
    with zipfile.ZipFile(output, "w", zipfile.ZIP_STORED) as dst:
        dst.writestr("index.json", json.dumps(index, ensure_ascii=False))
        dst.writestr("tag_bank_1.json", json.dumps(tags, ensure_ascii=False))
        for i in range(0, len(rows), 10000):
            dst.writestr(f"term_meta_bank_{i // 10000 + 1}.json", json.dumps(rows[i:i + 10000], ensure_ascii=False))
    return len(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sources", required=True, type=Path, help="directory with the five Yomitan ZIPs")
    parser.add_argument("--accents", type=Path, help="Kanjium's accents.txt")
    parser.add_argument("--accents-revision", default="", help="the kanjium commit accents.txt is from")
    parser.add_argument("-o", "--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    manifest = {"sources": {}}
    for name in SOURCES:
        source = args.sources / name
        store(source, args.output / name)
        manifest["sources"][name] = {"sha256": digest(source), "bytes": source.stat().st_size}
    if args.accents:
        out = args.output / "kanjium-pitch.zip"
        n = pitch_dictionary(args.accents, out, f"kanjium {args.accents_revision}".strip())
        manifest["pitch"] = {"accents_sha256": digest(args.accents), "revision": args.accents_revision, "rows": n}
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")
    print(json.dumps(manifest, indent=1))


if __name__ == "__main__":
    main()
