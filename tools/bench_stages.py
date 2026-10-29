#!/usr/bin/env python3
"""The overlay pipeline's stage benchmark: host timings, Cortex-A9 estimates and a job model.

Every stage of an OCR job and of the Shell's lookups runs on the real sources in
host/bench/stage_bench.c (docs/performance.md says what each stage is and what it
leaves out). This script runs it in four ways:

  bench_stages.py smoke --binary build/host/vjo-stage-bench [--models DIR]
      every stage once on the committed inputs; each output checksum must equal the
      recorded one (tools/bench_stages_expected.json): the checksums hash each stage's whole
      output (grey bytes and signature, lines, text, entries and spans, every parsed setting,
      the Anki record's bytes; the log stage's formatted lines, though that stage is a replica
      of the Shell's formatting, not its code), so a speed change that alters an output fails
      here. Stages whose input is absent (weights, the dictionary) are reported skipped.
      ctest runs this (stage_bench_smoke).
  bench_stages.py host --binary build/host/vjo-stage-bench --models DIR --dict DIR [--runs 5] -o host.json
      the median host time of each workload (x86-64; never a Vita figure).
  bench_stages.py a9 --vita-build build/vita --models DIR --dict DIR -o a9.json [--no-cache]
      the stage code as the plugins are built (the Vita build's own libraries and kernel
      objects, arm-vita-eabi-gcc's machine code), run under qemu-arm -cpu cortex-a9 with
      vita-vn-ocr's insn_mix and cache plugins, twice per workload (1 and 3 iterations;
      the difference is one steady-state iteration), costed with vita-vn-ocr's own
      Cortex-A9 model (vita/a9_cycles.py at the pinned commit) as a cycle range at 444 and
      333 MHz. Estimates, not console measurements. The Vita build must be configured
      -DVJO_CORE_LTO=OFF: a release build's core library holds LTO objects ARM_CC cannot link.
  bench_stages.py dev --binary ... --models DIR --manifests DIR --shots DIR [--shots DIR] -o dev.json
                      [--size S|N|L [--first-ms-per-mpx MS]] [--skip-missing]
      the game worker's job on every region of vita-vn-ocr's linefind dev manifests
      (protected sets excluded, as tools/capture_parity.py does), twice: API 19's job and
      main's default one (the stable-text wait on a simulated display at 60 and 30 fps, beam + LM, the
      cascade; refined boxes in the PSP regions): strips (display frames), lines, CTC frames,
      cascade re-reads per job, the inputs of the job model. With --size, the default one is API 54's
      (the text spotter, VOCR6 of that size and the next size up as its cascade, the worker's policy), and
      --first-ms-per-mpx puts the spotter's compute (an A9 estimate per megapixel of the region) on the
      simulated clock of its first pass, so the stable-text wait sees the time it takes.
  bench_stages.py report --host host.json --a9 a9.json [--dev dev.json] -o OUT.json [--md OUT.md]
      the stage table and the end-to-end job model (docs/performance.md).

Environment: VITASDK, ARM_CC (an arm-none-linux-gnueabihf gcc), QEMU_PLUGINS_DIR (the
plugin-enabled qemu: bin/qemu-arm, plugins/libinsn_mix.so, plugins/libcache.so),
VJO_VOCR_SOURCE_DIR (vita-vn-ocr at the pinned commit, for the cost model) and
VJO_VOCR_SPOT_SOURCE_DIR (the text spotter's pin, cmake/VocrOCR.cmake VJO_VOCR_SPOT_COMMIT, when it is not
VJO_VOCR_SOURCE_DIR's commit).
"""
import argparse
import collections
import hashlib
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from evallib import DEV_MANIFESTS, find_shot, protected  # noqa: E402 (the protected sets, never read)

ROOT = Path(__file__).resolve().parent.parent
EXPECTED = ROOT / "tools/bench_stages_expected.json"
SAMPLE = ROOT / "host/samples/vn_textbox.jpg"
TEXTS = ROOT / "host/fixtures/word_lookup/vocr_lines.tsv"
CONFIG = ROOT / "docs/config.example.ini"
FB = (960, 544)
# The sample's dialogue box (host/samples/vn_textbox.jpg, tools/make_test_screens.py): 2 lines.
REGION = (32, 362, 896, 156)
FULL = (0, 0, 960, 544)
# The models the stages were recorded with (bench_stages_expected.json's checksums are theirs): the
# model sizes before VOCR5 (VOCR3-H15 Normal, VOCR3-FL10 Small, VOCR4-W20 Large and Normal's
# cascade), still pinned by name. VOCR5's sizes run the same stages; re-recording the checksums with
# them is a separate measurement.
MODELS = ("VOCR3_H15_w8.vocr", "VOCR3_FL10_w8.vocr", "VOCR4_W20_w8.vocr")
# The game worker's default job (core/config.c defaults): beam + LM decoding with the 1000k LM
# (ocr_lm = 1000k), the confidence cascade with the next size up (ocr_cascade_ms = 2000; here
# VOCR4-W20 beside VOCR3-H15) and the stable-text wait (ocr_settle_ms = 150);
# host/bench/stage_bench.c stage_job's mode bits.
LM = "vocr_lm6_o3_1000k.vlm"
CASCADE = MODELS[2]
JOB_FIRST, JOB_LM, JOB_CASCADE, JOB_SETTLE, JOB_REFINE, JOB_SPOT, JOB_WORKER = 1, 2, 4, 8, 16, 32, 64
JOB_DEFAULT = JOB_LM | JOB_CASCADE | JOB_SETTLE
# API 54's default job (since Rubi 0.10.0): the text spotter finds the lines (ocr_finder = spotter, with its
# weights), and the game worker's request_policy on the defaults (the head gate and the two-level head where a blob
# carries them, the counter re-decode, the short-line probe, ocr_vertical = auto's check); the sizes are VOCR6's,
# each with the next size up as its cascade (game_ocr/main.c cascade_of: Large has none). host/bench/stage_bench.c
# stage_job's mode bits.
SPOT = "textspot8_t64n8c16_w8.vtsp"
VOCR6 = ("VOCR6.2_S_w8.vocr", "VOCR6.2_N_w8g2.vocr", "VOCR6.2_L_w8_2l.vocr")
CASCADE6 = {VOCR6[0]: VOCR6[1], VOCR6[1]: VOCR6[2], VOCR6[2]: None}
JOB_SPOT_DEFAULT = JOB_DEFAULT | JOB_SPOT | JOB_WORKER
DEV_FPS = (60, 30)
DEFAULT_KEYS = ("strips", "lines", "frames", "rows_read", "display_ms", "settle_passes", "lm_lines", "lm_changed",
                "cascade_lines", "cascade_frames", "tries")
