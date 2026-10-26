#!/usr/bin/env python3
"""Rubi's lookups beside Yomitan's on the same taps: what differs, by class, per 1,000 lookups.

Inputs: the corpus (build_corpus.py), Rubi's taps (build/host/vjo-dict-taps: the Shell's
lookup with the shipped plaintext.vjdict and vndb-names.vjdict) and Yomitan's
(yomitan_lookup.mjs: the first entries of its popup for the same taps). Rubi shows one entry
(page 0) with the names' entry for the same word below it; Yomitan's popup lists its entries,
the first one on top. So Rubi's shown entry is compared with Yomitan's first entry.

A tap's shown entry differs ("entry mismatch") when the highlighted span or the headword (its
spelling, or its reading with katakana as hiragana) differs. Every mismatch gets one cause:

  deinflection_missing  Yomitan's longer match is an inflection of a word Rubi's dictionary has,
                        which Rubi's rules do not reach
  deinflection_extra    Rubi's longer match is an inflection Yomitan's rules do not accept
  deinflection_choice   same span, one side inflected and the other not, or two dictionary forms
  normalization         Yomitan's match needed a text processor Rubi has no equivalent of
                        (kanji variants, emphatic sequences, half width, combining marks...)
  missing_entry         Yomitan's match is a word Rubi's dictionary lacks (or Rubi has no entry)
  rubi_only_entry       Rubi's match is a word none of Yomitan's dictionaries has (its grammar points)
  longest_match         the spans differ otherwise
  reading               same span and spelling, another reading
  kana_kanji            same span and reading, one side spelled in kana (いつも, 何時も)
  spelling              same span and reading, both spelled otherwise (a variant or a homophone)
  headword              same span, another word: another spelling and another reading
  names                 one side shows a character's name (vndb-names) where the other shows a word

Shown either way, independently counted: frequency (same headword, Rubi's rank is none of the
Jiten values Yomitan shows for it), pitch (Yomitan's first entry has a pitch accent; Rubi ships
none), stacked names (Yomitan lists a name of the same span below its first entry; Rubi stacks
none, or the reverse) and names first (the first entry is a name while a dictionary word of the
same span exists).

Where the two disagree, SudachiPy's analysis of the line (the tapped token's dictionary form and
reading) says which, if either, reads the text as it does.

  python3 tools/yomitan_check/compare.py --corpus corpus.jsonl --rubi taps.tsv \
      --yomitan yomitan.jsonl --rubi-dict plaintext.vjdict --sources DIR -o report.json
"""
import argparse
import collections
import json
from pathlib import Path
import re
import struct
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]
NAMES_RUBI = "VNDB Names"
NAMES_YOMITAN = "VNDB Names"
SHOWN = ("frequency", "pitch", "stacked_names", "names_first_rubi", "names_first_yomitan")


def hira(s):
    return "".join(chr(ord(c) - 0x60) if 0x30A1 <= ord(c) <= 0x30F6 else c for c in s or "")


def is_kana(s):
    return bool(s) and all(0x3041 <= ord(c) <= 0x30FF for c in s)


def fold(s):
    """Rubi's key normalisation: katakana as hiragana, full-width ASCII as ASCII."""
    out = []
    for c in s or "":
        cp = ord(c)
        if 0x30A1 <= cp <= 0x30F6:
            cp -= 0x60
        if 0xFF01 <= cp <= 0xFF5E:
            cp -= 0xFEE0
        out.append(chr(cp))
    return "".join(out)


def vjdict_keys(path):
    """The keys of a .vjdict (version 2 or 3): every index record's key."""
    keys = set()
    with open(path, "rb") as f:
        head = f.read(64)
        if head[:6] != b"VJDICT" or head[6:7] not in b"23":
            raise ValueError(f"{path}: not a version 2/3 dictionary")
        data = struct.unpack_from("<Q", head, 32)[0]
        f.seek(4096)
        index = f.read(data - 4096)
        for page in range(0, len(index), 4096):
            if index[page:page + 4] != b"VJHP":
                continue
            count = struct.unpack_from("<I", index, page + 4)[0]
            for r in range(count):
                _, group, _, key_len, _ = struct.unpack_from("<QQIHH", index, page + 16 + r * 24)
                f.seek(group)
                keys.add(f.read(key_len).decode("utf-8"))
    return keys


