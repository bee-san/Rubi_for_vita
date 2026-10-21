#!/usr/bin/env python3
"""What the overlay's text normalisation (core/textnorm.c) does to recognized dev lines.

The inputs are vita-vn-ocr's development score files: per-line readings of the
plain-C runtime in W8I, console polarity (view `rv`), of VOCR3's and VOCR4's
selected FL10/H15/W20 on teacher_val (1,643 lines) and devnative v3 (986 crops,
strict and tv_native, ink and GRAD crops):

  $HERD/shared/artifacts/synth3/vocr3/select/<SIZE>/*.json        (VOCR3)
  $HERD/shared/artifacts/vocr4-train/select/<SIZE>/scores/*.json  (VOCR4)

Every reading goes through `vjo-textnorm` (the overlay's own normaliser, built
from core/textnorm.c). Reported per model, over all lines:

- CER with vita-vn-ocr's scorer (eval/metrics.py: norm() on both sides, then
  Levenshtein over the reference characters), before and after;
- CER on the exact text (no folding at all), before and after: the forms a reader
  sees, against the references' convention (which is NFKC's);
- each rule's changes, and for the ー / 一 rules the changed characters that
  the alignment with the reference makes right or wrong.

With --corpus, also what the ー / 一 rules do to CORRECT text, where every
change is a wrong one: vita-vn-ocr's training text ($HERD/shared/data/corpus,
dialogue, narration and wiki, from Aozora and Wikipedia), each line as it is and
wrapped every 20 and 26 characters as a text box wraps it (the worker normalises
each recognized line alone, so a word cut by the wrap ends a line). Reported:
the changes per 100k characters and the contexts they fall in (character
classes, no text).

No protected set is read (the score files hold dev cohorts only); the output
holds counts only, no text.

  python3 tools/textnorm_eval.py --tool build/host/vjo-textnorm --vocr-repo VITA_VN_OCR \\
      [--herd /local/home/skerraut/herd-vita-ocr] [--corpus] --out docs/benchmarks/textnorm-20261014.json
"""
import collections
import argparse
import json
import subprocess
import sys
from pathlib import Path

VOCR3 = {"FL10": "rp_FL10_C_mix_pre128k_seed1", "H15": "rp_H15_C_mix_seed2", "W20": "rp_W20_C_mix_seed1"}
VOCR4 = {"FL10": "v4c_FL10_seed1", "H15": "v4c_H15_seed1", "W20": "v4m_W20_seed2"}
VOCR3_FILES = ("teacher_val", "devnative_strict_ink", "devnative_strict_grad", "devnative_tv_native_ink",
               "devnative_tv_native_grad")
VOCR4_PREFIX = {"teacher_val": "tv", "devnative_strict_ink": "dn_strict_ink",
                "devnative_strict_grad": "dn_strict_grad", "devnative_tv_native_ink": "dn_tv_native_ink",
                "devnative_tv_native_grad": "dn_tv_native_grad"}
RULES = ("width", "tilde", "dash", "ellipsis", "long_vowel", "kanji_one", "counter")


def lines_of(herd, family, size, run):
    """[(cohort, ref, pred)] of one model on every dev cohort."""
    out = []
    for cohort in VOCR3_FILES:
        if family == "VOCR3":
            path = herd / "shared/artifacts/synth3/vocr3/select" / size / f"{cohort}.json"
        else:
            path = herd / "shared/artifacts/vocr4-train/select" / size / "scores" / f"{VOCR4_PREFIX[cohort]}_{run}.json"
        d = json.loads(path.read_text(encoding="utf-8"))
        preds = d["models"][run]["pred"]["rv"]
        if len(preds) != len(d["lines"]):
            raise SystemExit(f"{path}: {len(preds)} readings for {len(d['lines'])} lines")
        out += [(cohort, ln["ref"], p) for ln, p in zip(d["lines"], preds)]
    return out


def normalise(tool, texts):
    """vjo-textnorm over every text: (normalised, {rule: changes})."""
    for t in texts:
        if "\n" in t or "\t" in t:
            raise SystemExit("a reading holds a line break or a tab")
    run = subprocess.run([tool], input="".join(t + "\n" for t in texts), capture_output=True, text=True,
                         encoding="utf-8", check=True)
    out = []
    for row in run.stdout.split("\n")[:len(texts)]:
        text, counts = row.rsplit("\t", 1)
        out.append((text, dict(zip(RULES, (int(v) for v in counts.split())))))
    if len(out) != len(texts):
        raise SystemExit(f"vjo-textnorm returned {len(out)} lines for {len(texts)}")
    return out