CLOCKS = (444, 333)


def W(name, stage, needs=(), mode=0, region=REGION, fmt="abgr", model=None, unit="op", what="", cascade=CASCADE):
    return {"name": name, "stage": stage, "needs": set(needs), "mode": mode, "region": region, "fmt": fmt,
            "model": model, "unit": unit, "what": what, "cascade": cascade}


def job_needs(mode):
    return (("frame", "model") + (("lm",) if mode & JOB_LM else ()) + (("cascade",) if mode & JOB_CASCADE else ()) +
            (("spot",) if mode & JOB_SPOT else ()))


def spot_job(name, model, mode=JOB_SPOT_DEFAULT, region=REGION, what=""):
    """A job of API 54's default path with VOCR6 size `model` and its cascade (none for Large)."""
    casc = CASCADE6[model]
    mode = mode if casc else mode & ~JOB_CASCADE
    return W(name, "job", job_needs(mode), mode=mode, region=region, model=model, unit="job", what=what, cascade=casc)


# The workloads. unit: what one "op" of the stage is (ms per op are reported).
WORKLOADS = [
    W("capture_abgr", "capture", ("frame",), unit="strip",
      what="display hook: a strip of region rows to grey (A8B8G8R8), one rows call per display frame"),
    W("capture_abgr_sig", "capture", ("frame",), mode=1, unit="strip",
      what="the same, first pass: also the capture's scene signature"),
    W("capture_a2b10", "capture", ("frame",), fmt="a2b10", unit="strip", what="the same, A2B10G10R10"),
    W("capture_a1b5", "capture", ("frame",), fmt="a1b5", unit="strip", what="the same, A1B5G5R5 (PSP emulator)"),
    W("capture_full_abgr", "capture", ("frame",), region=FULL, unit="strip",
      what="display hook, full screen (the self-test's stage), A8B8G8R8"),
    W("capture_shot_abgr", "capture", ("frame",), mode=2, region=FULL, unit="strip",
      what="display hook: an Anki screenshot's 64 colour rows of the full frame (API 23), A8B8G8R8"),
    W("scene_sig", "scene_sig", ("frame",), unit="signature",
      what="display hook: the region's change-detection signature (every 8th game frame)"),
    W("scene_update", "scene_update", ("frame",), unit="update",
      what="kernel worker: one signature into the scene tracker (every 64 ms), screen unchanged"),
    W("scene_update_typing", "scene_update", ("frame",), mode=1, unit="update",
      what="the same while text types out (a cell changes every signature)"),
    W("find", "find", ("frame",), model=MODELS[0], unit="region", what="game worker: the line finder, one region"),
    W("recognize_h15", "recognize", ("frame", "model"), model=MODELS[0], unit="line",
      what="game worker: VOCR3-H15 on each found line (resident weights)"),
    W("recognize_fl10", "recognize", ("frame", "model"), model=MODELS[1], unit="line",
      what="game worker: VOCR3-FL10 on each found line (the fallback)"),
    W("recognize_w20", "recognize", ("frame", "model"), model=MODELS[2], unit="line",
      what="game worker: VOCR4-W20 on each found line (the cascade's model, greedy)"),
    W("job_h15", "job", job_needs(JOB_DEFAULT), mode=JOB_DEFAULT, model=MODELS[0], unit="job",
      what="game worker: the whole job on main's defaults (core/vocr_ocr.c: strips, checksums, finder, the "
           "stable-text wait, VOCR3-H15 with beam + 1000k LM, the cascade), resident weights and LM"),
    W("job_h15_first", "job", job_needs(JOB_DEFAULT | JOB_FIRST), mode=JOB_DEFAULT | JOB_FIRST, model=MODELS[0],
      unit="job", what="the same as a game's first job: weights and LM read in 32 KiB pieces and SHA-256 checked"),
    W("job_h15_lm", "job", job_needs(JOB_LM), mode=JOB_LM, model=MODELS[0], unit="job",
      what="the job with beam + LM decoding only (no wait, no cascade)"),
    W("job_h15_greedy", "job", job_needs(0), mode=0, model=MODELS[0], unit="job",
      what="API 19's job: greedy decoding, no wait, no cascade (the previous baseline's job_h15)"),
    W("sha256_h15", "sha256", ("model",), model=MODELS[0], unit="model",
      what="SHA-256 of the VOCR3-H15 weights (1,155,968 B), BearSSL"),
    # API 54's default path (Rubi 0.10.0 on): the text spotter, the VOCR6 sizes, their jobs
    W("spot_textbox", "spot", ("frame", "spot"), unit="region",
      what="game worker: the text spotter (textspot8, the default finder) on the sample's dialogue box, read halved "
           "as from a 960x544 frame, with its boxes' dark flags"),
    W("spot_full", "spot", ("frame", "spot"), region=FULL, unit="region",
      what="the same on the whole 960x544 frame (a game with no region drawn)"),
    W("recognize_vocr6_s", "recognize", ("frame", "model"), mode=1, model=VOCR6[0], unit="line",
      what="game worker: VOCR6-S (Small) on each found line, resident weights, the job's head options"),
    W("recognize_vocr6_n", "recognize", ("frame", "model"), mode=1, model=VOCR6[1], unit="line",
      what="game worker: VOCR6-N (Normal, the default size) on each found line: its head gate and two-level head"),
    W("recognize_vocr6_l", "recognize", ("frame", "model"), mode=1, model=VOCR6[2], unit="line",
      what="game worker: VOCR6-L (Large) on each found line: its two-level head"),
    spot_job("job_vocr6_n", VOCR6[1],
             what="game worker: the whole job on the defaults since API 54 (the spotter finds the lines, the stable-text "
                  "wait, VOCR6-N with beam + 1000k LM, the cascade to VOCR6-L, the worker's policy), resident files"),
    spot_job("job_vocr6_n_first", VOCR6[1], mode=JOB_SPOT_DEFAULT | JOB_FIRST,
             what="the same as a game's first job: the spotter's weights, the model's and the LM read in 32 KiB pieces "
                  "and SHA-256 checked (the cascade's are read at its first doubtful line: none on this sample)"),
    spot_job("job_vocr6_l", VOCR6[2], what="the default job with VOCR6-L (Large: no cascade), resident files"),
    spot_job("job_vocr6_s", VOCR6[0], what="the default job with VOCR6-S (Small, the cascade to VOCR6-N)"),
    spot_job("job_vocr6_n_full", VOCR6[1], region=FULL,
             what="the default job on the whole 960x544 frame (VOCR6-N, the spotter over the full screen)"),
    W("sha256_vocr6_l", "sha256", ("model",), model=VOCR6[2], unit="model",
      what="SHA-256 of the VOCR6-L weights (2,475,680 B), BearSSL: a game's first Large job"),
    W("postproc", "postproc", ("texts",), unit="text",
      what="Shell: the recognized text filtered and trimmed, its first selectable letter"),
    W("lookup_first", "lookup", ("texts", "dict"), unit="lookup",
      what="Shell: the first word's lookup with the bundled dictionary (Auto OCR prepares it)"),
    W("lookup_every", "lookup", ("texts", "dict"), mode=1, unit="lookup",
      what="Shell: a lookup at every selectable letter (Right in Character mode, a tap)"),
    W("config", "config", (), unit="parse", what="Shell: config.ini parsed (every overlay open)"),
    W("jpeg_shot", "jpeg", ("frame",), unit="shot",
      what="game worker: an Anki card's screenshot, the whole frame as a JPEG (core/jpegsw.c, quality 70; API 23)"),
    W("log", "log", (), unit="line", what="Shell: formatting a job's log lines (formatting only)"),
    W("anki", "anki", ("texts", "dict"), unit="card", what="Shell: an Anki card's note and queue record"),
    W("furigana", "furigana", ("texts", "dict"), unit="word",
      what="Shell: a word's reading aligned with its spelling (an Anki card from a queued record with ruby)"),
    W("total_count", "total", ("texts",), unit="record",
      what="Shell: the reading total's count of history.csv (Triangle's first press), 24,512 B windows as the "
           "control thread reads them, per record of 2,000 (docs/ocr-history.md#total-characters-read)"),
    W("total_number", "total_number", (), unit="number",
      what="Shell: the Triangle view's total with its commas (every press)"),
    W("history_scan", "history_scan", ("texts",), unit="read",
      what="Shell: line history's read, this game's lines in history.csv's last 24,512 B (the first Up past the top "
           "row; docs/line-history.md)"),
]


