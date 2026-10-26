#!/usr/bin/env python3
"""Host renders of the overlay on the game's text (API 46, docs/overlay-on-text.md, #309), for review on a PC.

A synthetic visual-novel frame (a drawn backdrop, a dialogue box, text in the repository's OFL fonts: never a game's
frame) is read by Rubi's own code built for the host, and the overlay is drawn where shell/overlay.cpp puts it:

  - the places: `vjo-ocr --chars` on the box (the game worker's job: the finder, VOCR5-N with the 1000k LM and the
    cascade, the CTC frames' cells and the char map core/charmap_build.c writes);
  - the word: Rubi's lookup of the text at the cursor (`vjo-cli`, the bundled dictionary): its match is the mark;
  - the drawing: the word's mark and the lines' underlines (vjo_cmap_span), the popup band beside the word
    (vjo_cmap_popup_top: 196 px, below it when it fits, else above), its entry styled as core/styled.c styles it
    (headword, reading, badge, definition, kanji lines) and the hint line. paf itself does not run on a PC: the
    text inside the band is laid out by this script, not by paf.

  overlay_mock.py --ocr vjo-ocr --cli vjo-cli --models DIR --dict-dir DIR --fonts DIR --out DIR [--scene NAME]
                  [--walk]   (also walk.gif: the cursor stepping through adv-bottom's words, as ▶ and ▼ move it)
  overlay_mock.py --ocr ... --frame PAGE.png --region x0,y0,x1,y1 --cursor N --out FILE   (a local check)

--fonts holds TTF conversions of docs/promo/assets/fonts (Zen Maru Gothic and Noto Sans JP, OFL 1.1): pip's
fonttools turns each .woff2 into a .ttf. Each scene writes NAME.png and NAME.json (the map, the word, the popup)."""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 960, 544
POPUP_H, POPUP_GAP = 196, 6                    # shell/overlay.cpp
MARK = (255, 209, 74, 89)                      # vjo_style_region: 1, 0.82, 0.29, 0.35
UNDERLINE = (255, 255, 255, 77)                # vjo_style_separator: 1, 1, 1, 0.3
PANEL = (0, 0, 0, 230)                         # vjo_style_panel: 0, 0, 0, 0.9
BAND = (8, 8, 10, 255)                         # vjo_style_band
RGB_TEXT, RGB_HL, RGB_READING, RGB_RANK, RGB_DIM = "#ffffff", "#ffd24a", "#9fd0ff", "#8a94a6", "#b8c0cc"
JA_PX, EN_PX = 27, 21                          # font_size_ja 18, font_size_en 14 as the console draws them
HINT = "Tap / ◀▶▲▼ / stick word · R stick scroll · □ region · ○ close"

# Synthetic scenes: each a backdrop, a box, its lines (x, y, text), the region a user would draw and the word.
SCENES = {
    "adv-bottom": {
        "what": "ADV: a dialogue box at the bottom; the popup goes above the word",
        "sky": ((44, 58, 112), (226, 140, 120)), "box": (40, 372, 920, 156), "name": (60, 336, "凪"),
        "lines": [(88, 398, "「明日、一緒に見に行こう！」"), (88, 446, "丘の上で、流れ星が見えるんだって。")],
        "region": (56, 380, 904, 516), "cursor": "一緒に",
    },
    "adv-second-line": {
        "what": "the same frame, the cursor moved down a line (Down, or a tap there)",
        "sky": ((44, 58, 112), (226, 140, 120)), "box": (40, 372, 920, 156), "name": (60, 336, "凪"),
        "lines": [(88, 398, "「明日、一緒に見に行こう！」"), (88, 446, "丘の上で、流れ星が見えるんだって。")],
        "region": (56, 380, 904, 516), "cursor": "見える",
    },
    "nvl-top": {
        "what": "NVL: text over the picture at the top; the popup goes below the word",
        "sky": ((18, 30, 64), (92, 70, 120)), "box": (24, 24, 912, 196), "name": None,
        "lines": [(64, 44, "夕凪の頃。"), (64, 96, "丘の上で、流れ星が見えた。"), (64, 148, "「ねえ、見て！流れ星だ！」")],
        "region": (40, 32, 920, 208), "cursor": "見えた",
    },
    "wrapped-word": {
        "what": "a native hook's line (no line break) that the game wrapped over two rows, the word across the wrap:"
                " the hook's text placed on OCR's characters, two marks",
        "sky": ((40, 70, 96), (200, 170, 120)), "box": (40, 372, 920, 156), "name": (60, 336, "凪"),
        "lines": [(88, 398, "丘の上で流れ星が見えるんだって。明日、一"), (88, 446, "緒に見に行こう！")],
        "hook": "丘の上で流れ星が見えるんだって。明日、一緒に見に行こう！",
        "region": (56, 380, 904, 516), "cursor": "一緒に",
    },
}


