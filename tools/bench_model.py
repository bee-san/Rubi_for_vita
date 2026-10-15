"""The stage table and the end-to-end job model of tools/bench_stages.py (docs/performance.md).

Measured inputs: the host timings, the Cortex-A9 instruction counts and cycle ranges (vita-vn-ocr's
cost model on the Vita compiler's code under qemu-arm: estimates), the linefind dev regions' strips,
lines, CTC frames and cascade re-reads per job, and the console's own timings from #81's --collect
logs (CONSOLE: aggregates only; the logs are private). Everything else in the job model is a constant
read from the source (polling periods, log lines), and each line of the model says which it is.
"""
import math

# Constants from the source, with where they are (checked by tools/tests/test_bench_model.py).
SOURCE = {
    "worker_take_poll_ms": (100, "game_ocr/main.c worker_main: sceKernelDelayThread(100000) between vjoOcrTake calls"),
    "shell_result_poll_ms": (50, "shell/game_ocr_client.c vjo_game_ocr_exchange: delay_us(50000) between vjoOcrRead calls"),
    "control_poll_ms": (50, "shell/worker.c POLL_TIMEOUT_US: the control thread's vjoWaitEvent timeout"),
    "kernel_tick_ms": (16, "kernel/main.c POLL_US: the kernel worker's tick"),
    "signature_every_frames": (8, "kernel/vjo_kernel.h VJO_CHECK_EVERY_FRAMES"),
    "settle_ms": (300, "kernel/scene.h SCENE_STABLE_US"),
}

# The Shell's log.txt lines of one Auto OCR job that finds lines, with the local dictionary in
# Whole word mode. Every line is its own open/append/close of log.txt, and of status.txt too while
# that holds less than 64 KiB (shell/log_vita.c).
#   start: "job start" (shell/worker.c start_job_kind, before the job is handed over);
#   result: vocr_run_one's "game vocr rc=" and "capture", log_vocr_measurements' "memory" and
#     "speed", one "lines" line per 6 lines, "cascade speed" when the cascade read a line again;
#   steps: one per game-worker step (log_vocr_steps). API 19's resident job has 11: mem, check, two
#     allocations, first pass, signature, find, load, recognize, two frees; main's default job 15:
#     the same and settle, lm, cascade, norm (game_ocr/main.c vjo_game_ocr_run_request);
#   text: main only (#88, log_text_lines): one per recognized line, two where the shown text differs;
#   after: "result arenas" (the Paf allocation), "local dictionary workspace", the first word's
#     "lookup", "job done";
#   subtitle: "subtitle text ready" (the control thread, beside the net thread's lookup).
JOB_LOG = {"start": 1, "result": 4, "steps_api19": 11, "steps": 15, "after": 4, "subtitle": 1}
KERNEL_LOG_LINES_PER_JOB = 3  # settled, ocr job N, first pass in (kernel/main.c, kernel/game_ocr.c)


def job_log_lines(kind, lines, cascade_jobs=0.0, text_extra=0.0):
    """Log lines of one job (JOB_LOG); kind "api19" (v0.8.0-test.1's job) or "default" (main's)."""
    n = JOB_LOG["start"] + JOB_LOG["result"] + math.ceil(lines / 6) + JOB_LOG["after"] + JOB_LOG["subtitle"]
    if kind == "api19":
        return n + JOB_LOG["steps_api19"]
    return n + JOB_LOG["steps"] + cascade_jobs + lines + text_extra


