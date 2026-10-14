"""The stage table and the end-to-end job model of tools/bench_stages.py (docs/performance.md).

Measured inputs: the host timings, the Cortex-A9 instruction counts and cycle ranges (vita-vn-ocr's
cost model on the Vita compiler's code under qemu-arm: estimates) and the linefind dev regions' strips,
lines and CTC frames per job. Everything else in the job model is a constant read from the source
(polling periods, display frame periods) or an assumption, and each such line says which it is.
"""

# Constants from the source, with where they are (checked by tools/tests/test_bench_model.py).
SOURCE = {
    "worker_take_poll_ms": (100, "game_ocr/main.c worker_main: sceKernelDelayThread(100000) between vjoOcrTake calls"),
    "shell_result_poll_ms": (50, "shell/game_ocr_client.c vjo_game_ocr_exchange: delay_us(50000) between vjoOcrRead calls"),
    "control_poll_ms": (50, "shell/worker.c POLL_TIMEOUT_US: the control thread's vjoWaitEvent timeout"),
    "kernel_tick_ms": (16, "kernel/main.c POLL_US: the kernel worker's tick"),
    "signature_every_frames": (8, "kernel/vjo_kernel.h VJO_CHECK_EVERY_FRAMES"),
    "settle_ms": (300, "kernel/scene.h SCENE_STABLE_US"),
}

# The Shell's log.txt lines of one Auto OCR job with the local dictionary in Whole word mode
# (shell/worker.c): job start; after the exchange: rc, capture, memory, speed, lines (6 lines per
# log line) and one per game-worker step (11 for a job with resident weights: mem, check, two
# allocations, first pass, signature, find, load, recognize, two frees); then the result arena,
# "subtitle text ready" (control thread, always logged), the first word's lookup and job done.
# Every line is its own open/write/close of log.txt, and of status.txt too while it holds less
# than 64 KiB (shell/log_vita.c).
JOB_LOG_LINES = {"before": 1, "report": 5 + 11, "after": 4}
KERNEL_LOG_LINES_PER_JOB = 3  # settled, ocr job N, first pass in (kernel/main.c, kernel/game_ocr.c)

# Assumptions (no console measurement exists yet; #81's log.txt will replace them: its lookup lines
# give reads and ms per lookup, its job lines the job's ms beside the game's own find/load/recognize).
ASSUME = {
    "fs_line_ms": (0.5, 3.0, "one open + write + close of a small append on ux0 (assumed range)"),
    "dict_read_ms": (0.2, 1.0, "one 4 KiB read at a random offset of the 134 MiB dictionary on ux0 (assumed)"),
    "dict_open_ms": (0.2, 1.0, "open + size of the dictionary file (assumed)"),
    "model_read_mb_s": (20.0, 40.0, "the model file through the kernel broker, 32 KiB at a time (assumed)"),
}
FPS = (60, 30)


