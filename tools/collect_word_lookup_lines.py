#!/usr/bin/env python3
"""Real recognizer output for the word-lookup end-to-end test (host/tests/test_word_lookup.c).

Runs the overlay's own local OCR adapter (`vjo-ocr --backend vocr`: the line
finder and the pinned H15/FL10 recognizer, as the game worker runs them) on the
dialogue regions of vita-vn-ocr's linefind development manifests, and prints
one JSON object per region with the exact text it recognized. The protected
test sets (vita-test, psp-test, fresh native), CLANNAD and every VN in
eval/manifests/test_games.json ("all") or with synth.common.is_test_vn are
never read. Screenshots are checked against the manifest's sha256, converted
to RGBA in a temporary directory and deleted afterwards; only text is kept.

  python3 tools/collect_word_lookup_lines.py --ocr build/host/vjo-ocr --models DIR \\
      --vocr-repo /path/to/vita-vn-ocr --shots DIR [--shots DIR ...] > lines.jsonl

The committed fixture (host/fixtures/word_lookup/vocr_lines.tsv) is a small
selection of these lines; see host/fixtures/word_lookup/README.md.
"""
import argparse
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

DEV_MANIFESTS = ("vita_dev.jsonl", "psp_dev.jsonl")
PROTECTED_SETS = {"vita-test", "psp-test", "vita_test", "psp_test", "native", "fresh-native", "fresh_native",
                  "clannad"}
CLANNAD = {"v4"}


def excluded(row, test_vns, is_test_vn):
    text = " ".join(str(row.get(k, "")) for k in ("set", "id")).lower()
    vn = row.get("vn")
    return (str(row.get("set", "")).lower() in PROTECTED_SETS or vn in CLANNAD or vn in test_vns
            or "clannad" in text or "native" in text or (is_test_vn is not None and is_test_vn(vn)))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ocr", required=True, help="vjo-ocr (built with VJO_WITH_VOCR)")
    ap.add_argument("--models", required=True, type=Path, help="directory with H15_w8.vocr and FL10_w8.vocr")
    ap.add_argument("--vocr-repo", required=True, type=Path, help="vita-vn-ocr checkout")
    ap.add_argument("--shots", action="append", required=True, help="screenshot directory (repeatable)")
    ap.add_argument("--model", action="append", help="model file name (default H15_w8.vocr and FL10_w8.vocr)")
    ap.add_argument("--tag", default="tight", help="manifest region tag to read (default tight)")
    args = ap.parse_args(argv)

    from PIL import Image  # noqa: PLC0415 (only this tool needs Pillow)
    test_vns = set(json.loads((args.vocr_repo / "eval/manifests/test_games.json").read_text())["all"])
    sys.path.insert(0, str(args.vocr_repo))
    try:
        from synth.common import is_test_vn  # noqa: PLC0415
    except ImportError:
        print("collect: synth.common.is_test_vn unavailable (numpy?); run with vita-vn-ocr's venv",
              file=sys.stderr)
        return 2
    models = args.model or ["H15_w8.vocr", "FL10_w8.vocr"]
    digests = {m: hashlib.sha256((args.models / m).read_bytes()).hexdigest() for m in models}
    with tempfile.TemporaryDirectory(prefix="vjo-words-") as tmp:
        raw = Path(tmp) / "region.rgba"
        for name in DEV_MANIFESTS:
            for line in (args.vocr_repo / "eval/linefind/manifests" / name).read_text(encoding="utf-8").splitlines():
                if not line.strip():
                    continue
                row = json.loads(line)
                if excluded(row, test_vns, is_test_vn) or args.tag not in row.get("tags", []):
                    continue
                shot = next((Path(d) / row["shot"] for d in args.shots if (Path(d) / row["shot"]).exists()), None)
                if shot is None:
                    print(f"collect: {row['shot']} not found", file=sys.stderr)
                    return 2
                if hashlib.sha256(shot.read_bytes()).hexdigest() != row["sha256"]:
                    print(f"collect: {shot} does not match the manifest's sha256", file=sys.stderr)
                    return 2
                img = Image.open(shot).convert("RGBA")
                x0, y0, x1, y1 = (int(v) for v in row["region"])
                x0, y0, x1, y1 = max(0, x0), max(0, y0), min(img.width, x1), min(img.height, y1)
                if x1 <= x0 or y1 <= y0:
                    continue
                crop = img.crop((x0, y0, x1, y1))
                raw.write_bytes(crop.tobytes())
                for model in models:
                    run = subprocess.run([args.ocr, "--backend", "vocr", "--model", model, str(args.models),
                                          str(crop.width), str(crop.height), str(raw)],
                                         capture_output=True, text=True, timeout=120)
                    if run.returncode:
                        print(f"collect: {row['id']} {model}: {run.stderr.strip()}", file=sys.stderr)
                        continue
                    print(json.dumps({"manifest": name, "id": row["id"], "vn": row["vn"], "shot": row["shot"],
                                      "sha256": row["sha256"], "region": [x0, y0, x1, y1], "model": model,
                                      "model_sha256": digests[model], "text": run.stdout.rstrip("\n")},
                                     ensure_ascii=False), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