def die(msg):
    print(f"bench_stages: {msg}", file=sys.stderr)
    sys.exit(2)


def frame_file(tmp):
    from PIL import Image
    img = Image.open(SAMPLE).convert("RGBA")
    if img.size != FB:
        die(f"{SAMPLE} is {img.size}, expected {FB}")
    path = Path(tmp) / "frame.rgba"
    path.write_bytes(img.tobytes())
    return path


def plaintext_dir(args):
    d = getattr(args, "dict", None) or os.environ.get("VJO_PLAINTEXT_DIR")
    return Path(d).resolve() if d and (Path(d) / "plaintext.vjdict").exists() else None


def model_path(models, name):
    if not models or not name:
        return None
    p = Path(models) / name
    return p if p.exists() else None


def bench_args(w, frame, models, dictionary, iters, rel=False):
    """argv after the stage name; rel: inputs by base name (the A9 lane's staging dir)."""
    def f(p):
        return Path(p).name if rel else str(p)
    a = [w["stage"], "--iters", str(iters), "--mode", str(w["mode"])]
    if "frame" in w["needs"] or w["stage"] in ("find",):
        x, y, ww, hh = w["region"]
        a += ["--frame", f(frame), "--fb", f"{FB[0]}x{FB[1]}", "--region", f"{x},{y},{ww},{hh}", "--fmt", w["fmt"]]
    if w["model"]:
        a += ["--model-name", w["model"]]
        if "model" in w["needs"]:
            a += ["--model", f(model_path(models, w["model"]))]
    if "lm" in w["needs"]:
        a += ["--lm", f(model_path(models, LM)), "--lm-name", LM]
    if "cascade" in w["needs"]:
        a += ["--cascade", f(model_path(models, w["cascade"])), "--cascade-name", w["cascade"]]
    if "spot" in w["needs"]:
        a += ["--spot", f(model_path(models, SPOT))]
    if "texts" in w["needs"]:
        a += ["--texts", f(TEXTS)]
    if "dict" in w["needs"]:
        a += ["--dict", "dict" if rel else str(dictionary)]
    if w["stage"] == "config":
        a += ["--config", f(CONFIG)]
    return a


