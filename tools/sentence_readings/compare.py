#!/usr/bin/env python3
"""Wrong and missing readings per 1,000 kanji tokens: Rubi's sentence readings (core/sentence_reading.h) and
Yomitan's {sentence-furigana} against a morphological analyser's, on real visual-novel lines
(reports/sentence-readings.md).

  python3 tools/sentence_readings/compare.py --corpus lines.jsonl --sudachi sudachi.jsonl \\
      --system rubi=rubi.tsv --system yomitan=yomitan.tsv [--adjudication adjudication.tsv] \\
      -o report.json [--patterns patterns.tsv]

Units are SudachiPy's tokens (split mode C) with a kanji (vjo_is_kanji's: CJK ideographs, 々〆ヵヶ): the kanji
tokens. A system's readings (each `start:end:reading`, code points, from vjo-sentence-readings or
yomitan_furigana.mjs) are laid over the line; a unit that a reading crosses grows to take the reading and the
tokens it reaches, so each grown unit is judged whole and counts once per kanji token in it. Its rendering
(each reading in place of its text, kana as hiragana) is compared with Sudachi's reading of the same tokens:

- correct: the same;
- missing: a kanji of the unit has no reading, and the readings it has agree with Sudachi's around it;
- wrong: anything else; a unit Sudachi has no kana reading for (an unknown word) is not counted (noref).

Sudachi is a third opinion, not ground truth. Where a system differs from it, adjudication.tsv (filled by
reading the disagreements in context: --patterns lists them, by unit, rendering and Sudachi's, with a line)
says who is right: `unit<TAB>rendering<TAB>sudachi<TAB>verdict`, verdict `system` (the system's reading is
right), `both` (both are), `sudachi` or `neither`; a `@line-id:start` key in place of the first three columns
judges one place only. A wrong unit that no line of the file judges stays wrong (`unreviewed`). The texts are
the games' scripts: lines.jsonl, the system outputs and patterns.tsv stay local; report.json holds counts only.
"""
import argparse
import collections
import json
import re
import sys
from pathlib import Path

VERDICTS = ("system", "both", "sudachi", "neither")


def is_kanji(c):
    o = ord(c)
    return (0x3400 <= o <= 0x4DBF or 0x4E00 <= o <= 0x9FFF or 0xF900 <= o <= 0xFAFF or 0x20000 <= o <= 0x3FFFF
            or c in "々〆ヵヶ")


def hira(s):
    """Katakana as hiragana (ヵヶ stay: they are kanji here)."""
    return "".join(chr(ord(c) - 0x60) if 0x30A1 <= ord(c) <= 0x30F4 else c for c in s)


def is_kana(s):
    return bool(s) and all(0x3041 <= ord(c) <= 0x3096 or c in "ーゝゞ" for c in hira(s))


def parse_readings(field):
    """`start:end:reading ...` -> sorted [(start, end, reading)]."""
    out = []
    for item in field.split():
        start, end, reading = item.split(":", 2)
        out.append((int(start), int(end), reading))
    return sorted(out)


def load_system(path):
    """A system's output: id -> readings (an ERROR line: None, the line failed)."""
    rows = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        cols = line.split("\t")
        if len(cols) < 2:
            continue
        rows[cols[0]] = None if cols[1].startswith("ERROR") else parse_readings(cols[1])
    return rows


def units(text, tokens, readings):
    """The line's units: [(lo, hi, kanji tokens in it, Sudachi's reading or None)], tokens [(b, e, reading)]."""
    out, taken = [], set()
    kanji_tokens = [i for i, (b, e, _) in enumerate(tokens) if any(is_kanji(c) for c in text[b:e])]
    for i in kanji_tokens:
        if i in taken:
            continue
        lo, hi = tokens[i][0], tokens[i][1]
        changed = True
        while changed:
            changed = False
            for s, e, _ in readings:
                if s < hi and e > lo and (s < lo or e > hi):
                    lo, hi, changed = min(lo, s), max(hi, e), True
            for b, e, _ in tokens:
                if b < hi and e > lo and (b < lo or e > hi):
                    lo, hi, changed = min(lo, b), max(hi, e), True
        inside = [k for k, (b, e, _) in enumerate(tokens) if b >= lo and e <= hi]
        taken.update(inside)
        reference = []
        for k in inside:
            b, e, reading = tokens[k]
            surface = text[b:e]
            if any(is_kanji(c) for c in surface):
                if not is_kana(reading):
                    reference = None
                    break
                reference.append(hira(reading))
            else:
                reference.append(hira(reading) if is_kana(reading) else hira(surface))
        n = sum(1 for k in inside if k in kanji_tokens)
        out.append((lo, hi, n, None if reference is None else "".join(reference)))
    return out


