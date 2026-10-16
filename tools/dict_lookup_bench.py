#!/usr/bin/env python3
"""The Shell's local word lookup over real recognized lines: file reads (what they fetch),
bytes, deinflection candidates generated / searched / found, the arena peak and the time,
per lookup and per screen, with a console latency model. Compares two builds or dictionaries
and fails unless every lookup shows exactly the same entries (a golden check).

  dict_lookup_bench.py --bench build/host/vjo-dict-bench --directory DIR --dicts plaintext.vjdict \\
      [--before OTHER_BENCH] [--before-directory DIR] [--before-dicts FILES] \\
      [--lines TSV] [--pool LABELS.jsonl --pool-screens 500 --reserved reserved.json] \\
      [--modes first,words,letters] [--rounds 21] [--golden FILE | --write-golden FILE] [-o out.json]

Lines: host/fixtures/word_lookup/vocr_lines.tsv (50 real VOCR lines, committed) by default.
--pool adds a fixed sample of screens from a line-label pool (vita-vn-ocr realplus labels:
kept, non-name lines, grouped by screenshot, ordered by sha256 of the seed and the shot id),
leaving out every VN of reserved.json's "test" (vita-test, psp-test, the CLANNAD family and
their shared releases); the text stays in a temporary file and is never written elsewhere.
The report records each pool file's sha256: a pool that changed gives other screens.

Host times have a warm OS file cache. The console figures are a model, not a measurement:
CONSOLE below, a least-squares fit of the 57 distinct lookup log lines of the owner's
v0.8.0-test.2 console log (private; only the fit is quoted; its first 4,966 lines are
test.1's whole log, so test.1 adds none), plus a bound for reads above 4 KiB, which those
lookups never made (the model weights' 32 KiB broker reads bound it).
"""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
LINES = ROOT / "host/fixtures/word_lookup/vocr_lines.tsv"
CONSOLE = {
    "fixed_ms": 21.9,       # per lookup (fit intercept): the open, size seek and close, and waits
    "read_ms": 3.73,        # one read call, up to 4 KiB (fit slope)
    "excess_kib_ms": 0.146,  # bound per KiB a read takes beyond 4 KiB
    "source": ("estimate: least-squares fit over the 57 distinct lookups of the v0.8.0-test.2 console "
               "log.txt, whose first 4,966 lines are test.1's whole log (ms = 21.9 + 3.73 x reads, R^2 0.52, "
               "reads 5-33). By kind: cursor (n 35) 11.2 + 3.76 x reads, R^2 0.85; first word and on open "
               "(n 22) 35.4 + 3.91 x reads, R^2 0.27. docs/performance.md's 30-lookup fit: 15.3 + 3.82. "
               "A read's bytes beyond 4 KiB at most 0.146 ms/KiB: the 1,155,968 B of VOCR3-H15 weights in "
               "32 KiB reads took at most 165 ms (load 218-231 ms less the A9 SHA-256 estimate)"),
}
COUNTS = ("reads", "bytes", "excess_bytes", "pages", "nonsequential", "spans", "candidates", "probes",
          "filtered", "miss_cached", "hash_present", "found", "ranked", "hinted", "absent_reads")
KINDS = ("header", "index", "group", "head", "payload", "filter")


def console_ms(q):
    """The modelled console time of one lookup (no file opened: no I/O, 0 ms)."""
    if not q["opens"]:
        return 0.0
    return (CONSOLE["fixed_ms"] * q["opens"] + CONSOLE["read_ms"] * q["reads"]
            + CONSOLE["excess_kib_ms"] * q["excess_bytes"] / 1024)


def pool_lines(labels, reserved, screens, seed="dict-lookup-speed-20261015"):
    """Screens of a line-label pool: (shot, text) for `screens` screenshots outside the test VNs."""
    test = set(json.loads(Path(reserved).read_text())["test"])
    by_shot = {}
    with open(labels, encoding="utf-8") as stream:
        for line in stream:
            row = json.loads(line)
            if row.get("status") != "kept" or not row.get("label") or row.get("name") or row.get("vn") in test:
                continue
            try:
                index = int(row["id"].rsplit("_", 1)[1])
            except (KeyError, ValueError, IndexError):
                continue
            by_shot.setdefault(row["shot"], []).append((index, row["label"]))
    order = sorted(by_shot, key=lambda s: hashlib.sha256(f"{seed}:{s}".encode()).hexdigest())[:screens]
    return [(shot, "\n".join(text for _, text in sorted(by_shot[shot]))) for shot in order]


