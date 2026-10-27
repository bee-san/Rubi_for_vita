#!/usr/bin/env python3
"""Wrong and missing readings per 1,000 kanji tokens: Rubi's sentence readings (core/sentence_reading.h) and
Yomitan's {sentence-furigana} against a morphological analyser's, on real visual-novel lines
(reports/sentence-readings.md).

  python3 tools/sentence_readings/compare.py --corpus lines.jsonl --sudachi sudachi.jsonl \\
      --system rubi=rubi.tsv --system yomitan=yomitan.tsv [--adjudication adjudication.tsv] \\
      [--sample yomitan=300 --seed 20261027 [--sample-out sample.tsv]] -o report.json [--patterns patterns.tsv]

Units are SudachiPy's tokens (split mode C) with a kanji (vjo_is_kanji's: CJK ideographs, 々〆ヵヶ): the kanji
tokens. A system's readings (each `start:end:reading`, code points, from vjo-sentence-readings or
yomitan_furigana.mjs) are laid over the line; a unit that a reading crosses grows to take the reading and the
tokens it reaches, so each grown unit is judged whole and counts once per kanji token in it. Its rendering
(each reading in place of its text, kana as hiragana) is compared with Sudachi's reading of the same tokens:

- correct: the same;
- missing: a kanji of the unit has no reading, and the readings it has agree with Sudachi's around it: the unit's
  rendering, each kanji without a reading taken as any kana, is Sudachi's, and each reading agrees with Sudachi's
  tokens where it lies (over whole tokens, their reading; from a token's start, a prefix of it; to a token's end, a
  suffix; inside one, a part). How one token's reading splits between its kanji cannot be checked (大[だいが]学 of
  だいがく passes): such a unit is missing, never correct;
- wrong: anything else; a unit Sudachi has no kana reading for (an unknown word) is not counted (noref).

Sudachi is a third opinion, not ground truth. Where a system differs from it, adjudication.tsv (filled by
reading the disagreements in context: --patterns lists them, by unit, rendering and Sudachi's, with a line)
says who is right: `unit<TAB>rendering<TAB>sudachi<TAB>verdict`, verdict `system` (the system's reading is
right), `both` (both are), `sudachi` or `neither`; `@line-id:start<TAB>verdict[<TAB>rendering]` judges one place
only (with a rendering, only a unit rendered so there: one system's): a place's verdict comes before its pattern's.
A wrong unit that no line of the file judges stays wrong (`unreviewed`).

--sample NAME=N: when NAME's units that differ from Sudachi are more than N, a seeded sample of N of them is judged
instead of all (the protocol's rule for Yomitan): `random.Random(seed).sample` of the units in line order. The
sampled units' verdicts give the share of its differing tokens that are wrong, and its wrong rate the share times
its differing tokens per 1,000, with a 95 % bootstrap interval over the sampled units (the same seed, --bootstrap
resamples). --sample-out lists the sample (local only).

The texts are the games' scripts: lines.jsonl, the system outputs, patterns.tsv and the sample stay local;
report.json holds counts only.
"""
import argparse
import collections
import json
import random
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


def token_reading(text, token):
    """A Sudachi token's reading, as hiragana (a kana token's own text when it has no kana reading)."""
    b, e, reading = token
    return hira(reading) if is_kana(reading) else hira(text[b:e])


def readings_agree(text, lo, hi, tokens, readings):
    """Each reading of the unit agrees with Sudachi's tokens where it lies (the module's docstring: missing)."""
    inside = [t for t in tokens if t[0] >= lo and t[1] <= hi]
    for s, e, reading in readings:
        if s < lo or e > hi:
            continue
        cover = [t for t in inside if t[0] < e and t[1] > s]
        if not cover:
            continue
        ref, r = "".join(token_reading(text, t) for t in cover), hira(reading)
        starts, ends = cover[0][0] == s, cover[-1][1] == e
        if starts and ends:
            ok = r == ref
        elif starts:
            ok = ref.startswith(r) and r != ref
        elif ends:
            ok = ref.endswith(r) and r != ref
        else:
            ok = r in ref and r != ref
        if not ok:
            return False
    return True


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
            places.setdefault(cols[0][1:], []).append((cols[1], cols[2] if len(cols) > 2 and cols[2] else None))
        elif len(cols) >= 4 and cols[3] in VERDICTS:
            patterns[(cols[0], cols[1], cols[2])] = cols[3]
        else:
            sys.exit(f"{path}:{n}: expected unit, rendering, sudachi, verdict ({'/'.join(VERDICTS)})")
    return patterns, places


