#!/usr/bin/env python3
"""Dialogue CER of the overlay's line reading on vita-vn-ocr's development line crops.

For each recognizer it scores the greedy text and the beam + LM text of each pinned LM tier, decoded
with core/vocr_ocr.c's lm_opts(): beam 8, top-K 8, the tier's alpha, beta and line flags, gate 8, bracket 8.
It also scores the confidence the game worker records for each line (conf_min, in 1/1000) and, with
--cascade, the overlay's confidence cascade (core/vocr_ocr.c): a line whose confidence is under --conf
is read again by a larger recognizer, decoded with the same LM, and that reading is shown. The sweep
also reports the alternative rule that keeps the larger reading only when it is more confident.

The decode is the overlay's own C code (runtime/vocr.c's top-K extras and runtime/vocr_lm.c) through
runtime/pyvocr.py; tools/lm_lines_eval.py checks that the overlay's adapter gives the same text on every
dev region.

The lines are vita-vn-ocr eval/ctclm.py's development sets: teacher_val, and the devnative v3 ink and
GRAD crops. The owner pool is not read. Only dialogue lines are scored, using ctclm.py's rule (name
lines and UI labels are set apart). Scoring is eval/metrics.py: norm() on both sides, then Levenshtein
over the reference characters. Times are host us per line, for this shared box; the A9 is not measured.

The output holds counts and rates only: no text, no pixels.

  python3 tools/lm_cer_eval.py --vocr-repo VITA_VN_OCR --models DIR [--model M ...] [--lm L ...] \\
      [--cascade VOCR4_W20_w8.vocr --conf 700] --out lm-cer.json
"""
import argparse
import json
import sys
import time
from pathlib import Path

from evallib import LM_BOS, LM_MODEL_SETTINGS, LM_SETTINGS, lm_settings  # the worker's LM decoder settings

# the four pinned tiers (tools/prepare_ocr.py, the default); ctclm.2's old 1000k / 4000k only by --lm
PINNED = ("vocr_lm_o3_500k.vlm", "vocr_lm6_o3_1000k.vlm", "vocr_lm_o3_2000k.vlm", "vocr_lm6_o3_4000k.vlm")
SETS = ("tv", "dn_ink", "dn_grad")