# The console: #81's --collect logs of v0.8.0-test.1 (API 19's job; private, aggregates only), 11
# Auto OCR jobs that found lines in one game's 883x112 region with VOCR3-H15 resident, 30 lookups.
CONSOLE = {
    "h15_ms_per_frame": (5.056, "VOCR3-H15: 664 CTC frames in 10 jobs' recognize= times (the owner: 5.0-5.2 ms); "
                                "vita-vn-ocr's A9 model gives 5.00 ms at its 333 MHz upper bound"),
    "fs_append_ms": (12.1, 46.0, 15.7,
                     "one open/append/close of a file on ux0 with the game running, measured by the self-test's own "
                     "timestamps: each stage's closing report lines (a report, a status.txt and a log.txt append "
                     "each) took 12.1-46.0 ms per append, median 15.7 (5 stages)"),
    "job_unexplained_per_line_ms": (38.3,
                                    "the 11 jobs' Shell time minus the game's job, the lookup and the polls at their "
                                    "mean, per line the job logged: median 38.3 ms (15-94). An upper bound of a "
                                    "line's cost: it holds everything the model leaves out (the lowest-priority "
                                    "game worker's scheduling, file system contention with the game)"),
    "lookup_fixed_ms": (15.3, "a lookup's open, header and CPU: the intercept of ms against reads, 30 lookups"),
    "lookup_read_ms": (3.82, "one dictionary read (4 KiB at a random offset of the 134 MiB file): the slope"),
    "first_load_ms": (218.0, 231.0, "load= of the 4 jobs that read the weights: 1,155,968 B read and SHA-256 checked"),
    # the 11 jobs themselves, for the model's check (validation)
    "jobs": {"n": 11, "shell_ms_mean": 1392.7, "shell_ms_median": 1368.0, "game_ms_mean": 488.6,
             "lookup_ms_mean": 88.5, "unexplained_ms_mean": 823.7, "log_lines_mean": 19.1, "strips_mean": 5.5,
             "capture_ms_mean": 87.8, "find_ms_mean": 104.6, "frames_mean": 66.4, "lines": 1, "lookup_reads": 15.2,
             "region": (883, 112)},
}
FPS = (60, 30)


def ms(a9, name, mhz):
    w = (a9 or {}).get("workloads", {}).get(name)
    if not w or "ms_per_op" not in w:
        return None
    v = w["ms_per_op"].get(str(mhz)) or w["ms_per_op"].get(mhz)
    return tuple(v) if v else None


def per_frame(a9, name, mhz):
    """ms per CTC frame (low, high) of a recognize_* workload."""
    w = a9["workloads"][name]
    lo, hi = ms(a9, name, mhz)
    k = w["ops"] / max(1, w["counters"]["frames"])
    return lo * k, hi * k


def lm_per_frame(a9, mhz):
    """The beam + LM decoder's ms per CTC frame: job_h15_lm minus job_h15_greedy, per frame."""
    a, b = ms(a9, "job_h15_lm", mhz), ms(a9, "job_h15_greedy", mhz)
    f = max(1, a9["workloads"]["job_h15_lm"]["counters"]["frames"])
    return max(0.0, (a[0] - b[0]) / f), max(0.0, (a[1] - b[1]) / f)


def fmt_range(v, digits=1):
    if v is None:
        return "–"
    lo, hi = v
    if hi < 0.1:
        return f"{lo * 1000:.0f}-{hi * 1000:.0f} µs"
    return f"{lo:.{digits}f}-{hi:.{digits}f} ms"