def place_verdict(places, key, rendering):
    """The verdict of one place for a unit rendered so (a line without a rendering: any unit there), or None."""
    for verdict_, only in places.get(key, ()):
        if only is None or only == rendering:
            return verdict_
    return None


def percentile(sorted_values, q):
    """The q-quantile (0..1) of sorted values, interpolated between the nearest two."""
    k = (len(sorted_values) - 1) * q
    f = int(k)
    c = min(f + 1, len(sorted_values) - 1)
    return sorted_values[f] + (sorted_values[c] - sorted_values[f]) * (k - f)


def sample_estimate(units_, size, seed, resamples, differing_per_1000):
    """The --sample estimate (the module's docstring) from [(place, tokens, verdict or None)] in line order."""
    rng = random.Random(seed)
    sampled = rng.sample(units_, size)
    unjudged = sum(1 for _, _, v in sampled if v is None)
    tokens = sum(n for _, n, _ in sampled)
    wrong = sum(n for _, n, v in sampled if v in ("sudachi", "neither"))
    share = wrong / tokens if tokens else 0.0
    boot = []
    for _ in range(resamples):
        draw = [sampled[rng.randrange(size)] for _ in range(size)]
        t = sum(n for _, n, _ in draw)
        boot.append(sum(n for _, n, v in draw if v in ("sudachi", "neither")) / t if t else 0.0)
    boot.sort()
    return sampled, {
        "units_differing": len(units_), "units_sampled": size, "seed": seed, "resamples": resamples,
        "unjudged": unjudged, "wrong_units": sum(1 for _, _, v in sampled if v in ("sudachi", "neither")),
        "tokens_sampled": tokens, "wrong_tokens": wrong, "token_share_wrong": round(share, 4),
        "wrong_per_1000": round(share * differing_per_1000, 2),
        "wrong_per_1000_95ci_bootstrap": [round(percentile(boot, 0.025) * differing_per_1000, 1),
                                          round(percentile(boot, 0.975) * differing_per_1000, 1)],
        "right_by_adjudication_per_1000": round((1 - share) * differing_per_1000, 2),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--corpus", required=True, type=Path)
    parser.add_argument("--sudachi", required=True, type=Path)
    parser.add_argument("--system", action="append", required=True, help="NAME=readings.tsv")
    parser.add_argument("--adjudication", type=Path)
    parser.add_argument("-o", "--output", required=True, type=Path)
    parser.add_argument("--patterns", type=Path, help="the unjudged disagreements, for adjudication (local only)")
    parser.add_argument("--sample", action="append", default=[], help="NAME=N: judge a seeded sample of N units")
    parser.add_argument("--seed", type=int, default=20261027)
    parser.add_argument("--bootstrap", type=int, default=10000, help="the sample's bootstrap resamples")
    parser.add_argument("--sample-out", type=Path, help="the sampled units, for adjudication (local only)")
    args = parser.parse_args()
    samples = {}
    for spec in args.sample:
        name, size = spec.split("=", 1)
        samples[name] = int(size)
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
        differing = []  # (place, tokens, verdict or None) of each unit that differs from Sudachi, in line order
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
                if v == "missing" and not readings_agree(text, lo, hi, tokens, readings):
                    v = "wrong"
                if v != "wrong":
                    counts[v] += n
                    continue
                key = (text[lo:hi], display(rendered), reference)
                judged = place_verdict(places, f"{line['id']}:{lo}", display(rendered)) or patterns.get(key)
                differing.append((f"{line['id']}:{lo}", n, judged, key))
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
        if name in samples and len(differing) > samples[name]:
            sampled, estimate = sample_estimate([(p, n, v) for p, n, v, _ in differing], samples[name], args.seed,
                                                args.bootstrap,
                                                1000 * counts["wrong (differs from sudachi)"] / base)
            report["systems"][name]["sample"] = estimate
            if args.sample_out:
                keys = {p: k for p, _, _, k in differing}
                with args.sample_out.open("w", encoding="utf-8") as f:
                    for p, n, v in sampled:
                        unit, rendering, reference = keys[p]
                        f.write(f"{p}\t{n}\t{unit}\t{rendering}\t{reference}\t{v or ''}\n")
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    if args.patterns:
        with args.patterns.open("w", encoding="utf-8") as f:
            for (unit, rendering, reference), (n, where, names) in sorted(pending.items(), key=lambda kv: -kv[1][0]):
                f.write(f"{unit}\t{rendering}\t{reference}\t{n}\t{','.join(sorted(names))}\t{where}\n")
    print(json.dumps({k: v["per_1000_kanji_tokens"] for k, v in report["systems"].items()}, ensure_ascii=False))


if __name__ == "__main__":
    main()