def write_tsv(rows, path, model="pool"):
    with open(path, "w", encoding="utf-8") as out:
        out.write("# id\tmodel\tmodel_sha256\tshot_sha256\tregion\ttext\n")
        for ident, text in rows:
            escaped = text.replace("\t", " ").replace("\r", "").replace("\n", "\\n")
            out.write(f"{ident}\t{model}\t-\t-\t-\t{escaped}\n")


def file_digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def run(binary, directory, dicts, lines, mode, rounds, env=None):
    result = subprocess.run([str(binary), str(directory), dicts, str(lines), mode, str(rounds)],
                            capture_output=True, text=True, timeout=3600, check=False, env=env)
    if result.returncode:
        raise RuntimeError(f"{binary} {mode}: {result.stderr.strip()}")
    rows = [json.loads(line) for line in result.stdout.splitlines()]
    return [r for r in rows if "summary" not in r]


def pct(values, p):
    values = sorted(values)
    if not values:
        return 0
    return values[min(len(values) - 1, round(p / 100 * (len(values) - 1)))]


def stats(values, digits=3):
    values = list(values)
    if not values:
        return {}
    return {"mean": round(statistics.mean(values), digits), "p50": round(pct(values, 50), digits),
            "p95": round(pct(values, 95), digits), "max": round(max(values), digits)}


def summarize(queries, rounds):
    """Per lookup and per screen (one recognized text: all its lookups in this mode)."""
    for q in queries:
        q["console_ms"] = console_ms(q)
    screens = {}
    for q in queries:
        s = screens.setdefault(q["line"], {"us": 0.0, "console_ms": 0.0, "reads": 0, "lookups": 0})
        s["us"] += q["us"]
        s["console_ms"] += q["console_ms"]
        s["reads"] += q["reads"]
        s["lookups"] += 1
    total_us = sum(q["us"] for q in queries)
    out = {
        "lookups": len(queries),
        "with_entry": sum(1 for q in queries if q["entries"]),
        "opened_files": sum(1 for q in queries if q["opens"]),
        "host_us": stats(q["us"] for q in queries),
        "host_lookups_per_s": round(len(queries) / (total_us / 1e6), 1) if total_us and rounds > 1 else None,
        "console_ms": stats((q["console_ms"] for q in queries), 1),
        "arena_peak_max": max((q["arena_peak"] for q in queries), default=0),
    }
    for key in COUNTS:
        out[key] = stats((q[key] for q in queries), 2)
    out["reads_by"] = {k: round(statistics.mean(q["reads_by"][k] for q in queries), 3) for k in KINDS}
    out["bytes_by"] = {k: round(statistics.mean(q["bytes_by"][k] for q in queries), 1) for k in KINDS}
    out["per_screen"] = {
        "screens": len(screens),
        "lookups": stats((s["lookups"] for s in screens.values()), 2),
        "host_us": stats((s["us"] for s in screens.values()), 1),
        "console_ms": stats((s["console_ms"] for s in screens.values()), 1),
        "reads": stats((s["reads"] for s in screens.values()), 1),
    }
    return out


def golden_rows(mode, queries):
    """One row per screen: its lookups' cursors and shown-result digests, in order."""
    rows = {}
    for q in queries:
        rows.setdefault((q["id"], q["model"]), []).append(f"{q['cursor']}:{q['digest']}")
    return [f"{mode}\t{ident}\t{model}\t{' '.join(v)}" for (ident, model), v in rows.items()]


def dictionary_digest(directory, dicts):
    """sha256 of the configured dictionary files, in order (what a golden file is valid for)."""
    h = hashlib.sha256()
    for name in dicts.split(","):
        with open(Path(directory) / name, "rb") as stream:
            for block in iter(lambda: stream.read(1 << 20), b""):
                h.update(block)
    return h.hexdigest()