MARKS = ("一", "ー")


def char_class(c):
    """The class of a neighbour of a changed mark (no text in the report)."""
    if c is None:
        return "end"
    o = ord(c)
    if c in MARKS:
        return c
    if 0x3041 <= o <= 0x3096 or o in (0x309D, 0x309E):
        return "hira-small" if c in "ぁぃぅぇぉっゃゅょゎ" else "hira-counter" if c in "ゕゖ" else "hira"
    if 0x30A1 <= o <= 0x30FA or o in (0x30FD, 0x30FE):
        return "kata-small" if c in "ァィゥェォッャュョヮ" else "kata-counter" if c in "ヵヶ" else "kata"
    if 0x4E00 <= o <= 0x9FFF or 0x3400 <= o <= 0x4DBF or 0xF900 <= o <= 0xFAFF or c in "々〆":
        return "kanji"
    return "mark" if c in "。、!?！？…‥」』~〜～・:：,，" else "other"


def correct_text(tool, corpus):
    """The ー / 一 changes on correct text, whole lines and wrapped at 20 and 26 characters."""
    out = {}
    for name in ("dialogue", "narration", "wiki"):
        text = [t.replace("\t", " ") for t in (corpus / f"{name}.txt").read_text(encoding="utf-8").split("\n") if t]
        for width in (0, 20, 26):
            lines = text if not width else [t[k:k + width] for t in text for k in range(0, len(t), width)]
            s = {"lines": len(lines), "chars": sum(map(len, lines)), "一": sum(t.count("一") for t in lines),
                 "ー": sum(t.count("ー") for t in lines), "一->ー": 0, "ー->一": 0}
            ctx = collections.Counter()
            for src, (dst, counts) in zip(lines, normalise(tool, lines)):
                if not (counts["long_vowel"] or counts["kanji_one"]):
                    continue
                # the forms pass can shorten the text (... is …) but never adds or drops a mark:
                # the marks pair up in order
                at = [i for i, c in enumerate(src) if c in MARKS]
                now = [c for c in dst if c in MARKS]
                if len(at) != len(now):
                    raise SystemExit("vjo-textnorm added or dropped a ー / 一")
                for i, c in zip(at, now):
                    if src[i] == c:
                        continue
                    s[f"{src[i]}->{c}"] += 1
                    j = i + 1
                    while j < len(src) and src[j] in MARKS:
                        j += 1
                    ctx[f"{src[i]}->{c}: {char_class(src[i - 1] if i else None)} _ "
                        f"{char_class(src[j] if j < len(src) else None)}"] += 1
            s["wrong_per_100k_chars"] = round(1e5 * (s["一->ー"] + s["ー->一"]) / max(1, s["chars"]), 2)
            s["contexts"] = dict(ctx.most_common(10))
            out[f"{name}, " + (f"wrapped every {width} characters" if width else "whole lines")] = s
    return out


def lev(a, b):
    if a == b:
        return 0
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


def vowel_effect(metrics, ref, before, after):
    """The ー/一 characters the alignment with the reference reads right, before and after: (gained, lost)."""
    right_b = sum(1 for g, p in metrics.align_text(ref, before) if g == p and g in ("ー", "一"))
    right_a = sum(1 for g, p in metrics.align_text(ref, after) if g == p and g in ("ー", "一"))
    return max(0, right_a - right_b), max(0, right_b - right_a)


