#!/usr/bin/env python3
"""Measure the game worker's blank-line check on development screenshots.

Runs the overlay's own local OCR adapter (`vjo-ocr --backend vocr --lines`: the
line finder and the pinned recognizer exactly as the game worker runs them, with
each found line's box, polarity, ink statistics and text) on every dialogue
region of vita-vn-ocr's linefind development manifests (vita_dev, psp_dev: the
tight, loose and empty regions), and labels each found line against the
manifest's hand-marked line boxes:

- text: the found box overlaps a marked line (at least 10% of either box);
- empty: it overlaps none (scene art, the box frame, a "next" icon): what the
  recognizer reads there is never dialogue.

Every region is also run with the check on, recording what the job returns, so
the effect on the joined text and the time is measured end to end.

`--how` renders the screenshots as other display paths would before the region
is cut (the audit-overlay-dialogue-1 transforms): x1 (as captured), x2-nearest
and x2-bilinear (a PSP game's 480x272 frame shown 2x, unfiltered and with the
emulator's bilinear filter; PSP rows only), x2-bil-box1 (bilinear, then a
1-pixel box blur: "smooth graphics"), sub720-bilin and sub640-bilin (a Vita game
rendering at 720x408 or 640x368, upscaled bilinear to 960x544; Vita rows only),
blur1.0 and blur1.5 (a Gaussian blur of that sigma: a soft font or filter).
`--rules` recounts each line's edges in numpy under named rules (the one PR #84
first shipped, and the current one, which must equal the C's count exactly).
Screenshots are checked against the manifest's sha256, cropped to RGBA in a
temporary directory and deleted; the output (`--out`, JSON lines, one per found
line) holds boxes, statistics, decisions, text lengths and SHA-256 of the
recognized text, never pixels or text. The same reserved VNs as
tools/collect_word_lookup_lines.py are skipped (protected sets, CLANNAD and its
relations, the eval test sets, the owner's test folders).

  python3 tools/blank_lines_eval.py --ocr build/host/vjo-ocr --models DIR \\
      --vocr-repo /path/to/vita-vn-ocr --shots DIR [--shots DIR ...] [--how x2-bilinear ...] \\
      [--rules] --out lines.jsonl
  python3 tools/blank_lines_eval.py --summary lines.jsonl
"""
import argparse
import hashlib
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import collect_word_lookup_lines as cw  # noqa: E402 (the shared reservation rules)

DEFAULT_MODELS = ("VOCR3_H15_w8.vocr", "VOCR3_FL10_w8.vocr")
HOWS = ("x1", "x2-nearest", "x2-bilinear", "x2-bil-box1", "sub720-bilin", "sub640-bilin", "blur1.0", "blur1.5")
# (spans, divisor): a pixel is an edge, across (down), when the grey level within `spans` pixels to its
# right (below) differs from its own by at least max(24, contrast / divisor). core/vocr_ocr.c vjo_vocr_ink.
RULES = {"pr84-first": ((1,), 2), "current": ((1, 2), 4)}
BLANK_EDGES = 50


def render(img, how, platform):
    """(the screenshot as the display path shows it, the scale of its coordinates), or (None, 0)."""
    from PIL import Image, ImageFilter  # noqa: PLC0415
    if how == "x1":
        return img, 1.0
    if how.startswith("x2"):
        if platform != "psp":
            return None, 0
        big = img.resize((img.width * 2, img.height * 2), Image.NEAREST if how == "x2-nearest" else Image.BILINEAR)
        return (big.filter(ImageFilter.BoxBlur(1)) if how == "x2-bil-box1" else big), 2.0
    if how in ("sub720-bilin", "sub640-bilin"):
        if platform == "psp":
            return None, 0
        size = (720, 408) if how == "sub720-bilin" else (640, 368)
        return img.resize(size, Image.BILINEAR).resize((960, 544), Image.BILINEAR), 1.0
    if how in ("blur1.0", "blur1.5"):
        return img.filter(ImageFilter.GaussianBlur(float(how[4:]))), 1.0
    raise ValueError(how)