def font(fonts, name, px, wght=None):
    f = ImageFont.truetype(os.path.join(fonts, name), px)
    if wght is not None:
        try:
            f.set_variation_by_axes([wght])
        except OSError:
            pass
    return f


def backdrop(sky):
    """A drawn evening sky with hills: no game's art."""
    im = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(im)
    (r0, g0, b0), (r1, g1, b1) = sky
    for y in range(H):
        t = y / (H - 1)
        d.line([(0, y), (W, y)], fill=(int(r0 + (r1 - r0) * t), int(g0 + (g1 - g0) * t), int(b0 + (b1 - b0) * t)))
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.ellipse((560, 120, 820, 380), fill=(255, 214, 160, 90))
    glow = glow.filter(ImageFilter.GaussianBlur(40))
    im = Image.alpha_composite(im.convert("RGBA"), glow)
    d = ImageDraw.Draw(im)
    d.polygon([(0, 330), (180, 270), (420, 310), (640, 250), (960, 300), (960, 544), (0, 544)], fill=(36, 44, 58, 255))
    d.polygon([(0, 380), (260, 340), (520, 370), (760, 330), (960, 360), (960, 544), (0, 544)], fill=(24, 30, 40, 255))
    for x, y in ((120, 60), (300, 110), (470, 40), (860, 80), (700, 30), (210, 160)):
        d.ellipse((x - 2, y - 2, x + 2, y + 2), fill=(255, 255, 240, 220))
    return im


def frame_of(scene, fonts):
    im = backdrop(scene["sky"])
    over = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(over)
    x, y, w, h = scene["box"]
    d.rounded_rectangle((x, y, x + w, y + h), 14, fill=(10, 14, 28, 178), outline=(255, 255, 255, 120), width=2)
    if scene["name"]:
        nx, ny, name = scene["name"]
        d.rounded_rectangle((nx, ny, nx + 110, ny + 40), 10, fill=(150, 60, 90, 230))
        d.text((nx + 18, ny + 4), name, font=font(fonts, "zen-maru-gothic-bold.ttf", 26), fill="#ffffff")
    im = Image.alpha_composite(im, over)
    d = ImageDraw.Draw(im)
    f = font(fonts, "zen-maru-gothic-bold.ttf", 30)
    for lx, ly, text in scene["lines"]:
        d.text((lx, ly), text, font=f, fill="#ffffff", stroke_width=2, stroke_fill="#1a1426")
    return im.convert("RGB")


def read_chars(ocr, models, im, region, hook=None):
    """Rubi's job on the region: the char map (region px), the sentence; with hook, that text placed on OCR's
    characters (a native hook's answer, as the game worker places it)."""
    x0, y0, x1, y1 = region
    crop = im.crop(region).convert("RGBA")
    with tempfile.TemporaryDirectory() as t:
        raw, out, ht = os.path.join(t, "r.rgba"), os.path.join(t, "c.json"), os.path.join(t, "hook.txt")
        with open(raw, "wb") as f:
            f.write(crop.tobytes())
        if hook:
            with open(ht, "w", encoding="utf-8") as f:
                f.write(hook)
        args = [ocr, "--backend", "vocr", "--model", "VOCR5_N_w8g2.vocr", "--lm", "vocr_lm_o3_1000k.vlm",
                "--cascade", "VOCR5_L_w8g2.vocr", "--cascade-ms", "2000", "--counters", "on", "--head-gate", "on",
                "--two-level", "on", "--chars", out] + (["--hook-text", ht] if hook else []) + \
               [models, str(crop.width), str(crop.height), raw]
        p = subprocess.run(args, stdin=subprocess.DEVNULL, capture_output=True, timeout=600)
        if p.returncode or not os.path.exists(out):
            sys.exit(f"vjo-ocr failed: {p.stderr.decode()[-500:]}")
        with open(out, encoding="utf-8") as f:
            c = json.load(f)
    # to frame px: the region's origin (the worker writes screen px; here the region is read as its own frame)
    c["lines"] = [[y0a + y0, y1a + y0, first, count] for y0a, y1a, first, count in c["lines"]]
    c["chars"] = [[off, a + x0, b + x0] for off, a, b in c["chars"]]
    return c


def span(c, start, end):
    """core/charmap.c vjo_cmap_span: a rectangle per line of the characters starting in [start, end)."""
    out = []
    for y0, y1, first, count in c["lines"]:
        cells = [c["chars"][k] for k in range(first, first + count) if start <= c["chars"][k][0] < end]
        if cells and cells[-1][2] > cells[0][1] and y1 > y0:
            out.append((cells[0][1], y0, cells[-1][2] - cells[0][1], y1 - y0))
    return out