def available(w, models, dictionary):
    missing = []
    if "model" in w["needs"] and not model_path(models, w["model"]):
        missing.append(f"weights {w['model']} (--models)")
    if "lm" in w["needs"] and not model_path(models, LM):
        missing.append(f"LM {LM} (--models)")
    if "cascade" in w["needs"] and not model_path(models, w["cascade"]):
        missing.append(f"cascade weights {w['cascade']} (--models)")
    if "spot" in w["needs"] and not model_path(models, SPOT):
        missing.append(f"spotter weights {SPOT} (--models)")
    if "dict" in w["needs"] and not dictionary:
        missing.append("plaintext.vjdict (--dict or VJO_PLAINTEXT_DIR)")
    return missing


def run_json(cmd, cwd=None, env=None, timeout=7200):
    p = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd, env=env, timeout=timeout,
                       stdin=subprocess.DEVNULL, check=False)
    if p.returncode:
        raise RuntimeError(f"{' '.join(map(str, cmd))[:300]}: exit {p.returncode}: {p.stderr[-1500:]}")
    return json.loads(p.stdout.strip().splitlines()[-1])


# ---------------------------------------------------------------- smoke / host

def cmd_smoke(args):
    expected = json.loads(EXPECTED.read_text())["checks"]
    dictionary = plaintext_dir(args)
    failed = 0
    with tempfile.TemporaryDirectory(prefix="vjo-bench-") as tmp:
        frame = frame_file(tmp)
        for w in WORKLOADS:
            missing = available(w, args.models, dictionary)
            if missing:
                print(f"skip {w['name']}: no {', '.join(missing)}")
                continue
            r = run_json([args.binary, *bench_args(w, frame, args.models, dictionary, 1)])
            want = expected.get(w["name"])
            if args.record:
                expected[w["name"]] = r["check"]
                print(f"record {w['name']}: {r['check']}")
            elif want != r["check"]:
                failed += 1
                print(f"FAIL {w['name']}: output checksum {r['check']}, recorded {want}")
            else:
                print(f"ok {w['name']}: {r['check']} {json.dumps(r['counters'])}")
    if args.record:
        EXPECTED.write_text(json.dumps({"note": "output checksums of tools/bench_stages.py's workloads "
                                                "(FNV-1a of each stage's output); a speed change must not alter them",
                                        "checks": expected}, indent=1, sort_keys=True) + "\n")
    return 1 if failed else 0


def cmd_host(args):
    dictionary = plaintext_dir(args)
    out = {"lane": "host", "cpu": host_cpu(), "runs": args.runs, "workloads": {}}
    with tempfile.TemporaryDirectory(prefix="vjo-bench-") as tmp:
        frame = frame_file(tmp)
        for w in WORKLOADS:
            if available(w, args.models, dictionary):
                continue
            rs = [run_json([args.binary, *bench_args(w, frame, args.models, dictionary, args.iters)])
                  for _ in range(args.runs)]
            ns = statistics.median(r["ns_per_iter"] for r in rs)
            r = rs[0]
            ops = max(1, r["ops"])
            out["workloads"][w["name"]] = {"stage": w["stage"], "unit": w["unit"], "ops": r["ops"], "check": r["check"],
                                           "ns_per_iter": ns, "ms_per_op": ns / ops / 1e6, "counters": r["counters"],
                                           "text": r.get("text")}
            print(f"{w['name']:22s} {ns / 1e6:10.3f} ms/iter {ns / ops / 1e6:10.4f} ms/{w['unit']}  {r['counters']}")
    Path(args.out).write_text(json.dumps(out, indent=1, ensure_ascii=False) + "\n")
    return 0


def host_cpu():
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return "unknown"


# ---------------------------------------------------------------- dev regions