def raw_lines(herd, vocr_repo, model_path):
    """[(cohort, ref, raw reading)]: the C runtime in W8I on every dev crop, console polarity, NOT
    normalised (the score files keep norm()'d readings, which hide the forms the console shows)."""
    import numpy as np  # noqa: PLC0415
    sys.path.insert(0, str(vocr_repo / "runtime"))
    from pyvocr import Vocr  # noqa: PLC0415
    rt = Vocr(str(model_path), mode="w8i", max_width=1024)
    out = []
    for cohort in VOCR3_FILES:
        d = json.loads((herd / "shared/artifacts/synth3/vocr3/select/H15" / f"{cohort}.json").read_text(encoding="utf-8"))
        z = np.load(d["dev"]["file"])
        data, offs, shapes = z["data"], z["offsets"], z["shapes"]
        labels = json.loads(str(z["labels"]))
        refs = iter(d["lines"])
        for i, lab in enumerate(labels):
            if not lab:
                continue
            h, w = (int(v) for v in shapes[i])
            ln = next(refs)
            out.append((cohort, ln["ref"], rt.text(data[offs[i]:offs[i + 1]].reshape(h, w))))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tool", required=True, help="vjo-textnorm")
    ap.add_argument("--vocr-repo", required=True, type=Path, help="vita-vn-ocr checkout (eval/metrics.py)")
    ap.add_argument("--herd", type=Path, default=Path("/local/home/skerraut/herd-vita-ocr"))
    ap.add_argument("--raw-model", action="append", default=[], metavar="NAME=FILE",
                    help="also read every dev crop raw with this blob (the console's text before normalisation)")
    ap.add_argument("--corpus", action="store_true",
                    help="also measure the ー / 一 rules on correct text ($HERD/shared/data/corpus)")
    ap.add_argument("--out", type=Path)
    args = ap.parse_args(argv)
    sys.path.insert(0, str(args.vocr_repo / "eval"))
    import metrics  # noqa: PLC0415 (vita-vn-ocr's scorer, the authority)

    sets = [(f"{family}-{size}", run, None) for family, runs in (("VOCR3", VOCR3), ("VOCR4", VOCR4))
            for size, run in runs.items()]
    sets += [(f"raw {name}", None, Path(path)) for name, _, path in (s.partition("=") for s in args.raw_model)]
    report = {}
    for key, run, raw in sets:
        if raw is None:
            family, size = key.split("-")
            rows = lines_of(args.herd, family, size, run)
        else:
            rows = raw_lines(args.herd, args.vocr_repo, raw)
        normed = normalise(args.tool, [p.replace("\n", " ").replace("\t", " ") for _, _, p in rows])
        s = {"lines": len(rows), "ref_chars": 0, "scored_before": 0, "scored_after": 0, "exact_ref_chars": 0,
             "exact_before": 0, "exact_after": 0, "lines_changed": 0, "lines_scored_differently": 0,
             "vowel_right": 0, "vowel_wrong": 0, **{r: 0 for r in RULES}}
        for (cohort, ref, pred), (after, counts) in zip(rows, normed):
            g = metrics.norm(ref)
            s["ref_chars"] += len(g)
            eb, ea = lev(g, metrics.norm(pred)), lev(g, metrics.norm(after))
            s["scored_before"] += eb
            s["scored_after"] += ea
            s["exact_ref_chars"] += len(ref)
            s["exact_before"] += lev(ref, pred)
            s["exact_after"] += lev(ref, after)
            s["lines_changed"] += after != pred
            s["lines_scored_differently"] += eb != ea
            for r in RULES:
                s[r] += counts[r]
            if counts["long_vowel"] or counts["kanji_one"]:
                good, bad = vowel_effect(metrics, ref, pred, after)
                s["vowel_right"] += good
                s["vowel_wrong"] += bad
        s["cer_scored_before"] = round(100 * s["scored_before"] / s["ref_chars"], 3)
        s["cer_scored_after"] = round(100 * s["scored_after"] / s["ref_chars"], 3)
        s["cer_exact_before"] = round(100 * s["exact_before"] / s["exact_ref_chars"], 3)
        s["cer_exact_after"] = round(100 * s["exact_after"] / s["exact_ref_chars"], 3)
        report[key] = {"run": run or str(raw.name), "readings": "raw (C runtime, W8I, rv)" if raw else
                       "the score file's (already norm()'d)", **s}
    doc = {"what": __doc__.split("\n\n")[0], "cohorts": list(VOCR3_FILES), "view": "rv (console polarity)",
           "scorer": "vita-vn-ocr eval/metrics.py norm() + Levenshtein", "models": report}
    if args.corpus:
        doc["correct_text"] = {"what": "ー / 一 changes on correct text (every one is wrong): vita-vn-ocr's "
                               "training corpus", **correct_text(args.tool, args.herd / "shared/data/corpus")}
    text = json.dumps(doc, indent=1, ensure_ascii=False) + "\n"
    if args.out:
        args.out.write_text(text, encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