def popup_top(c, word):
    """core/charmap.c vjo_cmap_popup_top: below or above all the text when it fits, else beside the word."""
    x, y, w, h = word
    below, above = y + h + POPUP_GAP, y - POPUP_GAP - POPUP_H
    top, bottom = min(l[0] for l in c["lines"]), max(l[1] for l in c["lines"])
    if bottom + POPUP_GAP + POPUP_H <= H:
        return bottom + POPUP_GAP
    if top - POPUP_GAP - POPUP_H >= 0:
        return top - POPUP_GAP - POPUP_H
    if below + POPUP_H <= H:
        return below
    if above >= 0:
        return above
    return H - POPUP_H if y + h // 2 < H // 2 else 0


def lookup(cli, dict_dir, text):
    """Rubi's lookup of the text at the cursor: the first entry (headword, reading, badge, definition, kanji
    lines) and the length of its match in bytes."""
    p = subprocess.run([cli, "--text", text, "--local-dir", dict_dir, "--local-dicts", "plaintext.vjdict", "--nav"],
                       stdin=subprocess.DEVNULL, capture_output=True, timeout=120)
    out = p.stdout.decode()
    body, nav = out.split("\n[navigation]\n")
    entries = body.split("────────────────────────\n", 1)[1].split("\n\n")
    m = re.match(r"1: (.*)", nav.splitlines()[0])
    hl = re.search(r"【(.*?)】", m.group(1)) if m else None
    match = hl.group(1) if hl else ""
    return entries[0].splitlines(), len(match.encode())


def entry_runs(lines):
    """The entry as vjo_styled_entry styles it: [(text, rgb, px)] per line."""
    head = re.match(r"(.*?) \((.*?)\)(?: (#\d+|unranked))?$", lines[0])
    runs = []
    if head:
        runs.append([(head.group(1), RGB_HL, JA_PX), (" (" + head.group(2) + ")", RGB_READING, JA_PX)] +
                    ([(" " + head.group(3), RGB_RANK, EN_PX)] if head.group(3) else []))
    else:
        runs.append([(lines[0], RGB_HL, JA_PX)])
    kanji = 0
    for line in lines[1:]:
        k = re.match(r"^(.)=(.*)$", line)
        if k:
            if not kanji and len(runs) > 1:  # after a definition: one blank line (vjo_kanji_block)
                runs.append([("", RGB_DIM, EN_PX)])
            kanji = 1
            runs.append([(k.group(1), RGB_TEXT, JA_PX), ("=" + k.group(2), RGB_DIM, EN_PX)])
        elif line.startswith("Conjugation:"):
            runs.append([(line, RGB_READING, EN_PX)])
        else:
            runs.append([(line, RGB_DIM, EN_PX)])
    return runs


def draw_runs(d, fonts, runs, x, y, width, bottom):
    """Lines of styled runs, wrapped at width; returns the y reached (clipped at bottom, as the body's view)."""
    fja = font(fonts, "noto-sans-jp.ttf", JA_PX, 400)
    fen = font(fonts, "noto-sans-jp.ttf", EN_PX, 400)
    for line in runs:
        cx, lh = x, 0
        for text, rgb, px in line:
            f = fja if px == JA_PX else fen
            lh = max(lh, 38 if px == JA_PX else 30)
            for ch in re.findall(r"\S+\s*|\s+", text):
                w = d.textlength(ch, font=f)
                if cx + w > x + width and cx > x:
                    y += lh
                    cx = x
                if y + lh > bottom:
                    return y
                d.text((cx, y + (6 if px == EN_PX else 0)), ch, font=f, fill=rgb)
                cx += w
        y += lh
    return y


