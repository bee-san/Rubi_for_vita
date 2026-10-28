#!/usr/bin/env python3
"""What the frequency ranks change in the popup, measured with the overlay's lookup.

Each word form of Jiten's deck exports (its CSV: Word, ReadingKana, Occurences)
is looked up as a tap on its first character (build/host/vjo-word-forms, the
Shell's vjo_overlay_lookup_at) in two dictionaries: --before, the bundled
dictionary without ranks (format 2), and --after, the same build with Jiten's
global ranks (format 3), whose ranking breaks ties by frequency. Weighted by
the form's occurrences, it reports how often the popup shows a ranked entry,
how often the shown reading is the one Jiten's parser read in the text, and
which shown entries the ranks changed, with the most frequent changes listed.
With --frequency (Jiten's ZIP) it also judges the kana-written forms whose entry
changed, which the reading comparison cannot see (いつか: 何時か or ５日, both read
いつか): is the new entry's word more often written in kana, by Jiten's kana-form
(㋕) ranks?

  python3 tools/frequency_eval.py --tool build/host/vjo-word-forms \
      --before DIR/plaintext.vjdict --after DIR/plaintext.vjdict --decks DECKS \
      [--frequency jiten_freq_global.zip] [-o out.json]

The deck exports (jiten.moe, CC BY-SA 4.0) are not committed; the output
keeps aggregate counts, the most frequent changes and the inputs' SHA-256.
Standard library only.
"""
import argparse
import collections
import csv
import gzip
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from evallib import utf16_len  # noqa: E402

csv.field_size_limit(1 << 24)


def normalize(text):
    """The overlay's lookup normalization: katakana as hiragana, full width ASCII as ASCII."""
    out = []
    for char in text:
        cp = ord(char)
        if 0x30A1 <= cp <= 0x30F6:
            cp -= 0x60
        if 0xFF01 <= cp <= 0xFF5E:
            cp -= 0xFEE0
        out.append(chr(cp))
    return "".join(out)


def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def forms(decks):
    counts = collections.Counter()
    for path in decks:
        raw = path.read_bytes()
        text = (gzip.decompress(raw) if path.suffix == ".gz" else raw).decode("utf-8-sig")
        for row in csv.DictReader(io.StringIO(text)):
            word = row["Word"].strip()
            if word and "\n" not in word and len(word) <= 32:
                counts[(word, row["ReadingKana"].strip())] += int(row["Occurences"])
    return counts


def shown(tool, dictionary, words):
    """{word: (spelling, reading, rank, len16) or None} from one run of vjo-word-forms."""
    dictionary = Path(dictionary).resolve()
    result = subprocess.run([str(tool), str(dictionary.parent), dictionary.name], input="\n".join(words) + "\n",
                            capture_output=True, text=True, check=True)
    lines = result.stdout.split("\n")[:-1]
    if len(lines) != len(words):
        raise SystemExit(f"{tool}: {len(lines)} results for {len(words)} words")
    out = {}
    for word, line in zip(words, lines):
        if line == "-":
            out[word] = None
        else:
            spelling, reading, rank, len16 = line.split("\t")
            out[word] = (spelling, reading, int(rank), int(len16))
    return out


def kana_form_ranks(path):
    """Jiten's kana-form ranks (its N㋕ rows): {(term, normalised reading, or None for a row
    without one): the lowest rank}."""
    out = {}
    with zipfile.ZipFile(path) as archive:
        for name in sorted(n for n in archive.namelist() if n.startswith("term_meta_bank_")):
            for term, mode, data in json.loads(archive.read(name)):
                reading = None
                if mode != "freq":
                    continue
                if isinstance(data, dict) and "reading" in data:
                    reading, data = data["reading"], data["frequency"]
                if not isinstance(data, dict) or not str(data.get("displayValue", "")).endswith("㋕"):
                    continue
                key = (term, None if reading is None else normalize(reading))
                if key not in out or data["value"] < out[key]:
                    out[key] = data["value"]
    return out


def is_kana(text):
    return bool(text) and all(0x3041 <= ord(c) <= 0x30FF for c in text)


def kana_rank(table, entry):
    """How often Jiten saw an entry's word written in kana (its ㋕ rank), or None."""
    spelling, reading = entry[0], entry[1]
    if is_kana(spelling):
        return table.get((spelling, None))
    return table.get((spelling, normalize(reading)))


def kana_text(counts, before, after, table):
    """Kana-written word forms whose shown entry changed: does the new entry's word appear
    written in kana more often (a better ㋕ rank) than the old one's? The reading
    comparison cannot see these (both entries have the reading)."""
    inf = 1 << 30
    totals = collections.Counter()
    worse, better = collections.Counter(), collections.Counter()
    for (word, _), occurrences in counts.items():
        if not is_kana(word):
            continue
        b, a = before[word], after[word]
        totals["kana_tokens"] += occurrences
        if b is None or a is None or b[:2] == a[:2]:
            continue
        totals["changed"] += occurrences
        rb, ra = kana_rank(table, b), kana_rank(table, a)
        kb, ka = inf if rb is None else rb, inf if ra is None else ra
        kind = "better" if ka < kb else "worse" if ka > kb else "equal"
        totals[kind] += occurrences
        if kind != "equal":
            (better if kind == "better" else worse)[(word, b[0], b[1], rb, a[0], a[1], ra)] += occurrences

    def listed(counter, n):
        return [{"word": w, "before": [bs, br, rb], "after": [as_, ar, ra], "occurrences": k}
                for (w, bs, br, rb, as_, ar, ra), k in counter.most_common(n)]

    changed = totals["changed"]
    return {
        "kana_written_tokens": totals["kana_tokens"],
        "changed_tokens": changed,
        "changed_by_kana_form_rank_percent": {
            k: round(100.0 * totals[k] / changed, 2) if changed else 0.0 for k in ("better", "worse", "equal")},
        "most_frequent_worse": listed(worse, 40),
        "most_frequent_better": listed(better, 20),
    }