def edges_under(grey, spans, divisor):
    """vjo_vocr_ink's edges per 10,000 pixels of a grey crop (numpy), under one rule."""
    import numpy as np  # noqa: PLC0415
    h, w = grey.shape
    flat = np.sort(grey, axis=None)
    lo, hi = (int(flat[max(1, (flat.size * part + 999) // 1000) - 1]) for part in (20, 980))
    e = max(24, (hi - lo) // divisor)
    g = grey.astype(np.int16)
    across, down = np.zeros((h, w), bool), np.zeros((h, w), bool)
    for s in spans:
        if w > s:
            across[:, :w - s] |= np.abs(g[:, s:] - g[:, :w - s]) >= e
        if h > s:
            down[:h - s, :] |= np.abs(g[s:, :] - g[:h - s, :]) >= e
    return int(across.sum() + down.sum()) * 10000 // (w * h)


def overlap(a, b):
    """Intersection area of two [x0, y0, x1, y1] boxes over each box's own area."""
    ix = max(0, min(a[2], b[2]) - max(a[0], b[0]))
    iy = max(0, min(a[3], b[3]) - max(a[1], b[1]))
    inter = ix * iy
    area_a = max(1, (a[2] - a[0]) * (a[3] - a[1]))
    area_b = max(1, (b[2] - b[0]) * (b[3] - b[1]))
    return inter / area_a, inter / area_b


def label(box, marked):
    """'text' if the found line overlaps a marked line box, else 'empty'."""
    for m in marked:
        oa, ob = overlap(box, m)
        if oa >= 0.10 or ob >= 0.10:
            return "text"
    return "empty"


def run(ocr, models_dir, model, w, h, raw, lines_path, blank):
    cmd = [ocr, "--backend", "vocr", "--model", model, "--lines", str(lines_path)]
    if blank:
        cmd += ["--blank", "on"]
    t0 = time.monotonic()
    proc = subprocess.run(cmd + [str(models_dir), str(w), str(h), str(raw)], capture_output=True, text=True,
                          timeout=300)
    wall = time.monotonic() - t0
    stats = dict(kv.split("=", 1) for kv in proc.stderr.split() if "=" in kv)
    found = [json.loads(x) for x in lines_path.read_text(encoding="utf-8").splitlines() if x.strip()]
    return proc.returncode, proc.stdout.rstrip("\n"), stats, found, wall


def collect(args):
    from PIL import Image  # noqa: PLC0415 (only this mode needs Pillow)
    test_vns = set(json.loads((args.vocr_repo / "eval/manifests/test_games.json").read_text())["all"])
    reserved, is_test_vn = cw.reservation(args.vocr_repo)
    owner_test = cw.owner_test_folder_vns()
    models = args.model or list(DEFAULT_MODELS)
    hows = args.how or ["x1"]
    skipped = {}
    mismatches = 0
    out = open(args.out, "w", encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="vjo-blank-") as tmp:
        raw, lines_path = Path(tmp) / "region.rgba", Path(tmp) / "lines.jsonl"
        for name in cw.DEV_MANIFESTS:
            for row_text in (args.vocr_repo / "eval/linefind/manifests" / name).read_text(encoding="utf-8").splitlines():
                if not row_text.strip():
                    continue
                row = json.loads(row_text)
                why = cw.excluded(row, reserved, test_vns, is_test_vn, owner_test)
                if why:
                    skipped.setdefault(row["vn"], why)
                    continue
                shot = next((Path(d) / row["shot"] for d in args.shots if (Path(d) / row["shot"]).exists()), None)
                if shot is None or hashlib.sha256(shot.read_bytes()).hexdigest() != row["sha256"]:
                    print(f"blank: {row['shot']} missing or changed", file=sys.stderr)
                    return 2
                shown = Image.open(shot).convert("RGBA")
                for how in hows:
                    img, k = render(shown, how, row["set"])
                    if img is None:
                        continue
                    x0, y0, x1, y1 = (int(round(v * k)) for v in row["region"])
                    x0, y0, x1, y1 = max(0, x0), max(0, y0), min(img.width, x1), min(img.height, y1)
                    if x1 - x0 < 8 or y1 - y0 < 8 or x1 - x0 > 960 or y1 - y0 > 544:
                        continue
                    crop = img.crop((x0, y0, x1, y1))
                    raw.write_bytes(crop.tobytes())
                    grey = None
                    if args.rules:
                        import numpy as np  # noqa: PLC0415
                        a = np.asarray(crop).astype(np.int32)
                        grey = ((77 * a[..., 0] + 150 * a[..., 1] + 29 * a[..., 2]) >> 8).astype(np.uint8)
                    marked = [[b[0] * k - x0, b[1] * k - y0, b[2] * k - x0, b[3] * k - y0]
                              for b in (ln["box"] for ln in row["lines"] if not ln.get("ignore"))]
                    ignored = [[b[0] * k - x0, b[1] * k - y0, b[2] * k - x0, b[3] * k - y0]
                               for b in (ln["box"] for ln in row["lines"] if ln.get("ignore"))]
                    mismatches += region_lines(args, models, row, name, how, crop, raw, lines_path, marked, ignored,
                                               grey, out)
                out.flush()
    out.close()
    print(f"blank: {len(skipped)} reserved VNs skipped", file=sys.stderr)
    if mismatches:
        print(f"blank: {mismatches} lines where the numpy count of the current rule is not the C's", file=sys.stderr)
        return 3
    return 0


def region_lines(args, models, row, name, how, crop, raw, lines_path, marked, ignored, grey, out):
    """One region in one display path, each model with and without the check: its lines and the region."""
    mismatches = 0
    for model in models:
        rc, text, stats, found, wall = run(args.ocr, args.models, model, crop.width, crop.height, raw,
                                           lines_path, False)
        rc_b, text_b, stats_b, found_b, wall_b = run(args.ocr, args.models, model, crop.width,
                                                     crop.height, raw, lines_path, True)
        for ln in found:
            x, y, w, h = ln["box"]
            box = [x, y, x + w, y + h]
            lab = label(box, marked)
            if lab == "empty" and label(box, ignored) == "text":
                lab = "ignored"
            ln_text = ln.pop("text")
            rec = {"manifest": name, "id": row["id"], "vn": row["vn"], "tags": row.get("tags", []),
                   "how": how, "model": model, "label": lab, "chars": len(ln_text),
                   "text_sha256": hashlib.sha256(ln_text.encode()).hexdigest()[:16], **ln}
            rec["blank_in_job"] = any(b["index"] == ln["index"] and b["blank"] for b in found_b)
            if grey is not None:
                rec["rules"] = {r: edges_under(grey[y:y + h, x:x + w], *RULES[r]) for r in RULES}
                mismatches += rec["rules"]["current"] != ln["ink"]["edges"]
            print(json.dumps(rec, ensure_ascii=False), file=out)
        region = {"region": row["id"], "vn": row["vn"], "tags": row.get("tags", []), "how": how,
                  "model": model,
                  "rc": rc, "rc_blank": rc_b, "chars": len(text.replace("\n", "")),
                  "chars_blank": len(text_b.replace("\n", "")),
                  "same_text": text == text_b, "recognize_us": int(stats.get("recognize_us", 0)),
                  "recognize_us_blank": int(stats_b.get("recognize_us", 0)),
                  "load_us": int(stats.get("load_us", 0)), "load_us_blank": int(stats_b.get("load_us", 0)),
                  "blank_us": int(stats_b.get("blank_us", 0)), "lines": int(stats.get("lines", 0)),
                  "blank": int(stats_b.get("blank", 0))}
        print(json.dumps(region, ensure_ascii=False), file=out)
    return mismatches


def summary(path):
    lines, regions = [], []
    for x in Path(path).read_text(encoding="utf-8").splitlines():
        rec = json.loads(x)
        (regions if "region" in rec else lines).append(rec)
    report = {}
    groups = sorted({(r.get("how", "x1"), r["model"]) for r in regions}, key=lambda g: (HOWS.index(g[0]), g[1]))
    for how, model in groups:
        ls = [ln for ln in lines if ln["model"] == model and ln.get("how", "x1") == how]
        rs = [r for r in regions if r["model"] == model and r.get("how", "x1") == how]
        by = {}
        for lab in ("text", "empty", "ignored"):
            sel = [ln for ln in ls if ln["label"] == lab]
            rej = [ln for ln in sel if ln["blank"] or ln["blank_in_job"]]
            by[lab] = {"lines": len(sel), "chars": sum(ln["chars"] for ln in sel), "rejected": len(rej),
                       "rejected_chars": sum(ln["chars"] for ln in rej),
                       "rejected_nonempty_text": sum(1 for ln in rej if ln["chars"])}
        text = sorted(ln["ink"]["edges"] for ln in ls if ln["label"] == "text")
        rules = {}
        if ls and all("rules" in ln for ln in ls):
            for r in RULES:
                rules[r] = {lab: {"lines": sum(1 for ln in ls if ln["label"] == lab),
                                  "skipped": sum(1 for ln in ls if ln["label"] == lab and ln["rules"][r] < BLANK_EDGES),
                                  "skipped_chars": sum(ln["chars"] for ln in ls
                                                       if ln["label"] == lab and ln["rules"][r] < BLANK_EDGES)}
                            for lab in ("text", "empty")}
                rules[r]["lowest_text_edges"] = sorted(ln["rules"][r] for ln in ls if ln["label"] == "text")[:3]
        report[f"{how} {model}"] = {
            "how": how, "model": model, "lowest_text_edges": text[:3],
            "regions": len(rs), "vns": len({r["vn"] for r in rs}), "found_lines": len(ls), "by_label": by,
            "regions_text_changed": sum(1 for r in rs if not r["same_text"]),
            "chars_before": sum(r["chars"] for r in rs), "chars_after": sum(r["chars_blank"] for r in rs),
            "recognize_ms_before": round(sum(r["recognize_us"] for r in rs) / 1000, 1),
            "recognize_ms_after": round(sum(r["recognize_us_blank"] for r in rs) / 1000, 1),
            "blank_check_ms": round(sum(r["blank_us"] for r in rs) / 1000, 1),
            "regions_all_blank": sum(1 for r in rs if r["lines"] and r["blank"] == r["lines"]),
        }
        if rules:
            report[f"{how} {model}"]["rules"] = rules
    print(json.dumps(report, indent=1, ensure_ascii=False))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ocr", help="vjo-ocr (built with VJO_WITH_VOCR)")
    ap.add_argument("--models", type=Path, help="directory with the pinned .vocr files")
    ap.add_argument("--vocr-repo", type=Path, help="vita-vn-ocr checkout (manifests, reservation)")
    ap.add_argument("--shots", action="append", help="screenshot directory (repeatable)")
    ap.add_argument("--model", action="append", help="model file name (default VOCR3 H15 and FL10)")
    ap.add_argument("--how", action="append", choices=HOWS, help="display path (repeatable; default x1)")
    ap.add_argument("--rules", action="store_true", help="recount each line's edges under RULES (needs numpy)")
    ap.add_argument("--out", help="JSON lines to write")
    ap.add_argument("--summary", help="summarize a --out file")
    args = ap.parse_args(argv)
    if args.summary:
        return summary(args.summary)
    if not (args.ocr and args.models and args.vocr_repo and args.shots and args.out):
        ap.error("--ocr, --models, --vocr-repo, --shots and --out are required to collect")
    return collect(args)


if __name__ == "__main__":
    sys.exit(main())
