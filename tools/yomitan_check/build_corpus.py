#!/usr/bin/env python3
"""The comparison's corpus: real visual-novel lines and the characters a user would tap in them.

Lines are the dialogue of hooked-text pages (vita-vn-ocr's vocr6-ingest page pool: each page's
text as the game drew it, with its speaker), from the train titles of its splits, train pages
only, complete and trusted. A page's dialogue is its text without the speaker's name line, its
wrapped screen lines joined (as a hook or a text hooker gives it to a lookup). Identical
dialogue is kept once. The sample spreads over the titles: every title gets the same share, a
title with fewer lines gives its rest to the others (seeded, so the same inputs give the same
sample).

Taps are the word starts in each line: SudachiPy's longest units (split mode C), less
punctuation and symbols, tokens without kana or kanji, and the parts of an inflected word
(an auxiliary verb after a verb, an adjective or another auxiliary; て/で/ば/ちゃ/じゃ/たり/だり
after one; a dependent いる/しまう/おく… or ない after て/で; the word after a prefix).

  python3 tools/yomitan_check/build_corpus.py --pagepool DIR --splits splits_v1.json \
      [--lines 5000] [--seed 20261026] -o corpus.jsonl --manifest manifest.json

corpus.jsonl (one line per line: id, title, text, taps) stays local: the pool's texts are the
games' scripts. The manifest has the ids, each text's SHA-256 and the counts; it can be
committed. Needs sudachipy and sudachidict_core (their versions go into the manifest).
"""
import argparse
import collections
import hashlib
import importlib.metadata
import json
from pathlib import Path
import random
import re

JAPANESE = re.compile(r"[\u3041-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff\u3005\u3006\u30fc]")
NAME_LINE = re.compile(r"^[【\[［].{1,20}[】\]］]$")
TE = {"て", "で", "ば", "ちゃ", "じゃ", "たり", "だり"}


def dialogue(page):
    """A page's dialogue: its text less the speaker's name line, the screen lines joined."""
    lines = (page.get("text") or "").split("\n")
    names = {page.get("speaker")} | {line["text"] for line in page.get("lines", []) if line.get("kind") == "name"}
    names.discard(None)
    kept = [s for i, s in enumerate(lines) if not (i == 0 and (s in names or NAME_LINE.match(s)))]
    return "".join(s.strip() for s in kept).strip()


def word_starts(tokenizer, mode, text):
    """Code point offsets of the word starts in text (see the module's docstring)."""
    taps, prev = [], None
    for m in tokenizer.tokenize(text, mode):
        pos, surface = m.part_of_speech(), m.surface()
        if pos[0] in ("補助記号", "空白"):
            prev = None
            continue
        if not JAPANESE.search(surface):
            prev = m
            continue
        cont = False
        if prev is not None:
            pp, ps = prev.part_of_speech(), prev.surface()
            predicate = pp[0] in ("動詞", "形容詞", "助動詞") or (pp[0] == "接尾辞" and pp[1] in ("動詞的", "形容詞的"))
            if pos[0] == "助動詞" and predicate:
                cont = True
            elif pos[0] == "助詞" and pos[1] == "接続助詞" and surface in TE and predicate:
                cont = True
            elif pos[0] in ("動詞", "形容詞") and pos[1] == "非自立可能" and pp[0] == "助詞" and ps in ("て", "で"):
                cont = True
            elif pp[0] == "接頭辞":
                cont = True
        if not cont:
            taps.append(m.begin())
        prev = m
    return taps


def allocate(available, total):
    """Equal shares of total over the titles, a title's unused share to the others (water filling)."""
    share = {t: 0 for t in available}
    left = dict(available)
    remaining = total
    while remaining > 0 and any(left.values()):
        open_titles = [t for t in left if left[t] > 0]
        each = max(1, remaining // len(open_titles))
        for t in sorted(open_titles):
            take = min(each, left[t], remaining)
            share[t] += take
            left[t] -= take
            remaining -= take
            if remaining == 0:
                break
    return share


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pagepool", required=True, type=Path)
    parser.add_argument("--splits", required=True, type=Path)
    parser.add_argument("--lines", type=int, default=5000)
    parser.add_argument("--seed", type=int, default=20261026)
    parser.add_argument("-o", "--output", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    args = parser.parse_args()
    from sudachipy import Dictionary, SplitMode
    tokenizer = Dictionary(dict="core").create()  # noqa: the 0.7 API that still works

    splits = json.loads(args.splits.read_text())["titles"]
    pools = collections.OrderedDict()
    seen = set()
    for path in sorted(args.pagepool.glob("*.jsonl")):
        title = path.stem
        if splits.get(title, {}).get("title_split") != "train":
            continue
        for row in path.read_text(encoding="utf-8").splitlines():
            page = json.loads(row)
            if page.get("split") != "train" or not page.get("complete") or page.get("untrusted") or page.get("text_free"):
                continue
            text = dialogue(page)
            if len(text) < 2 or not JAPANESE.search(text) or text in seen:
                continue
            seen.add(text)
            pools.setdefault(title, []).append((f"{title}:{page['page']}", text))
    share = allocate({t: len(v) for t, v in pools.items()}, args.lines)
    chosen = []
    for title, rows in pools.items():
        rng = random.Random(f"{args.seed}:{title}")
        chosen.extend((title, i, t) for i, t in sorted(rng.sample(rows, share[title])))
    out, manifest_rows, taps_total = [], [], 0
    for title, line_id, text in chosen:
        taps = word_starts(tokenizer, SplitMode.C, text)
        if not taps:
            continue
        taps_total += len(taps)
        out.append({"id": line_id, "title": title, "text": text, "taps": taps})
        manifest_rows.append([line_id, hashlib.sha256(text.encode()).hexdigest()[:16], len(text), len(taps)])
    args.output.write_text("".join(json.dumps(r, ensure_ascii=False) + "\n" for r in out), encoding="utf-8")
    pagepool_manifest = args.pagepool / "manifest.json"
    manifest = {
        "what": "tools/yomitan_check corpus: dialogue of vocr6-ingest pagepool/v1 train pages of train titles",
        "inputs": {"pagepool_manifest_sha256": hashlib.sha256(pagepool_manifest.read_bytes()).hexdigest()
                   if pagepool_manifest.is_file() else None,
                   "splits_sha256": hashlib.sha256(args.splits.read_bytes()).hexdigest()},
        "seed": args.seed, "lines": len(out), "taps": taps_total, "titles": len(share),
        "ids_sha256": hashlib.sha256("".join(r["id"] + "\n" for r in out).encode()).hexdigest(),
        "texts_sha256": hashlib.sha256("".join(r["text"] + "\n" for r in out).encode()).hexdigest(),
        "per_title": {t: n for t, n in share.items() if n},
        "tokenizer": {"sudachipy": importlib.metadata.version("sudachipy"),
                      "sudachidict_core": importlib.metadata.version("sudachidict_core"), "split_mode": "C"},
        "rows": "[id, sha256(text)[:16], characters, taps]",
        "ids": manifest_rows,
    }
    args.manifest.write_text(json.dumps(manifest, ensure_ascii=False, indent=0) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in manifest.items() if k != "ids"}, ensure_ascii=False))


if __name__ == "__main__":
    main()
