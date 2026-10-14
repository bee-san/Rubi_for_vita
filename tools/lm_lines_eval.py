#!/usr/bin/env python3
"""The overlay's beam + LM decode against vita-vn-ocr's own, on development regions.

Runs the overlay's adapter (`vjo-ocr --backend vocr --lm LM --lines`: the line
finder, the pinned recognizer and the LM decoder exactly as the game worker runs
them) on vita-vn-ocr's linefind development regions (vita_dev, psp_dev: tight,
loose and empty; the reserved VNs skipped as tools/collect_word_lookup_lines.py
skips them), then reads every found line's own crop again through vita-vn-ocr's
Python binding of the same runtime (`runtime/pyvocr.py`: `Vocr.run_topk` and
`VocrLM.decode` with the overlay's settings) and checks the two agree: the
greedy text, the LM text and the confidence. It reports, per model and LM, the
lines decoded and changed, the confidence spread, and the decode time (host).

With --cascade MODEL (the confidence cascade, `vjo-ocr --cascade`, a ceiling the
host never reaches), a line under --conf is read again by the larger model on
the same crop and decoded with the same LM, and that reading is the one shown:
the reference does the same with pyvocr, and the two must agree on which lines
were read again, the text shown and its confidence.

The output (--out) holds counts, flags and times only: no pixels and no text.

  python3 tools/lm_lines_eval.py --ocr build/host/vjo-ocr --models DIR (with the .vocr and .vlm files) \\
      --vocr-repo VITA_VN_OCR --shots DIR [--shots DIR ...] [--model M] [--lm L] --out lm.json
"""
import argparse
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import collect_word_lookup_lines as cw  # noqa: E402 (the shared reservation rules)

# core/vocr_ocr.c lm_opts(): one setting for every model; the bonus per tier
BETA = {"vocr_lm_o3_500k.vlm": 3000, "vocr_lm_o3_1000k.vlm": 3500, "vocr_lm_o3_2000k.vlm": 3000,
        "vocr_lm_o3_4000k.vlm": 3500}