def ms(a9, name, mhz):
    w = (a9 or {}).get("workloads", {}).get(name)
    if not w or "ms_per_op" not in w:
        return None
    v = w["ms_per_op"].get(str(mhz)) or w["ms_per_op"].get(mhz)
    return tuple(v) if v else None


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
        return (f"reads {c['fb_bytes_read'] // ops:,} B of the frame, writes {c['grey_bytes_written'] // ops:,} B of "
                f"grey per strip ({c['strip_rows']} rows); kernel scratch 2 x 3,840 B (static)")
    if name == "scene_sig":
        return f"sampled rows of the {c['sig_w']}x{c['sig_h']} region (A8B8G8R8), {c['grid_cols']}-column grid"
    if name == "find":
        return f"{c['arena_bytes']:,} B arena; {c['row_reads']} row reads (two passes) of the region"
    if name.startswith("recognize"):
        return f"{c['arena_bytes']:,} B arena + resident weights; {c['frames'] // ops} CTC frames per line"
    if name.startswith("job"):
        return (f"{c['workspace_bytes']:,} B workspace + {c['capture_bytes']:,} B capture block; {c['strips']} strips, "
                f"{c['rows_read']} rows read for a {c['strip_rows']}-row strip")
    if name.startswith("sha256"):
        return f"{c['bytes']:,} B of weights"
    if name.startswith("lookup"):
        return (f"{c['reads'] / ops:.1f} reads, {c['read_bytes'] / ops / 1024:.1f} KiB and {c['opens'] / ops:.0f} open "
                f"per lookup; arena peak {c['arena_peak']:,} B of the Shell's 64 KiB")
    if name == "config":
        return f"{c['bytes']:,} B of config.ini"
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
        src = h or a
        rows.append({
            "workload": w["name"], "unit": w["unit"], "what": w["what"],
            "ops_per_iter": src.get("ops"),
            "host_ms": h["ms_per_op"] if h else None,
            "a9_insns": a.get("insns_per_op") if a and "error" not in a else None,
            "a9_444": ms(a9, w["name"], 444), "a9_333": ms(a9, w["name"], 333),
            "l1d_misses": a.get("l1d_misses_per_op") if a and "error" not in a else None,
            "libc_share": (a.get("share", {}).get("libc", 0) + a.get("share", {}).get("printf", 0))
            if a and "error" not in a else None,
            "parity": a.get("parity") if a else None,
            "counters": src.get("counters"),
            "memory": memory_note(w["name"], src.get("counters"), src.get("ops")),
        })
    return rows