def cmd_dev(args):
    from PIL import Image
    rows, excluded = [], 0
    for name in DEV_MANIFESTS:
        for line in (Path(args.manifests) / name).read_text(encoding="utf-8").splitlines():
            if not line.strip():
                continue
            row = json.loads(line)
            if protected(row):
                excluded += 1
                continue
            rows.append((name, row))
    model_name = VOCR6["SNL".index(args.size)] if args.size else args.model
    casc_name = CASCADE6[model_name] if args.size else CASCADE
    model = model_path(args.models, model_name)
    lm, cascade = model_path(args.models, LM), model_path(args.models, casc_name) if casc_name else None
    spot = model_path(args.models, SPOT) if args.size else None
    if not model or not lm or (casc_name and not cascade) or (args.size and not spot):
        die(f"no {model_name}, {LM}, {casc_name} or {SPOT} in --models")
    out = {"model": model_name, "cascade": casc_name, "spot": bool(spot), "regions": [], "excluded": excluded,
           "first_ms_per_mpx": args.first_ms_per_mpx}
    frames = {}
    with tempfile.TemporaryDirectory(prefix="vjo-bench-dev-") as tmp:
        for name, row in rows:
            shot = find_shot(row["shot"], args.shots)
            if shot is None:
                if not args.skip_missing:
                    die(f"{row['shot']} not found in {args.shots}")
                out["missing"] = out.get("missing", 0) + 1
                continue
            if row["shot"] not in frames:
                if row.get("sha256") and hashlib.sha256(shot.read_bytes()).hexdigest() != row["sha256"]:
                    die(f"{shot} does not match the manifest's sha256")
                img = Image.open(shot).convert("RGB")
                if row.get("mask"):
                    import numpy as np
                    a = np.asarray(img).copy()
                    for x0, y0, x1, y1 in row["mask"]:
                        ref = a[max(0, y0 - 6):y0, x0:x1].reshape(-1, 3)
                        a[y0:y1, x0:x1] = np.median(ref, axis=0).astype(np.uint8)
                    img = Image.fromarray(a)
                if img.size[0] > 960 or img.size[1] > 544:
                    die(f"{shot} is larger than a Vita frame")
                raw = Path(tmp) / f"{len(frames):04d}.rgba"
                raw.write_bytes(img.convert("RGBA").tobytes())
                frames[row["shot"]] = (raw, img.size)
            raw, (fw, fh) = frames[row["shot"]]
            x0, y0, x1, y1 = (int(v) for v in row["region"])
            x0, y0, x1, y1 = max(0, x0), max(0, y0), min(fw, x1), min(fh, y1)
            if x1 <= x0 or y1 <= y0:
                continue
            # The kernel crops x to an even pixel (kernel/capture.c capture_crop_of): the same here.
            x0 &= ~1
            base = [args.binary, "job", "--iters", "1", "--frame", str(raw), "--fb", f"{fw}x{fh}",
                    "--region", f"{x0},{y0},{x1 - x0},{y1 - y0}", "--model", str(model), "--model-name", model_name]
            # API 19's job (greedy, no wait), and main's default job: a PSP game's also refines the finder's
            # boxes (ocr_refine_lines = psp; the psp_dev frames stand in for Adrenaline's). With --size, API 54's:
            # the spotter, VOCR6 of the size, its cascade (Large none), the worker's policy.
            r0 = run_json([*base, "--mode", "0"])
            mode = (JOB_SPOT_DEFAULT if args.size else JOB_DEFAULT) | (JOB_REFINE if name.startswith("psp") else 0)
            if not casc_name:
                mode &= ~JOB_CASCADE
            extra = ["--lm", str(lm), "--lm-name", LM]
            if casc_name:
                extra += ["--cascade", str(cascade), "--cascade-name", casc_name]
            if spot:
                extra += ["--spot", str(spot)]
                if args.first_ms_per_mpx:
                    extra += ["--first-us", str(int(args.first_ms_per_mpx * (x1 - x0) * (y1 - y0) / 1000.0))]
            reg = {"manifest": name, "id": row["id"], "w": x1 - x0, "h": y1 - y0,
                   "gt_lines": len(row.get("lines", [])), "mode": mode, "ns": r0["ns_per_iter"]}
            c0 = r0["counters"]
            # API 19's job
            reg.update({"strips": c0["strips"], "lines": c0["lines"], "frames": c0["frames"], "tall": c0["tall"],
                        "rows_read": c0["rows_read"], "strip_rows": c0["strip_rows"]})
            # main's default job, its display at each rate (d60_*, d30_*)
            for fps in DEV_FPS:
                try:
                    r = run_json([*base, "--mode", str(mode), "--fps", str(fps), *extra])
                except RuntimeError as e:  # API 54: a region the spotter takes for vertical text (auto's columns)
                    reg[f"d{fps}_error"] = str(e)[-200:]
                    continue
                c = r["counters"]
                reg.update({f"d{fps}_{k}": c[k] for k in DEFAULT_KEYS})
                reg[f"d{fps}_ns"] = r["ns_per_iter"]
                reg[f"d{fps}_check"] = r["check"]
            if any(f"d{fps}_error" in reg for fps in DEV_FPS):
                out.setdefault("failed", []).append(reg)
                continue
            out["regions"].append(reg)
    regs = out["regions"]
    out["summary"] = summarize(regs)
    Path(args.out).write_text(json.dumps(out, indent=1) + "\n")
    print(json.dumps(out["summary"], indent=1))
    return 0


def summarize(regs):
    def dist(key):
        v = sorted(r[key] for r in regs)
        return {"mean": statistics.mean(v), "median": statistics.median(v), "p90": v[int(0.9 * (len(v) - 1))],
                "max": v[-1]}
    with_text = [r for r in regs if r["lines"]]
    out = {"regions": len(regs), "with_lines": len(with_text), "strips": dist("strips"), "lines": dist("lines"),
           "frames": dist("frames"), "rows_read_per_row": statistics.mean(r["rows_read"] / r["h"] for r in regs),
           "pixels_read": statistics.mean(r["rows_read"] * r["w"] for r in regs),
           "tall": sum(r["tall"] for r in regs)}
    for fps in DEV_FPS:
        p = f"d{fps}_"
        if not regs or p + "strips" not in regs[0]:
            continue
        d = {k: dist(p + k) for k in DEFAULT_KEYS if k not in ("tries", "rows_read")}
        d["rows_read_per_row"] = statistics.mean(r[p + "rows_read"] / r["h"] for r in regs)
        d["pixels_read"] = statistics.mean(r[p + "rows_read"] * r["w"] for r in regs)
        d["retried_jobs"] = sum(r[p + "tries"] > 1 for r in regs)
        d["cascade_share"] = sum(r[p + "cascade_lines"] for r in regs) / max(1, sum(r[p + "lm_lines"] for r in regs))
        d["lm_changed_share"] = sum(r[p + "lm_changed"] for r in regs) / max(1, sum(r[p + "lm_lines"] for r in regs))
        d["jobs_with_cascade"] = sum(r[p + "cascade_lines"] > 0 for r in regs)
        out[f"default_{fps}fps"] = d
    return out


# ---------------------------------------------------------------- the A9 lane

KERNEL_FLAGS = ["-Wall", "-Wextra", "-Wno-unused-parameter", "-O2", "-std=gnu99", "-fno-builtin", "-mgeneral-regs-only"]
CORE_FLAGS = ["-O2", "-Wall", "-Wextra", "-Wno-unused-parameter", "-fno-builtin", "-fshort-wchar", "-ffunction-sections",
              "-fdata-sections"]
LIBS = ("libvjo_vocr.a", "libvjo_vocr_rt.a", "libvjo_core_vita.a", "libvjo_bearssl.a")
LIBC = re.compile(r"^(memcpy|memset|memmove|memcmp|__memcmpeq|memchr|strlen|strnlen|__strnlen|strcmp|strncmp|"
                  r"strchr|strrchr|strstr|__memcpy\w*|__memset\w*)$")
LIBGCC = re.compile(r"^(__aeabi_\w+|__udivsi3|__divsi3|__udivmodsi4|__modsi3|__umodsi3|__gnu_\w+|__udivmoddi4|"
                    r"__divdi3|__udivdi3|__moddi3|__umoddi3|__clz\w+|__ctz\w+|__popcount\w+)$")