LM_EOS = 2


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ocr", required=True)
    ap.add_argument("--models", required=True, type=Path)
    ap.add_argument("--vocr-repo", required=True, type=Path, help="vita-vn-ocr checkout (runtime with vocr_lm.c)")
    ap.add_argument("--shots", action="append", required=True)
    ap.add_argument("--model", action="append")
    ap.add_argument("--lm", action="append")
    ap.add_argument("--limit", type=int, default=0, help="regions per manifest (0: all)")
    ap.add_argument("--cascade", help="the cascade's larger model (VOCR4_W20_w8.vocr)")
    ap.add_argument("--conf", type=int, default=700, help="the cascade's threshold (1/1000)")
    ap.add_argument("--out", type=Path)
    args = ap.parse_args(argv)
    import numpy as np  # noqa: PLC0415
    from PIL import Image  # noqa: PLC0415
    sys.path.insert(0, str(args.vocr_repo / "runtime"))
    from pyvocr import Vocr, VocrLM  # noqa: PLC0415

    test_vns = set(json.loads((args.vocr_repo / "eval/manifests/test_games.json").read_text())["all"])
    reserved, is_test_vn = cw.reservation(args.vocr_repo)
    owner_test = cw.owner_test_folder_vns()
    models = args.model or ["VOCR3_H15_w8.vocr", "VOCR3_FL10_w8.vocr"]
    lms = args.lm or ["vocr_lm_o3_4000k.vlm"]
    every = models + ([args.cascade] if args.cascade and args.cascade not in models else [])
    ref = {m: Vocr(str(args.models / m), mode="w8i", max_width=1024) for m in every}
    dec = {(m, l): VocrLM(ref[m], str(args.models / l), beam=8, topk=8, alpha=500, beta=BETA[l], gate=8000,
                          flags=LM_EOS, bracket=8000) for m in every for l in lms}
    report = {f"{m} + {l}": {"regions": 0, "lines": 0, "decoded": 0, "changed": 0, "greedy_mismatch": 0,
                             "lm_mismatch": 0, "conf_mismatch": 0, "low_conf_lines": 0, "lm_us": 0, "load_us": [],
                             "conf_hist": [0] * 11, "cascade_lines": 0, "cascade_changed": 0,
                             "cascade_mismatch": 0, "cascade_us": 0} for m in models for l in lms}
    skipped = {}
    with tempfile.TemporaryDirectory(prefix="vjo-lm-") as tmp:
        raw, lines_path = Path(tmp) / "region.rgba", Path(tmp) / "lines.jsonl"
        for name in cw.DEV_MANIFESTS:
            rows = [json.loads(x) for x in (args.vocr_repo / "eval/linefind/manifests" / name).read_text().splitlines()
                    if x.strip()]
            done = 0
            for row in rows:
                why = cw.excluded(row, reserved, test_vns, is_test_vn, owner_test)
                if why:
                    skipped.setdefault(row["vn"], why)
                    continue
                if args.limit and done >= args.limit:
                    break
                shot = next((Path(d) / row["shot"] for d in args.shots if (Path(d) / row["shot"]).exists()), None)
                if shot is None or hashlib.sha256(shot.read_bytes()).hexdigest() != row["sha256"]:
                    raise SystemExit(f"lm: {row['shot']} missing or changed")
                img = Image.open(shot).convert("RGBA")
                x0, y0, x1, y1 = (int(v) for v in row["region"])
                x0, y0, x1, y1 = max(0, x0), max(0, y0), min(img.width, x1), min(img.height, y1)
                if x1 - x0 < 8 or y1 - y0 < 8 or x1 - x0 > 960 or y1 - y0 > 544:
                    continue
                done += 1
                crop = img.crop((x0, y0, x1, y1))
                raw.write_bytes(crop.tobytes())
                a = np.asarray(crop.convert("RGB")).astype(np.int32)
                grey = ((77 * a[..., 0] + 150 * a[..., 1] + 29 * a[..., 2]) >> 8).astype(np.uint8)
                for m in models:
                    for l in lms:
                        r = report[f"{m} + {l}"]
                        casc = args.cascade if args.cascade and args.cascade != m else None
                        extra_args = (["--cascade", casc, "--cascade-conf", str(args.conf), "--cascade-ms", "600000"]
                                      if casc else [])
                        run = subprocess.run([args.ocr, "--backend", "vocr", "--model", m, "--lm", l, *extra_args,
                                              "--lines", str(lines_path), str(args.models), str(crop.width),
                                              str(crop.height), str(raw)], capture_output=True, text=True, timeout=300)
                        st = dict(kv.split("=", 1) for ln in run.stderr.splitlines()
                                  if ln.startswith(("lm=", "cascade=")) for kv in ln.split())
                        r["cascade_us"] += int(st.get("cascade_us", 0))
                        r["regions"] += 1
                        r["lm_us"] += int(st.get("lm_us", 0))
                        r["load_us"].append(int(st.get("lm_load_us", 0)))
                        for ln in (json.loads(x) for x in lines_path.read_text(encoding="utf-8").splitlines() if x):
                            if ln["blank"]:
                                continue
                            r["lines"] += 1
                            x, y, w, h = ln["box"]
                            g = grey[y:y + h, x:x + w]
                            out = ref[m].run_topk(g, 8, invert=bool(ln["dark"]))
                            want_lm = dec[(m, l)].decode(out["topv"], out["topc"], out["blank"]) if out["text"] else ""
                            conf = min(1000, max(0, int(out["conf_min"] * 1000 + 0.5))) if out["text"] else 1000
                            r["greedy_mismatch"] += out["text"] != ln["greedy"]
                            if casc and conf < args.conf:  # read again: the larger model's reading is shown
                                big = ref[casc].run_topk(g, 8, invert=bool(ln["dark"]))
                                shown = (dec[(casc, l)].decode(big["topv"], big["topc"], big["blank"])
                                         if big["text"] else "")
                                big_conf = min(1000, max(0, int(big["conf_min"] * 1000 + 0.5))) if big["text"] else 1000
                                r["cascade_lines"] += 1
                                r["cascade_changed"] += shown != want_lm
                                r["cascade_mismatch"] += (shown != ln["text"] or not ln["flags"] & 0x0400
                                                          or abs((ln["flags"] & 0x3FF) - big_conf) > 1)
                                continue
                            r["cascade_mismatch"] += bool(casc and ln["flags"] & 0x0400)
                            r["lm_mismatch"] += want_lm != ln["text"]
                            if ln["flags"] & 0x4000:
                                r["decoded"] += 1
                                r["changed"] += bool(ln["flags"] & 0x8000)
                                r["conf_mismatch"] += abs((ln["flags"] & 0x3FF) - conf) > 1
                                r["conf_hist"][min(10, (ln["flags"] & 0x3FF) // 100)] += 1
                                r["low_conf_lines"] += (ln["flags"] & 0x3FF) < 700
    for r in report.values():
        loads = sorted(r.pop("load_us"))
        r["lm_load_us_median"] = loads[len(loads) // 2] if loads else 0
    doc = {"what": __doc__.split("\n\n")[0], "manifests": list(cw.DEV_MANIFESTS), "reserved_vns_skipped": len(skipped),
           "host": "times: host µs (shared box); the A9 is not measured", "models": report}
    text = json.dumps(doc, indent=1, ensure_ascii=False) + "\n"
    if args.out:
        args.out.write_text(text, encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