def job_model(a9, dev, mhz, fps, first_job=False, lines=None, frames=None, strips=None):
    """The latency of one Auto OCR job, settled screen to the first word ready, as a (low, high)
    range in ms with its parts. lines/frames/strips: the dev regions' means unless given."""
    s = (dev or {}).get("summary", {})
    strips = strips if strips is not None else s.get("strips", {}).get("mean", 8)
    lines = lines if lines is not None else s.get("lines", {}).get("mean", 2.4)
    frames = frames if frames is not None else s.get("frames", {}).get("mean", 86)
    rec = a9["workloads"]["recognize_h15"]
    per_frame = [c / mhz / 1000 * rec["ops"] / max(1, rec["counters"]["frames"]) for c in rec["cycles_per_op"][str(mhz)]]
    find = ms(a9, "find", mhz)
    hook = ms(a9, "capture_abgr", mhz)
    lookup_cpu = ms(a9, "lookup_first", mhz)
    post = ms(a9, "postproc", mhz)
    fmt_line = ms(a9, "log", mhz)
    lookup = a9["workloads"]["lookup_first"]["counters"]
    reads_per = lookup["reads"] / max(1, lookup["lookups"])
    frame_ms = 1000.0 / fps
    take = SOURCE["worker_take_poll_ms"][0]
    poll = SOURCE["shell_result_poll_ms"][0]
    n_lines = sum(JOB_LOG_LINES.values())
    parts = {
        "take (game worker poll)": (0.0, float(take), f"0-{take} ms, mean {take / 2:.0f}: {SOURCE['worker_take_poll_ms'][1]}"),
        # The finder computes between its strips: inside the strip waits when each strip's work takes
        # less than a frame (low), on top of them when the game's threads keep the lowest-priority
        # worker from running (high).
        "capture + finder": (max(strips * frame_ms, find[0]), strips * frame_ms + find[1],
                             f"{strips:.1f} strips (dev mean), one display frame each ({frame_ms:.1f} ms at {fps} "
                             f"fps), the finder's {fmt_range(find)} (A9) inside them (low) or after them (high); the "
                             f"hook's CPU ({fmt_range(hook, 2)} per strip) is the game's, in its display call"),
        "recognition (VOCR3-H15)": (per_frame[0] * frames, per_frame[1] * frames,
                                    f"A9 estimate, {frames:.0f} CTC frames (dev mean, {lines:.1f} lines)"),
        "result (Shell poll)": (0.0, float(poll), f"0-{poll} ms, mean {poll / 2:.0f}: {SOURCE['shell_result_poll_ms'][1]}"),
        "log.txt (+ status.txt)": (n_lines * ASSUME["fs_line_ms"][0], n_lines * 2 * ASSUME["fs_line_ms"][1],
                                   f"{n_lines} lines, each an open/write/close (twice while status.txt < 64 KiB); "
                                   f"assumed {ASSUME['fs_line_ms'][0]}-{ASSUME['fs_line_ms'][1]} ms each; "
                                   f"formatting {fmt_range(fmt_line, 3)} per line"),
        "text post-processing": (post[0], post[1], "A9 estimate"),
        "first-word lookup": (lookup_cpu[0] + ASSUME["dict_open_ms"][0] + reads_per * ASSUME["dict_read_ms"][0],
                              lookup_cpu[1] + ASSUME["dict_open_ms"][1] + reads_per * ASSUME["dict_read_ms"][1],
                              f"CPU {fmt_range(lookup_cpu, 2)} (A9) + 1 open + {reads_per:.1f} reads (assumed "
                              f"{ASSUME['dict_read_ms'][0]}-{ASSUME['dict_read_ms'][1]} ms each)"),
    }
    if first_job:
        sha = ms(a9, "sha256_h15", mhz)
        mb = 1155968 / 1e6
        parts["weights: read + SHA-256 (first job)"] = (
            sha[0] + mb / ASSUME["model_read_mb_s"][1] * 1000, sha[1] + mb / ASSUME["model_read_mb_s"][0] * 1000,
            f"SHA-256 {fmt_range(sha)} (A9) + 1.16 MB read (assumed {ASSUME['model_read_mb_s'][0]:.0f}-"
            f"{ASSUME['model_read_mb_s'][1]:.0f} MB/s)")
    lo = sum(p[0] for p in parts.values())
    hi = sum(p[1] for p in parts.values())
    mean_poll = take / 2 + poll / 2
    return {"mhz": mhz, "fps": fps, "first_job": first_job, "strips": strips, "lines": lines, "frames": frames,
            "parts": {k: {"low": v[0], "high": v[1], "note": v[2]} for k, v in parts.items()},
            "total": [lo, hi], "mean_polling_ms": mean_poll}


def build_report(host, a9, dev, workloads):
    rows = stage_rows(host, a9, workloads)
    report = {"stages": rows, "source": {k: v[0] for k, v in SOURCE.items()},
              "assumptions": {k: list(v) for k, v in ASSUME.items()}, "job_log_lines": JOB_LOG_LINES,
              "host_cpu": (host or {}).get("cpu"), "dev": (dev or {}).get("summary")}
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
        md += ["", "| job model | 444 MHz, 60 fps | 444 MHz, 30 fps | 333 MHz, 60 fps | 333 MHz, 30 fps |",
               "|---|---:|---:|---:|---:|"]
        models = [job_model(a9, dev, mhz, fps) for mhz in (444, 333) for fps in FPS]
        report["jobs"] += models
        for part in models[0]["parts"]:
            md.append(f"| {part} | " + " | ".join(
                f"{m['parts'][part]['low']:.0f}-{m['parts'][part]['high']:.0f} ms" for m in models) + " |")
        md.append("| **total** | " + " | ".join(f"**{m['total'][0]:.0f}-{m['total'][1]:.0f} ms**" for m in models) + " |")
        first = [job_model(a9, dev, mhz, fps, first_job=True) for mhz in (444, 333) for fps in FPS]
        report["jobs"] += first
        md.append("| total, a game's first job | " + " | ".join(
            f"{m['total'][0]:.0f}-{m['total'][1]:.0f} ms" for m in first) + " |")
    return report, "\n".join(md) + "\n"