def yomitan_terms(sources):
    """{folded term or reading} of the term dictionaries' rows."""
    keys = set()
    for name in ("Daijirin4-plain.zip", "Sankoku8-plain.zip", "JMdict-plain.zip", "VNDB-Names-PSP-PSVita.zip"):
        with zipfile.ZipFile(Path(sources) / name) as z:
            for member in z.namelist():
                if re.match(r"term_bank_\d+\.json$", member):
                    for row in json.loads(z.read(member)):
                        keys.add(fold(row[0]))
                        if row[1]:
                            keys.add(fold(row[1]))
    return keys


def load_rubi(path):
    out = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        f = line.split("\t")
        if f[1] == "-1":
            continue
        key = (f[0], int(f[1]))
        if f[2] == "error":
            out[key] = {"error": f[3]}
            continue
        n = int(f[2])
        if not n:
            out[key] = None
            continue
        more = [dict(zip(("spelling", "reading", "dictionary"), m.split("|"))) for m in f[8].split(";") if m]
        out[key] = {"len": n, "spelling": f[3], "reading": f[4], "rank": int(f[5]),
                    "reasons": [x for x in f[6].split(",") if x],
                    "dictionary": f[7], "more": more}
    return out


def load_yomitan(paths):
    out = {}
    for path in paths:
        for line in Path(path).read_text(encoding="utf-8").splitlines():
            r = json.loads(line)
            out[(r["id"], r["tap"])] = r
    return out


def shortest_chain(entry):
    chains = [c["rules"] for c in entry["inflections"]] or [[]]
    return min(chains, key=len)


def processors(entry):
    chains = entry.get("processors") or [[]]
    return min(chains, key=len)


def is_name(dictionaries):
    return any(d.startswith(NAMES_YOMITAN) for d in dictionaries)


class Oracle:
    """SudachiPy's reading of the tapped token: its dictionary form and that form's reading."""

    def __init__(self):
        from sudachipy import Dictionary, SplitMode
        self.tok = Dictionary(dict="core").create()
        self.mode = SplitMode.C
        self.lines = {}
        self.readings = {}

    def tokens(self, line_id, text):
        if line_id not in self.lines:
            self.lines[line_id] = {m.begin(): m for m in self.tok.tokenize(text, self.mode)}
        return self.lines[line_id]

    def reading_of(self, word):
        if word not in self.readings:
            ms = list(self.tok.tokenize(word, self.mode))
            self.readings[word] = hira("".join(m.reading_form() for m in ms))
        return self.readings[word]

    def judge(self, line_id, text, tap, term, reading):
        """1 when (term, reading) is SudachiPy's token at tap (its dictionary or normalised form,
        that form's reading), 0 when it is not, None when the token is not a word to judge."""
        m = self.tokens(line_id, text).get(tap)
        if m is None or m.part_of_speech()[0] in ("補助記号", "空白"):
            return None
        forms = {m.dictionary_form(), m.normalized_form(), m.surface()}
        want = {fold(f) for f in forms}
        if fold(term) not in want and hira(reading) not in {hira(self.reading_of(f)) for f in forms}:
            return 0
        if hira(reading) not in {self.reading_of(f) for f in forms} | {hira(m.reading_form())}:
            return 0
        return 1


def excerpt(text, tap, length):
    chars = list(text)
    a, b = max(0, tap - 6), min(len(chars), tap + max(length, 1) + 6)
    return "".join(chars[a:tap]) + "【" + "".join(chars[tap:tap + max(length, 1)]) + "】" + "".join(chars[tap + max(length, 1):b])


def describe_rubi(r):
    if not r:
        return None
    d = {"len": r["len"], "spelling": r["spelling"], "reading": r["reading"], "rank": r["rank"],
         "dictionary": r["dictionary"]}
    if r["reasons"]:
        d["conjugation"] = r["reasons"]
    if r["more"]:
        d["below"] = [f"{m['spelling']} ({m['reading']})" for m in r["more"]]
    return d


def describe_yomitan(y):
    if not y or not y["entries"]:
        return None
    e = y["entries"][0]
    h = e["headwords"][0]
    d = {"len": y["len"], "term": h["term"], "reading": h["reading"], "dictionaries": e["dictionaries"],
         "frequencies": [f["displayValue"] for f in e["frequencies"] if f["dictionary"] == "Jiten"]}
    chain = shortest_chain(e)
    if chain:
        d["inflections"] = chain
    proc = processors(e)
    if proc:
        d["text_processors"] = proc
    if e["pitches"]:
        d["pitch"] = [p["positions"] for p in e["pitches"]]
    if len(y["entries"]) > 1:
        d["next"] = [f"{x['headwords'][0]['term']} ({x['headwords'][0]['reading']})" for x in y["entries"][1:3]]
    return d