def conf_of(out):
    """The game worker's confidence: conf_min in 1/1000, 1000 for a line with no character."""
    return min(1000, max(0, int(out["conf_min"] * 1000 + 0.5))) if out["text"] else 1000


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--vocr-repo", required=True, type=Path, help="vita-vn-ocr with runtime/vocr_lm.c, eval/ctclm.py")
    ap.add_argument("--models", required=True, type=Path, help="directory with the .vocr and .vlm files")
    ap.add_argument("--model", action="append", help="recognizer (default VOCR3 H15 and FL10)")
    ap.add_argument("--lm", action="append", choices=sorted(LM_SETTINGS),
                    help="LM file (default the four pinned tiers; ctclm.2's old 1000k/4000k by name)")
    ap.add_argument("--cascade", help="the larger recognizer a low-confidence line is read again with")
    ap.add_argument("--conf", type=int, default=700, help="the cascade's confidence threshold (1/1000)")
    ap.add_argument("--sweep", default="500,600,700,800,900", help="other thresholds to report the cascade at")
    ap.add_argument("--sets", default=",".join(SETS))
    ap.add_argument("--out", type=Path)
    args = ap.parse_args(argv)
    sys.path.insert(0, str(args.vocr_repo / "eval"))
    sys.path.insert(0, str(args.vocr_repo / "runtime"))
    import ctclm  # noqa: PLC0415 (vita-vn-ocr: the development sets and their dialogue rule)
    from metrics import lev, norm  # noqa: PLC0415 (vita-vn-ocr's scorer)
    from pyvocr import Vocr, VocrLM  # noqa: PLC0415

    sets = [s for s in args.sets.split(",") if s]
    if not set(sets) <= set(SETS):
        raise SystemExit(f"lm_cer: only the development sets {SETS} are read here")
    rows = [r for s in sets for r in ctclm.load_set(s) if not r["name"]]
    models = args.model or ["VOCR3_H15_w8.vocr", "VOCR3_FL10_w8.vocr"]
    lms = args.lm or list(PINNED)
    every = models + ([args.cascade] if args.cascade and args.cascade not in models else [])
    rt = {m: Vocr(str(args.models / m), mode="w8i", max_width=1024) for m in every}
    dec = {}

    def decoder(reader, lm, job):
        """The decoder of `reader`'s top-K in a job on model `job`: its settings for lm (lm_settings(): the worker
        decodes a cascade's re-reads with the job's model's)."""
        alpha, beta, flags = lm_settings(job, lm)
        key = (reader, lm, alpha, beta, flags)
        if key not in dec:
            dec[key] = VocrLM(rt[reader], str(args.models / lm), beam=8, topk=8, alpha=alpha, beta=beta, gate=8000,
                              flags=flags, bracket=8000)
        return dec[key]

    def read(m, img, job=None):
        """(per decoder the text, the confidence, frames, host us of the run, host us per LM decode); job: the model
        whose job reads (the cascade's re-reads), else m."""
        t0 = time.perf_counter()
        out = rt[m].run_topk(img, 8)
        run_us = (time.perf_counter() - t0) * 1e6
        texts, dec_us = {"greedy": out["text"]}, {}
        for lm in lms:
            t1 = time.perf_counter()
            texts[lm] = decoder(m, lm, job or m).decode(out["topv"], out["topc"], out["blank"]) if out["text"] else ""
            dec_us[lm] = (time.perf_counter() - t1) * 1e6
        return texts, conf_of(out), out["frames"], run_us, dec_us

    report = {}
    ref_chars = sum(len(r["ref"]) for r in rows)
    cascade_reads = {}
    for m in every:
        cascade_reads[m] = [read(m, r["img"]) for r in rows]
    for m in models:
        reads = cascade_reads[m]
        s = {"lines": len(rows), "ref_chars": ref_chars, "errors": {}, "cer": {}, "lines_changed_by_lm": {},
             "host_us_per_line": round(sum(x[3] for x in reads) / len(rows), 1),
             "host_lm_us_per_line": {lm: round(sum(x[4][lm] for x in reads) / len(rows), 1) for lm in lms}}
        for key in ["greedy", *lms]:
            e = sum(lev(norm(x[0][key]), r["ref"]) for x, r in zip(reads, rows))
            s["errors"][key] = e
            s["cer"][key] = round(100 * e / ref_chars, 3)
            if key != "greedy":
                s["lines_changed_by_lm"][key] = sum(x[0][key] != x[0]["greedy"] for x in reads)
        by_set = {}
        for name in sets:
            idx = [i for i, r in enumerate(rows) if r["set"] == name]
            n = sum(len(rows[i]["ref"]) for i in idx)
            by_set[name] = {"lines": len(idx), "ref_chars": n,
                            **{key: round(100 * sum(lev(norm(reads[i][0][key]), rows[i]["ref"]) for i in idx) / max(1, n), 3)
                               for key in ["greedy", *lms]}}
        s["cer_by_set"] = by_set
        # the confidence as the cascade's trigger, on the greedy reading
        wrong = [lev(norm(x[0]["greedy"]), r["ref"]) > 0 for x, r in zip(reads, rows)]
        hist = [0] * 11
        for x in reads:
            hist[min(10, x[1] // 100)] += 1
        flagged = [x[1] < args.conf for x in reads]
        s["confidence"] = {
            "hist_per_100": hist, "threshold": args.conf, "flagged": sum(flagged),
            "flagged_share": round(sum(flagged) / len(rows), 4),
            "lines_with_error": sum(wrong),
            "errors_caught": sum(f and w for f, w in zip(flagged, wrong)),
            "recall": round(sum(f and w for f, w in zip(flagged, wrong)) / max(1, sum(wrong)), 4),
            "precision": round(sum(f and w for f, w in zip(flagged, wrong)) / max(1, sum(flagged)), 4)}
        if args.cascade and args.cascade != m:
            big = cascade_reads[args.cascade]
            # m's own settings for its re-reads
            if any(lm_settings(m, lm) != lm_settings(args.cascade, lm) for lm in lms):
                big = [read(args.cascade, r["img"], m) for r in rows]

            def cascade_at(threshold, rule, keys, idx=None):
                """CER per decoder when lines under threshold are read again; rule: keep the larger's
                reading when it is more confident ('higher'), always ('always'), or unless it reads
                nothing where the first read text ('nonempty')."""
                idx = range(len(rows)) if idx is None else idx
                n = sum(len(rows[i]["ref"]) for i in idx)
                out = {}
                for key in keys:
                    e = 0
                    for i in idx:
                        x, b = reads[i], big[i]
                        again = x[1] < threshold
                        use = b if again and (rule == "always" or (rule == "higher" and b[1] > x[1]) or
                                              (rule == "nonempty" and (b[0][key] or not x[0][key]))) else x
                        e += lev(norm(use[0][key]), rows[i]["ref"])
                    out[key] = round(100 * e / max(1, n), 3)
                return out
            sweep = {}
            for t in sorted({args.conf, *(int(v) for v in args.sweep.split(",") if v)}):
                again = [x[1] < t for x in reads]
                sweep[str(t)] = {"lines_read_again": sum(again),
                                 "frames_read_again": sum(b[2] for b, a in zip(big, again) if a),
                                 "higher": cascade_at(t, "higher", ["greedy", lms[-1]]),
                                 "always": cascade_at(t, "always", ["greedy", lms[-1]]),
                                 "nonempty": cascade_at(t, "nonempty", ["greedy", lms[-1]])}
            per_set = {name: {"primary": {k: by_set[name][k] for k in ("greedy", lms[-1])},
                              "cascade": cascade_at(args.conf, "always", ["greedy", lms[-1]],
                                                    [i for i, r in enumerate(rows) if r["set"] == name]),
                              "larger_alone": {key: round(100 * sum(lev(norm(big[i][0][key]), rows[i]["ref"])
                                                                    for i, r in enumerate(rows) if r["set"] == name)
                                                          / max(1, by_set[name]["ref_chars"]), 3)
                                               for key in ("greedy", lms[-1])}}
                       for name in sets}
            c = {"model": args.cascade, "threshold": args.conf, "rule": "the larger model's reading is shown",
                 "lines_read_again": sum(flagged), "sweep": sweep, "by_set": per_set,
                 "frames_primary": sum(x[2] for x in reads),
                 "frames_read_again": sum(b[2] for b, f in zip(big, flagged) if f), "errors": {}, "cer": {}}
            for key in ["greedy", *lms]:
                e = 0
                for x, b, f, r in zip(reads, big, flagged, rows):
                    e += lev(norm((b if f else x)[0][key]), r["ref"])
                c["errors"][key] = e
                c["cer"][key] = round(100 * e / ref_chars, 3)
            # the larger recognizer on every line, for reference
            c["larger_alone_cer"] = {key: round(100 * sum(lev(norm(b[0][key]), r["ref"]) for b, r in zip(big, rows))
                                               / ref_chars, 3) for key in ["greedy", *lms]}
            s["cascade"] = c
        report[m] = s
    doc = {"what": __doc__.split("\n\n")[0], "sets": sets, "dialogue_lines": len(rows), "ref_chars": ref_chars,
           "decoder": {"beam": 8, "topk": 8, "alpha": {lm: LM_SETTINGS[lm][0] for lm in lms},
                       "beta": {lm: LM_SETTINGS[lm][1] for lm in lms}, "gate": 8000,
                       "model_settings": {f"{m} {lm}": list(v) for (m, lm), v in LM_MODEL_SETTINGS.items()
                                          if m in models and lm in lms},
                       "flags": {lm: "BOS" if LM_SETTINGS[lm][2] == LM_BOS else "EOS" for lm in lms}, "bracket": 8000},
           "host": "times: host us (shared box); the A9 is not measured", "models": report}
    text = json.dumps(doc, indent=1, ensure_ascii=False) + "\n"
    if args.out:
        args.out.write_text(text, encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