def memory_note(name, c, ops):
    """What the stage touches, from its counters (bytes per op)."""
    if not c:
        return ""
    ops = max(1, ops or 1)
    if name.startswith("capture"):
        # per op = per strip; the region's last strip is shorter (896x156 in 64-row strips: 64 + 64 + 28)
        mean_rows = c["rows"] / max(1, c["strips"])
        return (f"reads {c['fb_bytes_read'] // ops:,} B of the frame, writes {c['grey_bytes_written'] // ops:,} B of "
                f"grey per strip (mean {mean_rows:.1f} rows of {c['strip_rows']}-row strips); kernel scratch "
                f"2 x 3,840 B (static)")
    if name == "scene_sig":
        return f"sampled rows of the {c['sig_w']}x{c['sig_h']} region (A8B8G8R8), {c['grid_cols']}-column grid"
    if name == "find":
        return f"{c['arena_bytes']:,} B arena; {c['row_reads']} row reads (two passes) of the region"
    if name.startswith("recognize"):
        return f"{c['arena_bytes']:,} B arena + resident weights; {c['frames'] // ops} CTC frames per line"
    if name.startswith("job"):
        extra = ""
        if c.get("lm_lines"):
            extra = f"; {c['lm_lines']} lines decoded with the LM"
        if c.get("settle_passes"):
            extra += f", {c['settle_passes']} settle pass"
        return (f"{c['workspace_bytes']:,} B workspace + {c['capture_bytes']:,} B capture block; {c['strips']} strips, "
                f"{c['rows_read']} rows read for a {c['strip_rows']}-row strip{extra}")
    if name.startswith("sha256"):
        return f"{c['bytes']:,} B of weights"
    if name.startswith("lookup"):
        return (f"{c['reads'] / ops:.1f} reads, {c['read_bytes'] / ops / 1024:.1f} KiB and {c['opens'] / ops:.0f} open "
                f"per lookup; arena peak {c['arena_peak']:,} B of the Shell's 64 KiB")
    if name == "config":
        return f"{c['bytes']:,} B of config.ini into a {c.get('struct_bytes', 0):,} B VjoConfig"
    if name == "log":
        return f"{c['bytes'] / ops:.0f} B per line"
    if name == "anki":
        return f"{c['json_bytes'] / ops:.0f} B queue record per card"
    if name == "postproc":
        return f"{c['bytes_in'] / ops:.0f} B of text"
    return ""


def stage_rows(host, a9, workloads):
    rows = []
    for w in workloads:
        h = (host or {}).get("workloads", {}).get(w["name"])
        a = (a9 or {}).get("workloads", {}).get(w["name"])
        if not h and not a:
            continue
        a_ok = a if a and "error" not in a else None
        src = a_ok or h  # memory: the Vita compiler's 32-bit build first, the x86-64 host's otherwise
        rows.append({
            "workload": w["name"], "unit": w["unit"], "what": w["what"],
            "ops_per_iter": src.get("ops"),
            "host_ms": h["ms_per_op"] if h else None,
            "a9_insns": a_ok.get("insns_per_op") if a_ok else None,
            "a9_444": ms(a9, w["name"], 444), "a9_333": ms(a9, w["name"], 333),
            "l1d_misses": a_ok.get("l1d_misses_per_op") if a_ok else None,
            "libc_share": (a_ok.get("share", {}).get("libc", 0) + a_ok.get("share", {}).get("printf", 0))
            if a_ok else None,
            "parity": a.get("parity") if a else None,
            "counters": src.get("counters"),
            "counters_from": "a9" if a_ok else "host",
            "memory": memory_note(w["name"], src.get("counters"), src.get("ops")),
        })
    return rows


def dev_default(dev, fps):
    return (dev or {}).get("summary", {}).get(f"default_{fps}fps", {})


