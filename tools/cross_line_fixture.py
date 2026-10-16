#!/usr/bin/env python3
"""Candidate host fixtures of words a VN's own wrapping split over two lines (core/textjoin.h).

From a line-label pool (realplus-v3 lines_full: kept labels of VNDB PS Vita/PSP screenshots, the
training pool), the text boxes as tools/cross_line_eval.py groups them, without every VN that
vita-vn-ocr reserves (eval/manifests/reserved.json "test" and "dev_sample", test_games.json "all"):
each break the overlay joins and its lookup crosses (vjo-cross-lines), at most one per VN, in a
deterministic order (sha256 of the box). Text only; the rows are reviewed by eye before any is
committed to host/fixtures/cross_lines/split_words.tsv (README there).

  cross_line_fixture.py --tool build/host/vjo-cross-lines --dict DIR --pool lines_full_v3.jsonl \
                        --reserved eval/manifests/reserved.json --test-games eval/manifests/test_games.json \
                        --n 60 -o candidates.tsv
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cross_line_eval as cle  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tool", required=True)
    ap.add_argument("--dict", required=True)
    ap.add_argument("--files", default="plaintext.vjdict")
    ap.add_argument("--pool", required=True)
    ap.add_argument("--reserved", required=True)
    ap.add_argument("--test-games", required=True)
    ap.add_argument("--n", type=int, default=60)
    ap.add_argument("-o", "--out", required=True)
    args = ap.parse_args()

    reserved = json.load(open(args.reserved, encoding="utf-8"))
    banned = set(reserved.get("test", {})) | set(reserved.get("dev_sample", {}))
    banned |= set(json.load(open(args.test_games, encoding="utf-8"))["all"])
    boxes = []
    for box_id, lines in cle.realplus_boxes(args.pool):
        vn = box_id.split(":")[1].split("_")[0]
        if vn in banned or len(lines) < 2 or not all(t for t, _, _ in lines):
            continue
        boxes.append((hashlib.sha256(box_id.encode()).hexdigest(), box_id, vn, [t for t, _, _ in lines]))
    boxes.sort()
    texts = ["\n".join(lines) for _, _, _, lines in boxes]
    rows = cle.tool_rows(args.tool, args.dict, args.files, texts)
    seen, out = set(), []
    for (_, box_id, vn, lines), breaks in zip(boxes, rows):
        if vn in seen:
            continue
        for k, (joined, span) in enumerate(breaks):
            if joined and span:
                text = "\n".join(lines)
                out.append((box_id, vn, text.replace("\n", "\\n"), span[0], span[1], text[span[0]:span[1]].replace("\n", "|")))
                seen.add(vn)
                break
        if len(out) >= args.n:
            break
    with open(args.out, "w", encoding="utf-8") as f:
        f.write("# id\tvn\ttext\tstart\tend\tspan (| the break)\n")
        for r in out:
            f.write("\t".join(str(x) for x in r) + "\n")
    print(f"{len(out)} candidates from {len(boxes)} boxes ({len(banned)} VNs excluded)", file=sys.stderr)


if __name__ == "__main__":
    main()