def evaluate(counts, before, after):
    totals = collections.Counter()
    changes = collections.Counter()
    for (word, reading), occurrences in counts.items():
        b, a = before[word], after[word]
        totals["tokens"] += occurrences
        if a is None:
            totals["no_entry"] += occurrences
            continue
        whole = a[3] == utf16_len(word)
        totals["whole_word"] += occurrences * whole
        totals["ranked"] += occurrences * (a[2] > 0)
        totals["ranked_whole_word"] += occurrences * (whole and a[2] > 0)
        want = normalize(reading or word)
        hit_after = normalize(a[1]) == want
        hit_before = b is not None and normalize(b[1]) == want
        totals["reading_before"] += occurrences * hit_before
        totals["reading_after"] += occurrences * hit_after
        if whole:
            totals["whole_reading_before"] += occurrences * hit_before
            totals["whole_reading_after"] += occurrences * hit_after
        if b is None or b[:2] != a[:2] or b[3] != a[3]:
            totals["changed"] += occurrences
            kind = "better" if hit_after and not hit_before else "worse" if hit_before and not hit_after else "other"
            totals["changed_" + kind] += occurrences
            changes[(word, reading, b[:2] if b else None, a[:3], kind)] += occurrences
    with_entry = totals["tokens"] - totals["no_entry"]

    def pct(n, d=with_entry):
        return round(100.0 * n / d, 2) if d else 0.0

    top = [{"word": w, "jiten_reading": r, "before": list(b) if b else None, "after": list(a), "kind": k,
            "occurrences": n} for (w, r, b, a, k), n in changes.most_common(40)]
    return {
        "word_forms": len(counts), "tokens": totals["tokens"],
        "with_entry_tokens_percent": pct(with_entry, totals["tokens"]),
        "shown_entry_ranked_tokens_percent": pct(totals["ranked"]),
        "shown_entry_ranked_tokens_percent_of_all": pct(totals["ranked"], totals["tokens"]),
        "whole_word_matched_tokens_percent": pct(totals["whole_word"]),
        "shown_reading_is_jiten_reading_tokens_percent": {
            "before": pct(totals["reading_before"]), "after": pct(totals["reading_after"])},
        "whole_word_shown_reading_is_jiten_reading_tokens_percent": {
            "before": pct(totals["whole_reading_before"], totals["whole_word"]),
            "after": pct(totals["whole_reading_after"], totals["whole_word"])},
        "shown_entry_changed_tokens_percent": pct(totals["changed"]),
        "changed_tokens_percent": {k: pct(totals["changed_" + k]) for k in ("better", "worse", "other")},
        "counts": dict(sorted(totals.items())),
        "most_frequent_changes": top,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tool", required=True, type=Path, help="build/host/vjo-word-forms")
    parser.add_argument("--before", required=True, type=Path, help="the dictionary without ranks")
    parser.add_argument("--after", required=True, type=Path, help="the dictionary with ranks")
    parser.add_argument("--decks", required=True, type=Path, help="directory of Jiten deck CSV exports")
    parser.add_argument("--frequency", type=Path,
                        help="Jiten's frequency ZIP: also judge kana-written forms by its kana-form (㋕) ranks")
    parser.add_argument("-o", "--output", type=Path)
    args = parser.parse_args()
    decks = sorted(p for p in args.decks.iterdir() if p.name.endswith((".csv", ".csv.gz")))
    if not decks:
        parser.exit(1, f"no deck CSVs in {args.decks}\n")
    counts = forms(decks)
    words = sorted({w for w, _ in counts})
    result = {
        "what": "the popup's entry at the first character of each word form of Jiten's deck exports, weighted by "
                "the form's occurrences, before and after the frequency ranks (tools/frequency_eval.py)",
        "lookup": "vjo_overlay_lookup_at (build/host/vjo-word-forms), the Shell's tap lookup",
        "before": {"file": args.before.name, "sha256": digest(args.before)},
        "after": {"file": args.after.name, "sha256": digest(args.after)},
        "deck_source": "jiten.moe (CC BY-SA 4.0): each deck's CSV export",
        "decks": len(decks),
        "deck_files_sha256": hashlib.sha256("".join(
            f"{p.name}:{digest(p)}\n" for p in decks).encode()).hexdigest(),
    }
    before, after = shown(args.tool, args.before, words), shown(args.tool, args.after, words)
    result.update(evaluate(counts, before, after))
    if args.frequency:
        result["kana_text"] = kana_text(counts, before, after, kana_form_ranks(args.frequency))
    text = json.dumps(result, ensure_ascii=False, indent=1) + "\n"
    if args.output:
        args.output.write_text(text)
    sys.stdout.write(text)


if __name__ == "__main__":
    main()
