#!/usr/bin/env python3
"""The finder refinement in the overlay's job, A/B, on vita-vn-ocr's linefind development regions.

Runs the overlay's adapter (`vjo-ocr --backend vocr --lines`: the line finder, the recognizer, the
beam + LM decoder and the confidence cascade exactly as the game worker runs them) on the linefind
development regions (vita_dev, psp_dev; the reserved VNs skipped as tools/collect_word_lookup_lines.py
skips them). Each region is read twice, with `--refine on` and without.

The manifests mark each text line's box by hand but hold no text. So this measures what the
refinement does to the boxes the recognizer reads, against the hand-marked ones:
- the lines read;
- the IoU of each read line with its best hand-marked box (mean, and the share at 0.5 or more);
- the hand-marked lines that some read line covers at IoU 0.5 or more (a box that merged two text
  rows covers neither);
- the regions whose text changed;
- what it costs: the CTC frames the job's model ran (a refined line is shorter, so it is read
  wider), the strips read, the rows the refinement read from memory, and the host time of the
  refinement and of the whole job.

--how renders each screenshot as a display path shows it (tools/blank_lines_eval.py's render()):
x1 as captured; x2-bilinear and x2-nearest a PSP game's 480x272 frame shown 2x, as Adrenaline shows
it on the Vita (the overlay's display hook gets it with the emulator's bilinear filter), the region
and the marked boxes scaled with it (PSP regions only).

Text accuracy with the refinement is vita-vn-ocr #83's measurement (docs/finder-tighten.md: dev and
protected sets, whole dialogue, on 480x272 PSP frames and 960x544 Vita frames). This tool checks that
the overlay's integration moves the boxes as #83 intends, platform by platform and at the overlay's
own PSP scale.

Screenshots are checked against the manifest's sha256 and cropped in a temporary directory. The
output (--out) holds counts and rates only: no pixels and no text.

  python3 tools/refine_eval.py --ocr build/host/vjo-ocr --models DIR --vocr-repo VITA_VN_OCR \\
      --shots DIR [--shots DIR ...] [--model M] [--lm L] [--cascade C] [--how x1 --how x2-bilinear ...] \\
      --out refine.json
"""
import argparse
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import blank_lines_eval as ble  # noqa: E402 (the display paths)
import collect_word_lookup_lines as cw  # noqa: E402 (the shared reservation rules)

HOWS = ("x1", "x2-bilinear", "x2-nearest")