def check_same(name, before, after):
    """The golden check: the same lookups in the same order, every one showing the same entries."""
    if len(before) != len(after):
        raise RuntimeError(f"{name}: {len(before)} lookups before, {len(after)} after")
    for b, a in zip(before, after):
        if (b["id"], b["model"], b["cursor"]) != (a["id"], a["model"], a["cursor"]) or b["digest"] != a["digest"]:
            raise RuntimeError(f"{name}: {b['id']} {b['model']} at byte {b['cursor']}: shown result changed "
                               f"({b['digest']} -> {a['digest']}, cursor {b['cursor']} -> {a['cursor']})")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bench", type=Path, required=True, help="vjo-dict-bench to measure")
    ap.add_argument("--directory", type=Path, required=True)
    ap.add_argument("--dicts", default="plaintext.vjdict")
    ap.add_argument("--before", type=Path, help="baseline vjo-dict-bench (default: --bench)")
    ap.add_argument("--before-directory", type=Path)
    ap.add_argument("--before-dicts")
    ap.add_argument("--lines", type=Path, action="append", help=f"line TSV (default {LINES.relative_to(ROOT)})")
    ap.add_argument("--pool", type=Path, action="append", help="line-label pool (JSON lines; repeatable)")
    ap.add_argument("--pool-screens", type=int, default=500)
    ap.add_argument("--reserved", type=Path, help="vita-vn-ocr eval/manifests/reserved.json (required with --pool)")
    ap.add_argument("--modes", default="first,words,letters")
    ap.add_argument("--rounds", type=int, default=21, help="1 counts only; N times each lookup N-1 times")
    ap.add_argument("--golden", type=Path, help="fail unless every lookup shows what this golden file recorded")
    ap.add_argument("--write-golden", type=Path)
    ap.add_argument("-o", "--output", type=Path)
    args = ap.parse_args(argv)
    if args.pool and not args.reserved:
        ap.error("--pool needs --reserved (the test VNs to leave out)")
    if args.rounds < 1:
        ap.error("rounds must be positive")
    sets = [(str(p.resolve().relative_to(ROOT)) if p.resolve().is_relative_to(ROOT) else str(p), p, False)
            for p in (args.lines or [LINES])]
    report = {"method": __doc__.split("\n\n")[0].replace("\n", " "), "console_model": CONSOLE,
              "rounds": args.rounds, "dicts": args.dicts, "sets": {}}
    golden = []
    comparing = bool(args.before or args.before_directory or args.before_dicts)
    with tempfile.TemporaryDirectory(prefix="vjo-dict-bench-") as tmp:
        report["pools"] = []
        for n, labels in enumerate(args.pool or []):
            pool = pool_lines(labels, args.reserved, args.pool_screens)
            path = Path(tmp) / f"pool{n}.tsv"
            write_tsv(pool, path)
            name = f"pool:{labels.parent.name}/{labels.name}:{len(pool)} screens"
            sets.append((name, path, True))
            report["pools"].append({"set": name, "labels": str(labels), "labels_sha256": file_digest(labels),
                                    "screens": len(pool),
                                    "lines": sum(t.count("\n") + 1 for _, t in pool),
                                    "selection_sha256": hashlib.sha256("\n".join(s for s, _ in pool).encode()).hexdigest()})
        for name, path, is_pool in sets:
            report["sets"][name] = {}
            for mode in args.modes.split(","):
                after = run(args.bench, args.directory, args.dicts, path, mode, args.rounds)
                entry = {"after": summarize(after, args.rounds)}
                if not is_pool:  # golden files cover committed lines only
                    golden += golden_rows(mode, after)
                if comparing:
                    before = run(args.before or args.bench, args.before_directory or args.directory,
                                 args.before_dicts or args.dicts, path, mode, args.rounds)
                    check_same(f"{name} {mode}", before, after)
                    entry["before"] = summarize(before, args.rounds)
                    entry["golden"] = f"{len(after)} lookups, every shown entry and highlight identical"
                report["sets"][name][mode] = entry
                print(f"{name} {mode}: {len(after)} lookups"
                      + (" identical" if comparing else ""), file=sys.stderr)
    dictionary = dictionary_digest(args.directory, args.dicts)
    if args.write_golden:
        args.write_golden.write_text(
            "# tools/dict_lookup_bench.py golden: what each lookup of the committed lines shows (a digest of the\n"
            "# shown entry's text and highlight; no text). Re-record after an intended change of what is shown:\n"
            "#   python3 tools/dict_lookup_bench.py --bench build/host/vjo-dict-bench --directory DIR --rounds 1 \\\n"
            "#       --write-golden tools/dict_lookup_golden.tsv -o /dev/null\n"
            f"# recorded with\t{args.dicts}\tsha256 {dictionary}\n"
            "# mode\tid\tmodel\tcursor:digest ...\n" + "\n".join(golden) + "\n")
    if args.golden:
        lines = args.golden.read_text().splitlines()
        recorded = next((ln for ln in lines if ln.startswith("# recorded with\t")), "# recorded with\t?")
        want = [ln for ln in lines if ln and not ln.startswith("#")]
        if want != golden:
            got = set(golden)
            first = next((w for w in want if w not in got), None)
            raise SystemExit(f"golden mismatch ({len(want)} rows expected, {len(golden)} measured; golden "
                             f"{recorded[16:].strip()}, now {args.dicts} sha256 {dictionary}); first expected "
                             f"row not reproduced: {first[:200] if first else '(extra rows)'}. A change of what "
                             f"is shown must re-record it (see {args.golden.name}'s header).")
        report["golden_file"] = f"{args.golden.name}: {len(want)} screens identical"
    text = json.dumps(report, ensure_ascii=False, indent=2)
    if args.output:
        args.output.write_text(text + "\n")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
