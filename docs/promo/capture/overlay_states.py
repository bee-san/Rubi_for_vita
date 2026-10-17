#!/usr/bin/env python3
"""overlay_states.py: the overlay at each beat of the promo, as a Rubi checkout draws it.

    overlay_states.py CHECKOUT LOOKUPS.jsonl DICT_SHA256 > src/overlay-states.json

LOOKUPS.jsonl is overlay_lookups' output (the checkout's own lookups, each entry's spans exactly as
vjo_styled_entry draws them, Right as the overlay moves it along the header's displayed rows, and the lookup
a tap on each character makes). The s1 beats press Right twice on a one-row header (checked here); the s3 line
is only opened (the film's last beat: L + R in the game, right after the install). This script lays out each
beat's body in the order shell/overlay.cpp
render() writes it, with the strings read from the checkout's own sources (so a rename or a new string on
main shows up here, and a string that is gone stops the build):

  ["Looking up words…" | status] [the Anki queue line] [the Anki status] ["Queued for Anki\\n"] the entry

and the hint as render() formats it (shell/overlay.cpp). Card mining is on in the s1 beats (the hint's
"× queue" and the queue line) and off in the s3 beats, as after a first install (core/config.c: off by
default). Enter and cancel are × and ○ (a western console, core/buttons.h).
"""
import json
import re
import subprocess
import sys
from pathlib import Path

ENTER, CANCEL = "×", "○"
DIM, RANK = "#b8c0cc", "#8a94a6"  # core/styled.h VJO_RGB_DIM, VJO_RGB_RANK


def source_strings(root):
    ov = (root / "shell" / "overlay.cpp").read_text(encoding="utf-8")
    anki = (root / "shell" / "anki.c").read_text(encoding="utf-8")

    def need(text, pattern, what):
        m = re.search(pattern, text)
        if not m:
            sys.exit(f"overlay_states.py: {what} is not in the checkout's sources ({pattern})")
        return m

    q = need(ov, r'sceClibSnprintf\(queue, sizeof\(queue\), "Anki queue: %d([^"]*)"', "the Anki queue line")
    nav = need(ov, r'VJO_HIGHLIGHT_WORD \? "([^"]+)"', "the hint's Whole word part")
    hint = need(ov, r'"<font color=\\"#8a94a6\\">%s · %s([^"]*) · %s close</font>"', "the hint's format")
    anki_part = need(ov, r'sceClibSnprintf\(anki, sizeof\(anki\), "%s queue · ", s_btn\.enter_glyph\)', "the hint's Anki part")
    need(ov, r'vjo_styled_puts\(&body, "Looking up words…", VJO_RGB_DIM, en\)', "Looking up words…")
    need(ov, r'vjo_styled_puts\(&body, "Queued for Anki\\n", VJO_RGB_DIM, en\)', "Queued for Anki")
    need(anki, r'publish\(seq, "Saving card…"', "Saving card…")
    need(anki, r': "Saved for Anki",', "Saved for Anki")
    return {"queue_tail": q.group(1), "nav": nav.group(1), "hint_mid": hint.group(1)}


def hint(s, mining):
    anki = f"{ENTER} queue · " if mining else ""
    return f"{s['nav']} · {anki}{s['hint_mid']} · {CANCEL} close"


def body(s, step, *, mining, pending, anki_status=None, queued=False, in_flight=False):
    out = []

    def puts(text, rgb, px=21):
        if text:
            out.append({"text": text, "rgb": rgb, "px": px})

    if in_flight:  # the cursor moved: its lookup runs (cursor_matches is false), no entry yet
        puts("Looking up words…", DIM)
    if mining:
        if out:
            puts("\n", DIM)
        puts(f"Anki queue: {pending}{s['queue_tail']}", DIM)
    if anki_status:
        if out:
            puts("\n", DIM)
        puts(anki_status, DIM)
    if not in_flight:
        if out:
            puts("\n", DIM)
        if queued:
            puts("Queued for Anki\n", DIM)
        out += step["entry_spans"]
    return out


def main():
    root, lookups, dict_sha = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
    s = source_strings(root)
    commit = subprocess.run(["git", "-C", str(root), "rev-parse", "--short=7", "HEAD"], capture_output=True,
                            text=True, check=True).stdout.strip()
    lines = {}
    for line in lookups.read_text(encoding="utf-8").splitlines():
        o = json.loads(line)
        lines[o["input"]] = o
    m2 = lines["「また明日、一緒に見に行こう！」"]
    s3 = lines["「キラキラ……。\n子供の頃みたい。」"]

    def whole(w, o):
        if not w or "entry_spans" not in w or w.get("entry_truncated"):
            sys.exit(f"overlay_states.py: no whole entry at {w} in {o['sentence']}")
        return w

    # s1: open, Right, Right, each Right moving on the header's one row
    if m2["rows"] != 1 or [w["result"] for w in m2["walk"][1:3]] != ["moved", "moved"]:
        sys.exit(f"overlay_states.py: Right, Right on {m2['sentence']!r} is not two moves: {m2['walk']}")
    a = [whole(w["lookup"], m2) for w in m2["walk"][:3]]
    # s3: open only (the first lookup the overlay makes on its own)
    b = [whole(s3["walk"][0]["lookup"], s3)]

    def state(o, step, hl, **kw):
        mining = kw.get("mining", False)
        return {"header": o["sentence"], "hl": hl, "body": body(s, step, **kw), "hint": hint(s, mining),
                "lookup": {k: step[k] for k in ("matched", "spelling", "reading", "rank", "badge", "cursor_cp", "hl_cp")}}

    one = lambda st: [st["cursor_cp"], st["cursor_cp"] + 1]  # an in-flight lookup highlights the cursor's character
    states = {
        # s1-m2: 「また明日、一緒に見に行こう!」, card mining on, the queue empty
        "s1-m2-ov01-open": state(m2, a[0], a[0]["hl_cp"], mining=True, pending=0),
        "s1-m2-ov02-right-inflight": state(m2, a[1], one(a[1]), mining=True, pending=0, in_flight=True),
        "s1-m2-ov03-issho": state(m2, a[1], a[1]["hl_cp"], mining=True, pending=0),
        "s1-m2-ov04-issho-saving": state(m2, a[1], a[1]["hl_cp"], mining=True, pending=0, anki_status="Saving card…"),
        "s1-m2-ov05-issho-saved": state(m2, a[1], a[1]["hl_cp"], mining=True, pending=1, anki_status="Saved for Anki",
                                        queued=True),
        "s1-m2-ov06-miniiku": state(m2, a[2], a[2]["hl_cp"], mining=True, pending=1),
        "s1-m2-ov06b-miniiku-saving": state(m2, a[2], a[2]["hl_cp"], mining=True, pending=1, anki_status="Saving card…"),
        "s1-m2-ov07-miniiku-saved": state(m2, a[2], a[2]["hl_cp"], mining=True, pending=2, anki_status="Saved for Anki",
                                          queued=True),
        # s3-m1: 「キラキラ……。/子供の頃みたい。」 right after a first install: mining off; opened
        "s3-m1-ov03-open": state(s3, b[0], b[0]["hl_cp"], mining=False, pending=0),
    }
    json.dump({"source": f"capture/overlay_states.py: Rubi {commit}'s shell/overlay.cpp render() order and "
                         f"strings, its own lookups (capture/overlay_lookups.c) in the released plaintext.vjdict "
                         f"(sha256 {dict_sha[:16]}…), entries as vjo_styled_entry draws them",
               "states": states}, sys.stdout, ensure_ascii=False, indent=1)
    print()


if __name__ == "__main__":
    main()