PRINTF = re.compile(r"^(__vsnprintf\w*|vsnprintf|snprintf|__printf_\w+|__vfprintf\w*|vfprintf|_IO_\w+|__GI_\w+|"
                    r"_itoa\w*|__mpn_\w+|__printf_fp\w*|__strchrnul|__find_specmb\w*|__parse_one_spec\w*|"
                    r"_i18n_number_rewrite|__translated_number_width|outstring\w*|__wcsrtombs|"
                    r"__mbsrtowcs\w*|__libc_\w+|__lll_\w+|__overflow|__woverflow|__uflow|"
                    r"_dl_\w+|__\w*_internal\w*|group_number|_fitoa\w*|sceClib\w+)$")


def vita_tools():
    vitasdk = os.environ.get("VITASDK")
    if not vitasdk:
        die("set VITASDK")
    return Path(vitasdk) / "bin"


def kernel_objects(vita_build):
    found = {}
    for p in Path(vita_build).glob("CMakeFiles/VitaJPOverlay_Kernel.dir/**/kernel/*.c.obj"):
        found[p.name] = p
    # API 23: the Anki screenshot's JPEG encoder is the game worker's own object (not a library)
    for p in Path(vita_build).glob("CMakeFiles/VitaJPOverlay_OCR.dir/**/core/jpegsw.c.obj"):
        found[p.name] = p
    try:
        return [found["capture.c.obj"], found["scene.c.obj"], found["jpegsw.c.obj"]]
    except KeyError:
        die(f"{vita_build}: no kernel capture.c.obj/scene.c.obj or worker jpegsw.c.obj (build the Vita tree first)")


def build_a9(vita_build, out):
    """The bench, with the stage code from the Vita build's own objects."""
    vbin = vita_tools()
    vcc, vnm = vbin / "arm-vita-eabi-gcc", vbin / "arm-vita-eabi-nm"
    arm_cc = os.environ.get("ARM_CC")
    if not arm_cc:
        die("set ARM_CC (an arm-none-linux-gnueabihf-gcc)")
    vocr = os.environ.get("VJO_VOCR_SOURCE_DIR")
    if not vocr:
        die("set VJO_VOCR_SOURCE_DIR")
    libs = [Path(vita_build) / n for n in LIBS]
    for lib in libs:
        if not lib.exists():
            die(f"{lib} missing (build the Vita tree first)")
    # The Shell's core library is built with LTO for a release (vita/CMakeLists.txt): its objects then hold the Vita
    # compiler's LTO bytecode, which ARM_CC's linker cannot read. The lane needs a build configured -DVJO_CORE_LTO=OFF.
    vobjdump = vbin / "arm-vita-eabi-objdump"
    for lib in libs:
        heads = subprocess.run([str(vobjdump), "-h", str(lib)], capture_output=True, text=True, check=False).stdout
        if ".gnu.lto" in heads:
            die(f"{lib} holds LTO objects: configure {vita_build} with -DVJO_CORE_LTO=OFF for this lane")
    kobjs = kernel_objects(vita_build)
    # the text spotter's headers (API 54) from its own pin's checkout, after the recognizer's, as cmake/VocrOCR.cmake
    # orders them (VJO_VOCR_SPOT_SOURCE_DIR: tools/fast_gate.sh's; the recognizer's checkout when the pins are one)
    spot = os.environ.get("VJO_VOCR_SPOT_SOURCE_DIR") or vocr
    inc = ["-I", str(ROOT / "core"), "-I", str(ROOT / "include"), "-I", f"{vocr}/runtime", "-I", f"{spot}/runtime",
           "-I", str(ROOT / "third_party/bearssl/inc")]
    stage_o, glue_o, main_o = out / "stage_bench.o", out / "kernel_glue.o", out / "bench_main.o"
    subprocess.run([str(vcc), *CORE_FLAGS, *inc, "-c", "-o", str(stage_o), str(ROOT / "host/bench/stage_bench.c")],
                   check=True)
    subprocess.run([str(vcc), *KERNEL_FLAGS, *inc, "-c", "-o", str(glue_o), str(ROOT / "host/bench/kernel_glue.c")],
                   check=True)
    subprocess.run([arm_cc, "-O2", "-std=gnu99", "-mfloat-abi=hard", "-DVJO_A9_LANE", *inc, "-c", "-o", str(main_o),
                    str(ROOT / "host/bench/bench_main.c")], check=True)
    exe = out / "vjo-stage-bench-a9"
    subprocess.run([arm_cc, "-static", "-Wl,--no-enum-size-warning", "-Wl,--no-wchar-size-warning", "-o", str(exe),
                    str(main_o), str(stage_o), str(glue_o), *map(str, kobjs), *map(str, libs), "-lm"], check=True)
    objdump = arm_cc[:-3] + "objdump" if arm_cc.endswith("gcc") else "objdump"
    dis = subprocess.run([objdump, "-d", "--no-show-raw-insn", str(exe)], check=True, capture_output=True,
                         text=True).stdout
    (out / "bench.dis").write_text(dis)
    plugin = set()
    for obj in [*kobjs, *libs]:
        nm = subprocess.run([str(vnm), "--defined-only", str(obj)], capture_output=True, text=True, check=True).stdout
        plugin |= {ln.split()[-1] for ln in nm.splitlines() if len(ln.split()) >= 3 and ln.split()[1] in "tTwW"}
    (out / "plugin_functions.txt").write_text("\n".join(sorted(plugin)) + "\n")
    return exe, dis, plugin


def a9_model():
    vocr = os.environ.get("VJO_VOCR_SOURCE_DIR")
    sys.path[:0] = [f"{vocr}/vita"]
    import a9_cycles  # vita-vn-ocr's Cortex-A9 cost model, at the pinned commit
    return a9_cycles


def fn_class(fn, plugin):
    if fn in plugin:
        return "plugin"
    if LIBGCC.match(fn):
        return "plugin"   # the plugins link libgcc too
    if LIBC.match(fn):
        return "libc"     # glibc's, standing in for the firmware's (sceClib*, SceSysclibForDriver)
    if PRINTF.match(fn):
        return "printf"   # glibc's vsnprintf, standing in for sceClibVsnprintf
    return "driver"       # the bench's own code: not counted