def classify(text, tap, r, y, rubi_keys, yomitan_keys):
    """(mismatch cause or None, other classes)."""
    other = []
    y_entries = y["entries"] if y else []
    if not r and not y_entries:
        return None, other
    if not y_entries:
        if r["spelling"] and fold(r["spelling"]) not in yomitan_keys and fold(r["reading"]) not in yomitan_keys:
            return "rubi_only_entry", other
        return ("deinflection_extra" if r["reasons"] else "longest_match"), other
    e0 = y_entries[0]
    h = e0["headwords"][0]
    y_term, y_read, y_len = h["term"], h["reading"], y["len"]
    y_chain, y_proc = shortest_chain(e0), processors(e0)
    y_name = is_name(e0["dictionaries"])
    if e0["pitches"]:
        other.append("pitch")
    # over Yomitan's whole list (yomitan_lookup.mjs "names", "word"): a name of the first entry's
    # span besides the first entry, and the first entry of a dictionary of words
    y_names = y.get("names", [])
    y_stacked = bool(y_names[1:] if y_name else y_names)
    y_word = y.get("word")
    if not r:
        return ("deinflection_missing" if y_chain else "normalization" if y_proc else "missing_entry"), other
    r_name = r["dictionary"].startswith(NAMES_RUBI)
    r_stacked = any(m["dictionary"].startswith(NAMES_RUBI) for m in r["more"])
    if r_name and r["more"]:
        other.append("names_first_rubi")  # a name on top, the word of its span below it
    if y_name and y_word and y_word["len"] == y_len:
        other.append("names_first_yomitan")
    if not r_name and not y_name and r["len"] == y_len and r_stacked != y_stacked:
        other.append("stacked_names")
    same_head = r["spelling"] == y_term and hira(r["reading"]) == hira(y_read)
    if r["len"] == y_len and same_head:
        jiten = [f for f in e0["frequencies"] if f["dictionary"] == "Jiten" and f["headwordIndex"] == 0]
        values = {f["frequency"] for f in jiten}
        if (r["rank"] or None) not in (values or {None}):
            other.append("frequency")
        if bool(r["reasons"]) != bool(y_chain):
            return "deinflection_choice", other
        return None, other
    if r_name != y_name:
        return "names", other
    if r["len"] != y_len:
        if y_len > r["len"]:
            if y_chain:
                if fold(y_term) in rubi_keys or fold(y_read) in rubi_keys:
                    return "deinflection_missing", other
                return "missing_entry", other
            if y_proc:
                return "normalization", other
            if fold(y_term) not in rubi_keys and fold(y_read) not in rubi_keys:
                return "missing_entry", other
            return "longest_match", other
        if fold(r["spelling"]) not in yomitan_keys and fold(r["reading"]) not in yomitan_keys:
            return "rubi_only_entry", other
        if r["reasons"]:
            return "deinflection_extra", other
        return "longest_match", other
    if bool(r["reasons"]) != bool(y_chain) or (r["reasons"] and y_chain and r["spelling"] != y_term):
        return "deinflection_choice", other
    if r["spelling"] == y_term:
        return "reading", other
    if y_proc and not r["reasons"]:
        return "normalization", other
    if hira(r["reading"]) == hira(y_read):
        if is_kana(r["spelling"]) != is_kana(y_term):
            return "kana_kanji", other  # one word, one side spelled in kana
        return "spelling", other  # the same reading spelled otherwise: a variant or a homophone
    return "headword", other


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--corpus", required=True, type=Path)
    parser.add_argument("--rubi", required=True, type=Path)
    parser.add_argument("--yomitan", required=True, type=Path, nargs="+")
    parser.add_argument("--rubi-dict", required=True, type=Path, help="plaintext.vjdict (its keys)")
    parser.add_argument("--names-dict", type=Path, help="vndb-names.vjdict (its keys)")
    parser.add_argument("--sources", required=True, type=Path, help="the Yomitan ZIPs (their terms)")
    parser.add_argument("--examples", type=int, default=25)
    parser.add_argument("--no-oracle", action="store_true")
    parser.add_argument("--taps-out", type=Path, help="every differing tap, one JSON line each")
    parser.add_argument("-o", "--output", required=True, type=Path)
    args = parser.parse_args()

    corpus = [json.loads(line) for line in args.corpus.read_text(encoding="utf-8").splitlines()]
    rubi = load_rubi(args.rubi)
    yomi = load_yomitan(args.yomitan)
    rubi_keys = vjdict_keys(args.rubi_dict)
    if args.names_dict:
        rubi_keys |= vjdict_keys(args.names_dict)
    yomitan_keys = yomitan_terms(args.sources)
    oracle = None if args.no_oracle else Oracle()

    counts = collections.Counter()
    judged = collections.defaultdict(collections.Counter)
    examples = collections.defaultdict(list)
    per_title = collections.defaultdict(collections.Counter)
    taps_out = args.taps_out.open("w", encoding="utf-8") if args.taps_out else None
    total = 0
    for row in corpus:
        for tap in row["taps"]:
            key = (row["id"], tap)
            if key not in rubi or key not in yomi:
                counts["missing_result"] += 1
                continue
            r, y = rubi[key], yomi[key]
            if r and "error" in r:
                counts["rubi_error"] += 1
                continue
            total += 1
            per_title[row["title"]]["taps"] += 1
            cause, other = classify(row["text"], tap, r, y, rubi_keys, yomitan_keys)
            classes = ([cause] if cause else []) + other
            if cause:
                counts["mismatch"] += 1
                per_title[row["title"]]["mismatch"] += 1
            for c in classes:
                counts[c] += 1
            verdict = None
            if cause and oracle:
                ye = y["entries"][0]["headwords"][0] if y and y["entries"] else None
                rj = oracle.judge(row["id"], row["text"], tap, r["spelling"], r["reading"]) if r else 0
                yj = oracle.judge(row["id"], row["text"], tap, ye["term"], ye["reading"]) if ye else 0
                verdict = ("none" if rj is None and yj is None else
                           "both" if rj and yj else "rubi" if rj else "yomitan" if yj else "neither")
                judged[cause][verdict] += 1
            if classes:
                item = {"id": row["id"], "tap": tap,
                        "text": excerpt(row["text"], tap, (r or {}).get("len") or (y or {}).get("len") or 1),
                        "rubi": describe_rubi(r), "yomitan": describe_yomitan(y)}
                if verdict:
                    item["sudachi_agrees_with"] = verdict
                for c in classes:
                    if len(examples[c]) < args.examples:
                        examples[c].append(item)
                if taps_out:
                    taps_out.write(json.dumps({"classes": classes, **item}, ensure_ascii=False) + "\n")
    if taps_out:
        taps_out.close()

    def per_1000(n):
        return round(1000.0 * n / total, 2) if total else 0.0

    order = ["deinflection_missing", "deinflection_extra", "deinflection_choice", "normalization", "missing_entry",
             "rubi_only_entry", "longest_match", "reading", "kana_kanji", "spelling", "headword", "names"]
    report = {
        "taps": total, "lines": len(corpus),
        "entry_mismatch": {"taps": counts["mismatch"], "per_1000": per_1000(counts["mismatch"])},
        "causes": {c: {"taps": counts[c], "per_1000": per_1000(counts[c]),
                       "sudachi_agrees_with": dict(judged[c])} for c in order},
        "shown_either_way": {c: {"taps": counts[c], "per_1000": per_1000(counts[c])}
                             for c in SHOWN},
        "not_compared": {k: counts[k] for k in ("missing_result", "rubi_error")},
        "per_title": {t: {"taps": v["taps"], "mismatch_per_1000": round(1000.0 * v["mismatch"] / v["taps"], 1)}
                      for t, v in sorted(per_title.items())},
        "examples": {c: examples[c] for c in order + list(SHOWN)},
    }
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    summary = {k: report[k] for k in ("taps", "entry_mismatch")}
    summary["causes"] = {c: v["per_1000"] for c, v in report["causes"].items()}
    summary["shown_either_way"] = {c: v["per_1000"] for c, v in report["shown_either_way"].items()}
    json.dump(summary, sys.stdout, ensure_ascii=False, indent=1)
    print()


if __name__ == "__main__":
    main()