def iou(a, b):
    """Intersection over union of two [x0, y0, x1, y1] boxes."""
    ix = max(0, min(a[2], b[2]) - max(a[0], b[0]))
    iy = max(0, min(a[3], b[3]) - max(a[1], b[1]))
    inter = ix * iy
    union = (a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter
    return inter / union if union > 0 else 0.0


def run(args, crop, raw, lines_path, refine):
    cmd = [args.ocr, "--backend", "vocr", "--model", args.model, "--lines", str(lines_path)]
    if args.lm:
        cmd += ["--lm", args.lm]
    if args.cascade:
        cmd += ["--cascade", args.cascade]
    if refine:
        cmd += ["--refine", "on"]
    proc = subprocess.run(cmd + [str(args.models), str(crop.width), str(crop.height), str(raw)],
                          capture_output=True, text=True, timeout=300)
    stats = dict(kv.split("=", 1) for kv in proc.stderr.split() if "=" in kv)
    boxes = [json.loads(x)["box"] for x in lines_path.read_text(encoding="utf-8").splitlines() if x.strip()]
    return proc.returncode, proc.stdout.rstrip("\n"), stats, [[x, y, x + w, y + h] for x, y, w, h in boxes]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ocr", required=True)
    ap.add_argument("--models", required=True, type=Path)
    ap.add_argument("--vocr-repo", required=True, type=Path, help="vita-vn-ocr checkout (the manifests)")
    ap.add_argument("--shots", action="append", required=True)
    ap.add_argument("--model", default="VOCR3_H15_w8.vocr")
    ap.add_argument("--lm", default="vocr_lm6_o3_1000k.vlm", help="'' for greedy decoding")
    ap.add_argument("--cascade", default="VOCR4_W20_w8.vocr", help="'' for none")
    ap.add_argument("--how", action="append", choices=HOWS, help="display paths (default x1; x2-* PSP only)")
    ap.add_argument("--limit", type=int, default=0, help="regions per manifest (0: all)")
    ap.add_argument("--out", type=Path)
    args = ap.parse_args(argv)
    from PIL import Image  # noqa: PLC0415

    test_vns = set(json.loads((args.vocr_repo / "eval/manifests/test_games.json").read_text())["all"])
    reserved, is_test_vn = cw.reservation(args.vocr_repo)
    owner_test = cw.owner_test_folder_vns()
    groups, skipped = {}, {}

    hows = args.how or ["x1"]

    def group(key):
        def mode():
            return {"lines": 0, "iou_sum": 0.0, "iou_half": 0, "marked_matched": 0, "job_us": 0, "frames": 0,
                    "strips": 0}
        on = mode()
        on.update({"boxes_split": 0, "boxes_capped": 0, "refine_rows": 0, "refine_us": 0})
        return groups.setdefault(key, {"regions": 0, "marked_lines": 0, "text_changed": 0, "failed": 0,
                                       "off": mode(), "on": on})

    with tempfile.TemporaryDirectory(prefix="vjo-refine-") as tmp:
        raw, lines_path = Path(tmp) / "region.rgba", Path(tmp) / "lines.jsonl"
        for name in cw.DEV_MANIFESTS:
            done = 0
            for row_text in (args.vocr_repo / "eval/linefind/manifests" / name).read_text().splitlines():
                if not row_text.strip():
                    continue
                row = json.loads(row_text)
                why = cw.excluded(row, reserved, test_vns, is_test_vn, owner_test)
                if why:
                    skipped.setdefault(row["vn"], why)
                    continue
                if args.limit and done >= args.limit:
                    break
                shot = next((Path(d) / row["shot"] for d in args.shots if (Path(d) / row["shot"]).exists()), None)
                if shot is None or hashlib.sha256(shot.read_bytes()).hexdigest() != row["sha256"]:
                    raise SystemExit(f"refine: {row['shot']} missing or changed")
                base = Image.open(shot).convert("RGBA")
                counted = False
                for how in hows:
                    img, k = ble.render(base, how, row["set"])
                    if img is None:
                        continue
                    x0, y0, x1, y1 = (round(v * k) for v in row["region"])
                    x0, y0, x1, y1 = max(0, x0), max(0, y0), min(img.width, x1), min(img.height, y1)
                    if x1 - x0 < 8 or y1 - y0 < 8 or x1 - x0 > 960 or y1 - y0 > 544:
                        continue
                    counted = True
                    crop = img.crop((x0, y0, x1, y1))
                    raw.write_bytes(crop.tobytes())
                    marked = [[b[0] * k - x0, b[1] * k - y0, b[2] * k - x0, b[3] * k - y0]
                              for b in (ln["box"] for ln in (row.get("lines") or []) if not ln.get("ignore"))]
                    results = {mode: run(args, crop, raw, lines_path, mode == "on") for mode in ("off", "on")}
                    keys = ((row["set"], f"{row['set']} {'/'.join(row.get('tags') or ['-'])}") if how == "x1"
                            else (f"{row['set']} {how}",))
                    for key in keys:
                        g = group(key)
                        g["regions"] += 1
                        g["marked_lines"] += len(marked)
                        g["text_changed"] += results["off"][1] != results["on"][1]
                        for mode, (rc, _, st, boxes) in results.items():
                            m = g[mode]
                            g["failed"] += rc != 0
                            m["lines"] += len(boxes)
                            m["job_us"] += int(st.get("find_us", 0)) + int(st.get("recognize_us", 0))
                            m["frames"] += int(st.get("frames", 0))
                            m["strips"] += int(st.get("strips", 0))
                            for b in boxes:
                                best = max((iou(b, mk) for mk in marked), default=0.0)
                                m["iou_sum"] += best
                                m["iou_half"] += best >= 0.5
                            m["marked_matched"] += sum(1 for mk in marked if any(iou(b, mk) >= 0.5 for b in boxes))
                            if mode == "on":
                                m["boxes_split"] += int(st.get("refine_split", 0))
                                m["boxes_capped"] += int(st.get("refine_capped", 0))
                                m["refine_rows"] += int(st.get("refine_rows", 0))
                                m["refine_us"] += int(st.get("refine_us", 0))
                done += counted
    for g in groups.values():
        for mode in ("off", "on"):
            m = g[mode]
            m["mean_iou"] = round(m["iou_sum"] / max(1, m["lines"]), 4)
            m["iou_half_share"] = round(m["iou_half"] / max(1, m["lines"]), 4)
            m["marked_matched_share"] = round(m["marked_matched"] / max(1, g["marked_lines"]), 4)
            del m["iou_sum"]
        g["frames_ratio"] = round(g["on"]["frames"] / max(1, g["off"]["frames"]), 4)
        g["job_us_ratio"] = round(g["on"]["job_us"] / max(1, g["off"]["job_us"]), 4)
    doc = {"what": __doc__.split("\n\n")[0], "manifests": list(cw.DEV_MANIFESTS), "reserved_vns_skipped": len(skipped),
           "model": args.model, "lm": args.lm or None, "cascade": args.cascade or None, "hows": hows,
           "host": "times: host us (shared box); the A9 is not measured", "groups": dict(sorted(groups.items()))}
    text = json.dumps(doc, indent=1, ensure_ascii=False) + "\n"
    if args.out:
        args.out.write_text(text, encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