def render(text, lo, hi, readings):
    """The unit as the system reads it: [(piece, kind)], kind `reading` (a reading in place of its text, as
    hiragana), `kana` (a character of the text, as hiragana) or `miss` (a kanji without a reading)."""
    starts = {s: (e, r) for s, e, r in readings if s >= lo and e <= hi}
    out, i = [], lo
    while i < hi:
        if i in starts:
            e, r = starts[i]
            out.append((hira(r), "reading"))
            i = e
            continue
        out.append((text[i], "miss") if is_kanji(text[i]) else (hira(text[i]), "kana"))
        i += 1
    return out


def verdict(rendered, reference):
    """correct, missing or wrong (the module's docstring): a unit the system gave no reading at all is missing,
    whatever its kana."""
    kinds = {kind for _, kind in rendered}
    if "miss" not in kinds:
        return "correct" if "".join(p for p, _ in rendered) == reference else "wrong"
    if "reading" not in kinds:
        return "missing"
    pattern = "".join(".+" if kind == "miss" else re.escape(p) for p, kind in rendered)
    return "missing" if re.fullmatch(pattern, reference) else "wrong"


def display(rendered):
    return "".join("＊" if kind == "miss" else p for p, kind in rendered)


def load_adjudication(path):
    patterns, places = {}, {}
    if not path:
        return patterns, places
    for n, line in enumerate(Path(path).read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        cols = line.split("\t")
        if cols[0].startswith("@") and len(cols) >= 2 and cols[1] in VERDICTS:
            places[cols[0][1:]] = cols[1]
        elif len(cols) >= 4 and cols[3] in VERDICTS:
            patterns[(cols[0], cols[1], cols[2])] = cols[3]
        else:
            sys.exit(f"{path}:{n}: expected unit, rendering, sudachi, verdict ({'/'.join(VERDICTS)})")
    return patterns, places


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--corpus", required=True, type=Path)
    parser.add_argument("--sudachi", required=True, type=Path)
    parser.add_argument("--system", action="append", required=True, help="NAME=readings.tsv")
    parser.add_argument("--adjudication", type=Path)
    parser.add_argument("-o", "--output", required=True, type=Path)
    parser.add_argument("--patterns", type=Path, help="the unjudged disagreements, for adjudication (local only)")
    args = parser.parse_args()
    lines = [json.loads(x) for x in args.corpus.read_text(encoding="utf-8").splitlines() if x.strip()]
    sudachi = {}
    for x in args.sudachi.read_text(encoding="utf-8").splitlines():
        if x.strip():
            row = json.loads(x)
            sudachi[row["id"]] = [tuple(t) for t in row["tokens"]]
    patterns, places = load_adjudication(args.adjudication)
    report = {"lines": len(lines), "systems": {}}
    pending = collections.defaultdict(lambda: [0, None, set()])
    for spec in args.system:
        name, path = spec.split("=", 1)
        rows = load_system(path)
        counts = collections.Counter()
        wrong_by = collections.Counter()
        for line in lines:
            text, tokens = line["text"], sudachi.get(line["id"])
            readings = rows.get(line["id"])
            if tokens is None:
                counts["lines without sudachi"] += 1
                continue
            if readings is None:
                counts["lines failed" if line["id"] in rows else "lines missing"] += 1
                readings = []
            for lo, hi, n, reference in units(text, tokens, readings):
                counts["kanji tokens"] += n
                if reference is None:
                    counts["noref"] += n
                    continue
                counts["with reference"] += n
                rendered = render(text, lo, hi, readings)
                v = verdict(rendered, reference)
                if v != "wrong":
                    counts[v] += n
                    continue
                key = (text[lo:hi], display(rendered), reference)
                judged = places.get(f"{line['id']}:{lo}") or patterns.get(key)
                counts["wrong (differs from sudachi)"] += n
                if judged in ("system", "both"):
                    counts["right by adjudication"] += n
                    wrong_by["sudachi wrong" if judged == "system" else "both right"] += n
                elif judged in ("sudachi", "neither"):
                    counts["wrong"] += n
                    wrong_by[judged] += n
                else:
                    counts["wrong"] += n
                    counts["unreviewed"] += n
                    entry = pending[key]
                    entry[0] += n
                    entry[1] = entry[1] or f"{line['id']}:{lo}"
                    entry[2].add(name)
        base = counts["with reference"] or 1
        report["systems"][name] = {
            "counts": dict(sorted(counts.items())),
            "per_1000_kanji_tokens": {k: round(1000 * counts[k] / base, 2)
                                      for k in ("correct", "missing", "wrong", "unreviewed",
                                                "wrong (differs from sudachi)", "right by adjudication")},
            "adjudicated": dict(wrong_by),
        }
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    if args.patterns:
        with args.patterns.open("w", encoding="utf-8") as f:
            for (unit, rendering, reference), (n, where, names) in sorted(pending.items(), key=lambda kv: -kv[1][0]):
                f.write(f"{unit}\t{rendering}\t{reference}\t{n}\t{','.join(sorted(names))}\t{where}\n")
    print(json.dumps({k: v["per_1000_kanji_tokens"] for k, v in report["systems"].items()}, ensure_ascii=False))


if __name__ == "__main__":
    main()