def job_model(a9, dev, mhz, fps, kind="default", first_job=False, given=None):
    """The latency of one Auto OCR job, settled screen to the first word ready, as a (low, high) range and a
    point estimate in ms, with its parts. kind "default": main's default job at the dev regions' means;
    "api19": v0.8.0-test.1's job (greedy, no wait, no cascade). given: the job's own strips, frames, lines,
    log lines, lookup reads and region scale, in place of the dev means (the console check)."""
    s = (dev or {}).get("summary", {})
    d = dev_default(dev, fps)
    g = given or {}
    frame_ms = 1000.0 / fps
    if kind == "api19":
        strips = g.get("strips", s.get("strips", {}).get("mean", 8))
        frames = g.get("frames", s.get("frames", {}).get("mean", 86))
        lines = g.get("lines", s.get("lines", {}).get("mean", 2.4))
        display = (strips * frame_ms, strips * frame_ms)
        casc_frames, casc_jobs = 0.0, 0.0
    else:
        strips = d.get("strips", {}).get("mean", 9)
        frames = d.get("frames", {}).get("mean", 93)
        lines = d.get("lines", {}).get("mean", 2.4)
        # the dev lane's simulated display time: a frame per strip, and the wait's sleeps
        dms = d.get("display_ms", {}).get("mean", strips * frame_ms)
        display = (dms, dms)
        casc_frames = d.get("cascade_frames", {}).get("mean", 0.0)
        casc_jobs = d.get("jobs_with_cascade", 0) / max(1, s.get("regions", 1))
    log_lines = g.get("log_lines", job_log_lines(kind, lines, casc_jobs))
    scale = g.get("scale", 1.0)  # the finder's work scales with the region's pixels
    find = tuple(v * scale for v in ms(a9, "find", mhz))
    h15 = per_frame(a9, "recognize_h15", mhz)
    lmf = lm_per_frame(a9, mhz) if kind != "api19" else (0.0, 0.0)
    w20 = per_frame(a9, "recognize_w20", mhz)
    hook = ms(a9, "capture_abgr", mhz)
    lookup_cpu = ms(a9, "lookup_first", mhz)
    post = ms(a9, "postproc", mhz)
    fmt_line = ms(a9, "log", mhz)
    lookup = a9["workloads"]["lookup_first"]["counters"]
    reads_per = g.get("lookup_reads", lookup["reads"] / max(1, lookup["lookups"]))
    take = SOURCE["worker_take_poll_ms"][0]
    poll = SOURCE["shell_result_poll_ms"][0]
    fs_lo, fs_hi, fs_est, _ = CONSOLE["fs_append_ms"]
    lk_fixed, lk_read = CONSOLE["lookup_fixed_ms"][0], CONSOLE["lookup_read_ms"][0]
    # The console's own rate where it was measured: H15 at 5.06 ms per frame is the A9 model's upper
    # bound at 333 MHz (5.00); the estimate column takes every compute part at its upper bound, scaled
    # by that ratio.
    k_console = CONSOLE["h15_ms_per_frame"][0] / per_frame(a9, "recognize_h15", 333)[1]
    parts = {}

    def part(name, lo, hi, est, note):
        parts[name] = (lo, hi, est, note)

    part("take (game worker poll)", 0.0, float(take), take / 2,
         f"0-{take} ms, mean {take / 2:.0f}: {SOURCE['worker_take_poll_ms'][1]}")
    wait = "" if kind == "api19" else ", the stable-text wait's pass and sleep included"
    part("capture + finder" + ("" if kind == "api19" else " + wait"), max(display[0], find[0]), display[1] + find[1],
         display[1] + find[1],
         f"{strips:.1f} strips, one display frame each at {fps} fps ({display[0]:.0f} ms{wait}); the finder's "
         f"{fmt_range(find)} (A9) inside them (low) or after them (high); the hook's CPU ({fmt_range(hook, 2)} per "
         f"strip) is the game's, in its display call")
    part("recognition (VOCR3-H15" + (")" if kind == "api19" else ", beam + LM)"),
         frames * (h15[0] + lmf[0]), frames * (h15[1] + lmf[1]), frames * (h15[1] + lmf[1]) * k_console,
         f"A9 estimate, {frames:.0f} CTC frames ({lines:.1f} lines): {fmt_range(h15, 2)} a frame"
         + ("" if kind == "api19" else f" + the LM's {fmt_range(lmf, 2)}") +
         f"; console {CONSOLE['h15_ms_per_frame'][0]} ms a frame")
    if kind != "api19":
        part("cascade (VOCR4-W20 re-reads)", casc_frames * (w20[0] + lmf[0]), casc_frames * (w20[1] + lmf[1]),
             casc_frames * (w20[1] + lmf[1]) * k_console,
             f"{casc_frames:.0f} CTC frames a job read again (dev mean: {d.get('cascade_share', 0) * 100:.0f}% of "
             f"the lines), {fmt_range(w20, 2)} a frame (A9) + the LM")
    part("result (Shell poll)", 0.0, float(poll), poll / 2,
         f"0-{poll} ms, mean {poll / 2:.0f}: {SOURCE['shell_result_poll_ms'][1]}")
    part("log.txt", log_lines * fs_lo, log_lines * fs_hi, log_lines * fs_est,
         f"{log_lines:.0f} lines, each an open/append/close (twice while status.txt < 64 KiB): console "
         f"{fs_lo:.0f}-{fs_hi:.0f} ms an append (median {fs_est}); formatting {fmt_range(fmt_line, 3)} a line")
    part("text post-processing", post[0], post[1], post[1], "A9 estimate")
    part("first-word lookup", lookup_cpu[0] + lk_fixed + reads_per * lk_read * 0.8,
         lk_fixed + reads_per * lk_read * 1.2, lk_fixed + reads_per * lk_read,
         f"console: {lk_fixed} ms + {lk_read} ms a read, {reads_per:.1f} reads (CPU {fmt_range(lookup_cpu, 2)}, A9, "
         f"inside the console's {lk_fixed} ms); ±20% on the reads")
    if first_job:
        sha = ms(a9, "sha256_h15", mhz)
        lo_ms, hi_ms = CONSOLE["first_load_ms"][:2]
        lm_bytes = 987600 if kind != "api19" else 0
        k_lm = lm_bytes / 1155968
        part("weights" + ("" if kind == "api19" else " + LM") + ": read + SHA-256 (first job)", lo_ms * (1 + k_lm),
             hi_ms * (1 + k_lm), (lo_ms + hi_ms) / 2 * (1 + k_lm),
             f"console: {lo_ms:.0f}-{hi_ms:.0f} ms for the 1,155,968 B weights (SHA-256 alone {fmt_range(sha)}, A9)"
             + ("" if kind == "api19" else "; the 987,600 B LM scaled by its bytes"))
    lo = sum(p[0] for p in parts.values())
    hi = sum(p[1] for p in parts.values())
    est = sum(p[2] for p in parts.values())
    return {"mhz": mhz, "fps": fps, "kind": kind, "first_job": first_job, "strips": strips, "lines": lines,
            "frames": frames, "log_lines": log_lines, "cascade_frames": casc_frames,
            "parts": {k: {"low": v[0], "high": v[1], "estimate": v[2], "note": v[3]} for k, v in parts.items()},
            "total": [lo, hi], "estimate": est, "mean_polling_ms": take / 2 + poll / 2}