def cost(m, rows, counts, extra, cache, plugin, ops, keep=("plugin", "libc", "printf")):
    """Cycles per op (low, high per clock) for the classes in keep, a9_cycles' formulas."""
    tot = collections.Counter()
    per_fn = collections.Counter()
    share = collections.Counter()
    for a, c in counts.items():
        if c <= 0:
            continue
        fn, mn, ops_ = rows.get(a, ("?", "?", ""))
        cls = fn_class(fn, plugin)
        if cls not in keep:
            continue
        unit, issue, ls, flags = m.classify(mn, ops_)
        per_fn[fn] += c
        share[cls] += c
        tot["insns"] += c
        tot[f"issue_{unit}"] += issue * c
        tot[f"n_{unit}"] += c
        tot["ls"] += ls * c
        if "n2c" in flags:
            tot["n2c"] += c
        for f in flags & {"alu", "br", "mul"}:
            tot[f] += c
    fp = tot["issue_N"] + tot["issue_V"] + tot["issue_L"]
    sw_lo, sw_hi = max(0, extra.get("switches_l_free", 0)), max(0, extra.get("switches_l_vfp", 0))
    l1m = max(0, cache["l1d_misses"]) if cache else 0
    l2m = max(0, cache["l2_misses"]) if cache else 0
    low_parts = {"neon_vfp_issue": fp, "dual_issue": tot["insns"] / 2, "ls_pipe": tot["ls"],
                 "int_pipes": (tot["alu"] + tot["br"] + tot["mul"]) / 2, "l2_refill": l1m * m.REFILL}
    low = max(low_parts.values()) + sw_lo * m.DRAIN[0]
    res = {"insns_per_op": tot["insns"] / ops, "share": {k: v / max(1, tot["insns"]) for k, v in share.items()},
           "neon_insns_per_op": tot["n_N"] / ops, "vfp_insns_per_op": tot["n_V"] / ops,
           "mul_per_op": tot["mul"] / ops, "ls_per_op": tot["ls"] / ops,
           "l1d_misses_per_op": l1m / ops, "l2_misses_per_op": l2m / ops,
           "low_bound": max(low_parts, key=low_parts.get),
           "top_functions": [(f, c / ops) for f, c in per_fn.most_common(10)], "cycles_per_op": {}, "ms_per_op": {}}
    for mhz in CLOCKS:
        stall = l1m * m.L2_LAT[1] + l2m * (m.L2_LAT[1] + m.DRAM_NS * mhz / 1000)
        high = max(fp, tot["insns"]) + sw_hi * m.DRAIN[1] + tot["n2c"] * m.N2C + stall
        res["cycles_per_op"][mhz] = [low / ops, high / ops]
        res["ms_per_op"][mhz] = [low / ops / (mhz * 1000), high / ops / (mhz * 1000)]
    return res


def diff(a, b):
    keys = set(a) | set(b)
    return {k: b.get(k, 0) - a.get(k, 0) for k in keys}


def cmd_a9(args):
    m = a9_model()
    qdir = Path(args.qemu or os.environ.get("QEMU_PLUGINS_DIR", ""))
    if not (qdir / "bin/qemu-arm").exists():
        die("set QEMU_PLUGINS_DIR to the plugin-enabled qemu (vita-vn-ocr vita/scripts/build_qemu_plugins.sh)")
    dictionary = plaintext_dir(args)
    work = Path(args.work).resolve()
    work.mkdir(parents=True, exist_ok=True)
    exe, dis, plugin = build_a9(args.vita_build, work)
    rows = m.parse_dis(dis)
    with open(work / "classes.txt", "w") as f:
        for a, (_, mn, ops) in sorted(rows.items()):
            unit = m.classify(mn, ops)[0]
            if unit != "I":
                f.write(f"{a:x} {unit}\n")
    results = {"lane": "a9", "cache": args.cache, "iters": [args.k1, args.k2], "workloads": {}}
    expected = json.loads(EXPECTED.read_text())["checks"]
    only = set(args.only.split(",")) if args.only else None
    stage = Path(tempfile.mkdtemp(prefix="vjo-a9-", dir="/tmp"))  # fixed-length real path (cache sets)
    try:
        frame = frame_file(stage)
        shutil.copy2(exe, stage / exe.name)
        for p in (TEXTS, CONFIG):
            shutil.copy2(p, stage / p.name)
        for name in (*MODELS, *VOCR6, LM, SPOT):
            if model_path(args.models, name):
                shutil.copy2(model_path(args.models, name), stage / name)
        if dictionary:
            (stage / "dict").mkdir()
            os.symlink(dictionary / "plaintext.vjdict", stage / "dict/plaintext.vjdict")
        for w in WORKLOADS:
            if only and w["name"] not in only:
                continue
            if available(w, args.models, dictionary):
                print(f"skip {w['name']}", file=sys.stderr)
                continue
            runs = {}
            try:
                for k in (args.k1, args.k2):
                    cnt = work / f"counts_{w['name']}_{k}.txt"
                    cache = work / f"cache_{w['name']}_{k}.txt"
                    plugins = ["-plugin", f"{qdir}/plugins/libinsn_mix.so,out={cnt},classes={work}/classes.txt"]
                    if args.cache:
                        plugins += ["-plugin", f"{qdir}/plugins/libcache.so,{m.CACHE_ARGS}", "-d", "plugin", "-D",
                                    str(cache)]
                    r = run_json([str(qdir / "bin/qemu-arm"), "-cpu", "cortex-a9", "-seed", "0", *plugins,
                                  f"./{exe.name}", *bench_args(w, frame, args.models, dictionary, k, rel=True)],
                                 cwd=stage, env={})
                    counts, extra = m.read_counts(cnt)
                    runs[k] = (r, counts, extra, m.read_cache(cache) if args.cache else None)
            except RuntimeError as e:
                print(f"FAILED {w['name']}: {e}", file=sys.stderr)
                results["workloads"][w["name"]] = {"error": str(e)[-500:]}
                continue
            (_, c1, e1, ca1), (r2, c2, e2, ca2) = runs[args.k1], runs[args.k2]
            n = args.k2 - args.k1
            ops = max(1, r2["ops"]) * n
            res = cost(m, rows, diff(c1, c2), diff(e1, e2), diff(ca1, ca2) if args.cache else None, plugin, ops)
            res.update({"stage": w["stage"], "unit": w["unit"], "ops": r2["ops"], "check": r2["check"],
                        "counters": r2["counters"], "text": r2.get("text"),
                        # the Vita compiler's code gave the host build's output (tools/bench_stages_expected.json)
                        "parity": r2["check"] == expected.get(w["name"])})
            results["workloads"][w["name"]] = res
            lo, hi = res["ms_per_op"][444]
            print(f"{w['name']:22s} {res['insns_per_op']:14,.0f} insns/{w['unit']:9s} {lo:9.4f}-{hi:9.4f} ms at 444 MHz "
                  f"({res['low_bound']}) check {r2['check']}", file=sys.stderr)
    finally:
        shutil.rmtree(stage, ignore_errors=True)
    Path(args.out).write_text(json.dumps(results, indent=1, ensure_ascii=False) + "\n")
    return 0


