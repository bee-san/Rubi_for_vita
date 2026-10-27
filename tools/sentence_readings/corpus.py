#!/usr/bin/env python3
"""The test lines of the sentence-readings measurement (reports/sentence-readings.md): real visual-novel lines
that the development lines (tools/yomitan_check/build_corpus.py's corpus, on which the rules were chosen) do not
hold.

They are chosen as build_corpus.py chooses its own: the dialogue of vita-vn-ocr's vocr6-ingest page pool, train
pages of its train titles, complete and trusted, the speaker's line removed, each text once, every title the
same share (seeded); from the lines whose id and text the development corpus has not taken.

  python3 tools/sentence_readings/corpus.py --pagepool POOL/pagepool/v1 --splits POOL/splits_v1.json \\
      --exclude dev-corpus.jsonl [--lines 3000] [--seed 20261027] -o test.jsonl --manifest test-manifest.json

test.jsonl ({"id", "title", "text"} a line) stays local: the texts are the games' scripts. The manifest has the
ids, each text's SHA-256 prefix and the counts, and can be committed.
"""
import argparse
import collections
import hashlib
import json
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "yomitan_check"))
from build_corpus import JAPANESE, allocate, dialogue  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pagepool", required=True, type=Path)
    parser.add_argument("--splits", required=True, type=Path)
    parser.add_argument("--exclude", required=True, type=Path, action="append",
                        help="a corpus (jsonl with id and text) whose lines are left out")
    parser.add_argument("--lines", type=int, default=3000)
    parser.add_argument("--seed", type=int, default=20261027)
    parser.add_argument("-o", "--output", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    args = parser.parse_args()
    excluded_ids, excluded_texts = set(), set()
    for path in args.exclude:
        for row in path.read_text(encoding="utf-8").splitlines():
            if row.strip():
                line = json.loads(row)
                excluded_ids.add(line["id"])
                excluded_texts.add(line["text"])
    splits = json.loads(args.splits.read_text())["titles"]
    pools = collections.OrderedDict()
    seen = set(excluded_texts)
    for path in sorted(args.pagepool.glob("*.jsonl")):
        title = path.stem
        if splits.get(title, {}).get("title_split") != "train":
            continue
        for row in path.read_text(encoding="utf-8").splitlines():
            page = json.loads(row)
            if page.get("split") != "train" or not page.get("complete") or page.get("untrusted") or page.get("text_free"):
                continue
            text = dialogue(page)
            line_id = f"{title}:{page['page']}"
            if len(text) < 2 or not JAPANESE.search(text) or text in seen or line_id in excluded_ids:
                continue
            seen.add(text)
            pools.setdefault(title, []).append((line_id, text))
    share = allocate({t: len(v) for t, v in pools.items()}, args.lines)
    out = []
    for title, rows in pools.items():
        rng = random.Random(f"{args.seed}:{title}")
        out.extend({"id": i, "title": title, "text": t} for i, t in sorted(rng.sample(rows, share[title])))
    args.output.write_text("".join(json.dumps(r, ensure_ascii=False) + "\n" for r in out), encoding="utf-8")
    pagepool_manifest = args.pagepool / "manifest.json"
    manifest = {
        "what": "tools/sentence_readings test lines: dialogue of vocr6-ingest pagepool/v1 train pages of train "
                "titles, less the development corpus",
        "inputs": {"pagepool_manifest_sha256": hashlib.sha256(pagepool_manifest.read_bytes()).hexdigest()
                   if pagepool_manifest.is_file() else None,
                   "splits_sha256": hashlib.sha256(args.splits.read_bytes()).hexdigest(),
                   "excluded": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in args.exclude}},
        "seed": args.seed, "lines": len(out), "titles": sum(1 for n in share.values() if n),
        "characters": sum(len(r["text"]) for r in out),
        "ids_sha256": hashlib.sha256("".join(r["id"] + "\n" for r in out).encode()).hexdigest(),
        "texts_sha256": hashlib.sha256("".join(r["text"] + "\n" for r in out).encode()).hexdigest(),
        "per_title": {t: n for t, n in share.items() if n},
        "rows": "[id, sha256(text)[:16], characters]",
        "ids": [[r["id"], hashlib.sha256(r["text"].encode()).hexdigest()[:16], len(r["text"])] for r in out],
    }
    args.manifest.write_text(json.dumps(manifest, ensure_ascii=False, indent=0) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in manifest.items() if k not in ("ids", "per_title")}, ensure_ascii=False))


if __name__ == "__main__":
    main()
