#!/usr/bin/env python3
"""Build a plain English JMdict .vjdict using only Python's standard library.

The input stays outside Git. The manifest pins the release snapshot; --refresh
deliberately uses the latest EDRDG snapshot and records its new hash in results.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import tempfile
import urllib.request
import xml.etree.ElementTree as ET
import zipfile

from convert_dictionary import convert

SOURCE = "https://www.edrdg.org/pub/Nihongo/JMdict_e.gz"
LANG = "{http://www.w3.org/XML/1998/namespace}lang"


def rules_for(parts):
    rules = set()
    for pos in parts:
        lower = pos.lower()
        if "zuru" in lower:
            rules.add("vz")
        elif lower.startswith("ichidan verb"):
            rules.add("v1")
        elif lower.startswith("godan verb"):
            rules.add("v5")
        elif "suru" in lower:
            rules.add("vs")
        elif lower.startswith("kuru verb"):
            rules.add("vk")
        elif lower.startswith("adjective (keiyoushi)") or lower.startswith("adjective (keiyoushi) -"):
            rules.add("adj-i")
    return " ".join(sorted(rules))


def entry_rows(entry):
    senses, inherited = [], []
    for sense in entry.findall("sense"):
        parts = [p.text or "" for p in sense.findall("pos")] or inherited
        inherited = parts
        glosses = [g.text or "" for g in sense.findall("gloss") if g.get(LANG, "eng") == "eng"]
        if glosses:
            label = "(" + "; ".join(parts) + ") " if parts else ""
            notes = [n.text or "" for n in sense.findall("s_inf")]
            senses.append((sense, parts, label + "; ".join(glosses) +
                           (" — " + "; ".join(notes) if notes else "")))
    spellings = [k.text for k in entry.findall("k_ele/keb")]
    sequence = int(entry.findtext("ent_seq"))
    common = entry.find("k_ele/ke_pri") is not None or entry.find("r_ele/re_pri") is not None
    for reading in entry.findall("r_ele"):
        kana = reading.findtext("reb")
        restricted = [r.text for r in reading.findall("re_restr")]
        candidates = [None] if not spellings or reading.find("re_nokanji") is not None else spellings
        for spelling in candidates:
            if restricted and spelling not in restricted:
                continue
            selected, parts = [], []
            for sense, pos, gloss in senses:
                kanji_rules = [k.text for k in sense.findall("stagk")]
                kana_rules = [r.text for r in sense.findall("stagr")]
                if kanji_rules and spelling not in kanji_rules or kana_rules and kana not in kana_rules:
                    continue
                selected.append(gloss)
                parts.extend(pos)
            if selected:
                yield [spelling or kana, kana if spelling else "", "", rules_for(parts),
                       10 if common else 0, selected, sequence, ""]


def plain_zip(source, output):
    def write(archive, name, value):
        info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        archive.writestr(info, json.dumps(value, ensure_ascii=False, separators=(",", ":")))
    entries = rows = bank = 0
    batch = []
    with zipfile.ZipFile(output, "w") as archive, gzip.open(source, "rb") as stream:
        write(archive, "index.json", {"title": "JMdict (EDRDG)", "format": 3,
                                      "revision": "plain", "sequenced": True})
        parser = ET.iterparse(stream, events=("start", "end"))
        _, root = next(parser)
        for event, entry in parser:
            if event != "end" or entry.tag != "entry":
                continue
            entries += 1
            for row in entry_rows(entry):
                rows += 1
                batch.append(row)
                if len(batch) == 10000:
                    bank += 1
                    write(archive, f"term_bank_{bank}.json", batch)
                    batch = []
            root.clear()
        if batch:
            write(archive, f"term_bank_{bank + 1}.json", batch)
    if not rows:
        raise ValueError("JMdict contained no English entries")
    return {"source_entries": entries, "term_rows": rows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, help="Previously downloaded JMdict_e.gz")
    parser.add_argument("-o", "--output", type=Path, required=True)
    parser.add_argument("--refresh", action="store_true")
    args = parser.parse_args()
    manifest = json.loads((Path(__file__).resolve().parents[1] / "docs/jmdict-source.json").read_text())
    with tempfile.TemporaryDirectory(prefix="vjo-jmdict-") as directory:
        directory = Path(directory)
        source = args.source or directory / "JMdict_e.gz"
        if args.source is None:
            url = SOURCE if args.refresh else manifest.get("snapshot_url", SOURCE)
            urllib.request.urlretrieve(url, source)
        data = source.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if not args.refresh and (digest != manifest["sha256"] or len(data) != manifest["bytes"]):
            parser.error("JMdict differs from the release snapshot. Use its archived source, or --refresh to update deliberately.")
        archive = directory / "jmdict-plain.zip"
        stats = plain_zip(source, archive)
        stats.update(convert([archive], args.output))
        stats.update(source_url=SOURCE, source_sha256=digest, source_bytes=len(data),
                     sha256=hashlib.sha256(args.output.read_bytes()).hexdigest(),
                     bytes=args.output.stat().st_size, license="CC-BY-SA-4.0")
        args.output.with_suffix(".json").write_text(json.dumps(stats, indent=2) + "\n")
        print(json.dumps(stats, indent=2))


if __name__ == "__main__":
    main()