# ---------------------------------------------------------------- report

def strip_text(lane):
    """A lane's results without the recognized text (the bundle holds counts and geometry only)."""
    if not lane:
        return lane
    out = dict(lane)
    out["workloads"] = {k: {kk: vv for kk, vv in v.items() if kk != "text"} for k, v in lane.get("workloads", {}).items()}
    return out


DEV_COLUMNS = ("w", "h", "strips", "lines", "frames", "tall", "rows_read", "strip_rows", "d60_strips", "d60_frames",
               "d60_display_ms", "d60_settle_passes", "d60_cascade_lines", "d60_cascade_frames", "d30_strips",
               "d30_display_ms")


def cmd_report(args):
    from bench_model import build_report  # tools/bench_model.py: the job model and the tables
    import datetime
    host = json.loads(Path(args.host).read_text()) if args.host else None
    a9 = json.loads(Path(args.a9).read_text()) if args.a9 else None
    dev = json.loads(Path(args.dev).read_text()) if args.dev else None
    report, md = build_report(host, a9, dev, WORKLOADS)
    bundle = {"what": "tools/bench_stages.py report: the stage table and job model (report), the A9 and host lanes' "
                      "results without recognized text, the dev regions' summary and per-region counts (dev_rows, "
                      "columns dev_columns; no shot names)",
              "date": datetime.date.today().isoformat(), "report": report, "a9": strip_text(a9),
              "host": strip_text(host), "dev": (dev or {}).get("summary"), "dev_columns": list(DEV_COLUMNS),
              "dev_rows": [[r.get(c) for c in DEV_COLUMNS] for r in (dev or {}).get("regions", [])]}
    Path(args.out).write_text(json.dumps(bundle, indent=1, ensure_ascii=False) + "\n")
    if args.md:
        Path(args.md).write_text(md)
    else:
        print(md)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("smoke")
    s.add_argument("--binary", required=True)
    s.add_argument("--models", default=os.environ.get("VJO_VOCR_MODEL_DIR"))
    s.add_argument("--dict")
    s.add_argument("--record", action="store_true", help="write the checksums instead of checking them")
    h = sub.add_parser("host")
    h.add_argument("--binary", required=True)
    h.add_argument("--models", default=os.environ.get("VJO_VOCR_MODEL_DIR"))
    h.add_argument("--dict")
    h.add_argument("--runs", type=int, default=5)
    h.add_argument("--iters", type=int, default=5)
    h.add_argument("-o", "--out", required=True)
    a = sub.add_parser("a9")
    a.add_argument("--vita-build", required=True)
    a.add_argument("--models", default=os.environ.get("VJO_VOCR_MODEL_DIR"))
    a.add_argument("--dict")
    a.add_argument("--qemu")
    a.add_argument("--work", default="build/a9-bench")
    a.add_argument("--only", help="comma-separated workload names")
    a.add_argument("--k1", type=int, default=1)
    a.add_argument("--k2", type=int, default=3)
    a.add_argument("--no-cache", dest="cache", action="store_false")
    a.add_argument("-o", "--out", required=True)
    d = sub.add_parser("dev")
    d.add_argument("--binary", required=True)
    d.add_argument("--models", default=os.environ.get("VJO_VOCR_MODEL_DIR"))
    d.add_argument("--model", default=MODELS[0])
    d.add_argument("--size", choices=("S", "N", "L"),
                   help="API 54's default job instead (the spotter, VOCR6 of this size and its cascade, the worker's "
                        "policy): --model is ignored")
    d.add_argument("--first-ms-per-mpx", type=float, default=0.0,
                   help="--size: the spotter's compute during the first pass on the simulated clock, ms per megapixel "
                        "of the region (an A9 estimate the caller gives; 0: none, every strip a display frame only)")
    d.add_argument("--manifests", required=True)
    d.add_argument("--skip-missing", action="store_true", help="count and leave out the regions whose shot is not found")
    d.add_argument("--shots", action="append", required=True)
    d.add_argument("-o", "--out", required=True)
    r = sub.add_parser("report")
    r.add_argument("--host")
    r.add_argument("--a9")
    r.add_argument("--dev")
    r.add_argument("-o", "--out", required=True)
    r.add_argument("--md")
    args = ap.parse_args(argv)
    sys.path[:0] = [str(Path(__file__).resolve().parent)]
    return {"smoke": cmd_smoke, "host": cmd_host, "a9": cmd_a9, "dev": cmd_dev, "report": cmd_report}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
