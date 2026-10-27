#!/usr/bin/env python3
"""host/spinner_bench.c on its synthetic box with the release models (rubi-spinner, #353): an animated icon beside the
text or after its last line, at a constant or a variable speed, a moving picture outside the text or behind it, and a
line typing out beside a variable-speed icon, and audit-rubi-spinner's slow typewriters (a character every 10 or 15
frames), typing pauses and icons that follow the typed end. With the line watch (core/vocr_ocr.c) the overlay shows
the text in every one, within the time the job's waits allow, and the same text as on the still screen wherever the
motion is outside the lines' boxes (an icon after the last character, or a picture moving behind the glyphs, is in a
line's crop: its pixels are read with the line's, as on main). Before the watch it showed nothing in 11 of the first 15
(docs/text-stability.md "Animated icons and moving pictures").

    test_spinner_bench.py BENCH MODEL_DIR"""
import json
import subprocess
import sys

# the press's wait (1 s) and the read anyway's grace (150 ms and a finder's run), the recognition, the cascade's
# re-reads and the Shell's retry pace: generous; a regression shows nothing at all
MAX_SHOW_MS = 6000


def main():
    bench, models = sys.argv[1], sys.argv[2]
    p = subprocess.run([bench, models, "--synthetic"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=600,
                       check=False)
    if p.returncode:
        sys.exit(f"{bench} failed ({p.returncode}): {p.stderr.decode(errors='replace')[-400:]}")
    rows = {r["scenario"]: r for r in map(json.loads, p.stdout.decode("utf-8").splitlines())}
    still = rows.get("still")
    bad = []
    if not still or not still["shown"] or not still["text"]:
        sys.exit(f"the still screen's text was not shown: {still}")
    for name, r in rows.items():
        why = []
        if not r["shown"] or not r["text"]:
            why.append(f"not shown ({r['jobs']} jobs, {r['screen_changed']} with the screen changed)")
        elif r["show_ms"] > MAX_SHOW_MS:
            why.append(f"shown {r['show_ms']} ms after the press")
        inside = name.startswith("inline-") or "-follow-" in name or name == "bg-behind"  # motion in a line's crop
        if r["shown"] and not inside and r["text"] != still["text"]:
            why.append("another text than the still screen's")
        if r["memory"]["allocations"] and name != "still":
            why.append(f"{r['memory']['allocations']} allocations (the first job's weights and LM only)")
        print(f"{name:24s} shown {r['shown']} after {r['show_ms']:5d} ms ({r['how']}), {r['jobs']} job(s)"
              + (": " + "; ".join(why) if why else ""))
        if why:
            bad.append(name)
    if len(rows) < 23:
        bad.append(f"{len(rows)} scenarios")
    if bad:
        sys.exit("FAILED: " + ", ".join(bad))
    print(f"ok: {len(rows)} scenarios")


if __name__ == "__main__":
    main()