def console_check(a9):
    """API 19's job at the console jobs' own means, against what those jobs took (333 MHz, 60 fps). Not an
    independent validation: recognition's rate and the lookup's fit come from the same logs; the log
    lines are costed with the self-test's appends, so the remainder (unexplained) is what the model
    does not account for."""
    c = CONSOLE["jobs"]
    w, h = c["region"]
    scale = (w * h) / (896 * 156)
    # the Shell's job time (job N done ... in T ms) runs from the job's hand-over to its last lookup: no
    # "job start", "job done" or "subtitle" line inside it
    lines_inside = c["log_lines_mean"]
    m = job_model(a9, None, 333, 60, kind="api19",
                  given={"strips": c["strips_mean"], "frames": c["frames_mean"], "lines": c["lines"],
                         "log_lines": lines_inside, "lookup_reads": c["lookup_reads"], "scale": scale})
    unexplained = c["shell_ms_mean"] - m["estimate"]
    return {"model": m, "measured": c, "unexplained_ms": unexplained,
            "unexplained_per_line_ms": unexplained / max(1.0, lines_inside)}


def build_report(host, a9, dev, workloads):
    rows = stage_rows(host, a9, workloads)
    report = {"stages": rows, "source": {k: v[0] for k, v in SOURCE.items()},
              "console": {k: (list(v) if isinstance(v, tuple) else v) for k, v in CONSOLE.items()},
              "job_log": JOB_LOG, "host_cpu": (host or {}).get("cpu"), "dev": (dev or {}).get("summary")}
    md = ["| workload | per | host (x86-64) | A9 insns | A9 at 444 MHz | A9 at 333 MHz | libc share | parity |",
          "|---|---|---:|---:|---:|---:|---:|:-:|"]
    for r in rows:
        if r["host_ms"] is None:
            host_cell = "–"
        elif r["host_ms"] < 1:
            host_cell = f"{r['host_ms'] * 1000:.1f} µs"
        else:
            host_cell = f"{r['host_ms']:.2f} ms"
        if r["a9_insns"] is None:
            a9_cells = "– | – | – | – | –"
        else:
            parity = "yes" if r["parity"] else "NO" if r["parity"] is False else "–"
            a9_cells = (f"{r['a9_insns']:,.0f} | {fmt_range(r['a9_444'], 2)} | {fmt_range(r['a9_333'], 2)} | "
                        f"{r['libc_share'] * 100:.0f}% | {parity}")
        md.append(f"| `{r['workload']}` | {r['unit']} | {host_cell} | {a9_cells} |")
    md += ["", "| workload | what it is | memory it touches (per op) |", "|---|---|---|"]
    md += [f"| `{r['workload']}` | {r['what']} | {r['memory']} |" for r in rows]
    if a9 and "recognize_h15" in a9.get("workloads", {}):
        report["jobs"] = []
        for kind, title in (("default", "main's default job"), ("api19", "API 19's job (v0.8.0-test.1)")):
            models = [job_model(a9, dev, mhz, fps, kind=kind) for mhz in (444, 333) for fps in FPS]
            report["jobs"] += models
            md += ["", f"| {title} | 444 MHz, 60 fps | 444 MHz, 30 fps | 333 MHz, 60 fps | 333 MHz, 30 fps | "
                       "estimate (333 MHz, 60 fps) |", "|---|---:|---:|---:|---:|---:|"]
            for part in models[0]["parts"]:
                md.append(f"| {part} | " + " | ".join(
                    f"{m['parts'][part]['low']:.0f}-{m['parts'][part]['high']:.0f} ms" for m in models) +
                    f" | {models[2]['parts'][part]['estimate']:.0f} ms |")
            md.append("| **total** | " + " | ".join(f"**{m['total'][0]:.0f}-{m['total'][1]:.0f} ms**" for m in models)
                      + f" | **{models[2]['estimate']:.0f} ms** |")
            first = [job_model(a9, dev, mhz, fps, kind=kind, first_job=True) for mhz in (444, 333) for fps in FPS]
            report["jobs"] += first
            md.append("| total, a game's first job | " + " | ".join(
                f"{m['total'][0]:.0f}-{m['total'][1]:.0f} ms" for m in first) + f" | {first[2]['estimate']:.0f} ms |")
        chk = console_check(a9)
        report["console_check"] = chk
        m, c = chk["model"], chk["measured"]
        md += ["", "| console check (API 19's job, the 11 console jobs' means) | model, 333 MHz, 60 fps | console |",
               "|---|---:|---:|",
               f"| the Shell's job time | {m['total'][0]:.0f}-{m['total'][1]:.0f} ms (estimate {m['estimate']:.0f}) | "
               f"{c['shell_ms_mean']:.0f} ms mean, {c['shell_ms_median']:.0f} median |",
               f"| not explained by the model | | {chk['unexplained_ms']:.0f} ms "
               f"({chk['unexplained_per_line_ms']:.1f} ms per logged line) |"]
    return report, "\n".join(md) + "\n"