def compose(im, c, sentence, start, end, entry, fonts):
    out = im.convert("RGBA")
    over = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(over)
    lines = span(c, 0, len(sentence.encode()))[:4]          # OT_LINES underlines
    marks = span(c, start, end)[:2]                          # OT_WORD marks
    word = marks[0]
    if len(marks) > 1:
        x1 = max(marks[0][0] + marks[0][2], marks[1][0] + marks[1][2])
        wx = min(marks[0][0], marks[1][0])
        word = (wx, marks[0][1], x1 - wx, marks[1][1] + marks[1][3] - marks[0][1])
    top = popup_top(c, word)
    d.rectangle((0, top, W - 1, top + POPUP_H - 1), fill=PANEL)
    out = Image.alpha_composite(out, over)
    over = Image.new("RGBA", (W, H), (0, 0, 0, 0))           # the marks draw over the panel (later siblings)
    d = ImageDraw.Draw(over)
    for x, y, w, h in [(x, y + h, w, 2) for x, y, w, h in lines]:
        if not (y < top + POPUP_H and y + h > top):          # a mark the band covers is hidden
            d.rectangle((x, y, x + w - 1, y + h - 1), fill=UNDERLINE)
    for x, y, w, h in marks:
        if not (y < top + POPUP_H and y + h > top):
            d.rectangle((x, y, x + w - 1, y + h - 1), fill=MARK)
    out = Image.alpha_composite(out, over)
    d = ImageDraw.Draw(out)
    d.rectangle((0, top, W - 1, top + 11), fill=BAND)
    d.rectangle((0, top + POPUP_H - 44, W - 1, top + POPUP_H - 1), fill=BAND)
    draw_runs(d, fonts, entry_runs(entry), 20, top + 12, 920, top + POPUP_H - 44)
    d.text((20, top + POPUP_H - 44 + 7), HINT, font=font(fonts, "noto-sans-jp.ttf", 22, 400), fill=RGB_RANK)
    return out.convert("RGB"), {"marks": marks, "underlines": lines, "popup_top": top}


WALK = ("明日", "一緒に", "見に", "行こう", "丘", "上で", "流れ星", "見える")  # adv-bottom's words, ▶ then ▼ and ▶


def scene_frame(s, im, c, cursor, a):
    sent = c["sentence"]
    at = sent.find(cursor)
    if at < 0:
        sys.exit(f"OCR read {sent!r}, without {cursor!r}")
    start = len(sent[:at].encode())
    entry, mlen = lookup(a.cli, a.dict_dir, sent[at:])
    out, geo = compose(im, c, sent, start, start + (mlen or len(cursor.encode())), entry, a.fonts)
    return out, geo, entry


def walk_render(a):
    s = SCENES["adv-bottom"]
    im = frame_of(s, a.fonts)
    c = read_chars(a.ocr, a.models, im, s["region"])
    frames = [scene_frame(s, im, c, w, a)[0].resize((720, 408), Image.LANCZOS) for w in WALK]
    pal = [f.convert("P", palette=Image.ADAPTIVE, colors=128) for f in frames]
    pal[0].save(os.path.join(a.out, "walk.gif"), save_all=True, append_images=pal[1:], duration=900, loop=0,
                optimize=True)
    print(f"walk: {len(frames)} frames, {' '.join(WALK)}")


def scene_render(name, s, a):
    im = frame_of(s, a.fonts)
    c = read_chars(a.ocr, a.models, im, s["region"], s.get("hook"))
    sent = c["sentence"]
    out, geo, entry = scene_frame(s, im, c, s["cursor"], a)
    out.save(os.path.join(a.out, name + ".png"), optimize=True)
    with open(os.path.join(a.out, name + ".json"), "w", encoding="utf-8") as f:
        json.dump({"scene": s["what"], "drawn": [t for _, _, t in s["lines"]], "hook": s.get("hook"), "read": sent,
                   "cursor": s["cursor"],
                   "entry": entry[0], "map_lines": c["lines"], "map_chars": c["chars"], **geo},
                  f, ensure_ascii=False, indent=1)
    print(f"{name}: read {sent!r}; word {entry[0]!r}; popup at y={geo['popup_top']}; marks {geo['marks']}")


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for k in ("ocr", "cli", "models", "dict-dir", "fonts", "out"):
        p.add_argument("--" + k, required=True)
    p.add_argument("--scene", action="append", help="scenes to render (default: all)")
    p.add_argument("--walk", action="store_true", help="also walk.gif")
    p.add_argument("--frame", help="a local check: draw every character's cell of this frame's region")
    p.add_argument("--region", help="x0,y0,x1,y1 of --frame")
    a = p.parse_args(argv)
    if a.frame:  # every cell outlined, for a check by eye (never committed: corpus frames are private)
        im = Image.open(a.frame).convert("RGB")
        if im.size == (480, 272):
            im = im.resize((W, H), Image.BILINEAR)
        region = tuple(int(v) for v in a.region.split(","))
        c = read_chars(a.ocr, a.models, im, region)
        d = ImageDraw.Draw(im)
        for y0, y1, first, count in c["lines"]:
            for k in range(first, first + count):
                _, x0, x1 = c["chars"][k]
                d.rectangle((x0, y0, max(x0, x1 - 1), y1 - 1), outline=(255, 64, 160) if x1 > x0 else (64, 255, 255))
        im.save(a.out)
        print(c["sentence"])
        return 0
    os.makedirs(a.out, exist_ok=True)
    for name in a.scene or list(SCENES):
        scene_render(name, SCENES[name], a)
    if a.walk:
        walk_render(a)
    return 0


if __name__ == "__main__":
    sys.exit(main())
