#!/usr/bin/env python3
"""Deterministic generator for the procedural parts of the Hoshizora Diary art.

It bakes fixed-seed star fields and city skylines into the hand-written SVGs,
replacing whatever sits between the markers

    <!-- GEN:name -->  ...  <!-- /GEN:name -->

and copies the heroine group from vn-char.svg into vn-livearea.svg
(<!-- GEN:heroine -->). Re-running it gives byte-identical files.
Standard library only; run with any Python 3.9+:  python3 make_stars.py
"""
import math
import os
import random
import re

# The SVGs it fills in live in docs/promo/assets/vn.
HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "vn")

STAR_COLOURS = ["#ffffff", "#ffffff", "#e4eaff", "#d6defe", "#fff3dc", "#ffe9f4"]
WINDOW_COLOURS = ["#ffd88a", "#ffc874", "#ffe6a8", "#ffb766", "#fff1c9"]


def f(v):
    """Compact fixed-precision number."""
    s = f"{v:.1f}"
    return s[:-2] if s.endswith(".0") else s


def sparkle(x, y, arm, colour, op):
    """4-point glint: concave star made of four quadratic curves, plus a soft dot."""
    a = arm
    d = (f"M{f(x)} {f(y - a)}Q{f(x)} {f(y)} {f(x + a)} {f(y)}"
         f"Q{f(x)} {f(y)} {f(x)} {f(y + a)}Q{f(x)} {f(y)} {f(x - a)} {f(y)}"
         f"Q{f(x)} {f(y)} {f(x)} {f(y - a)}Z")
    return (f'<circle cx="{f(x)}" cy="{f(y)}" r="{f(arm * 0.55)}" fill="{colour}" opacity="{op * 0.22:.2f}"/>'
            f'<path d="{d}" fill="{colour}" opacity="{op:.2f}"/>')


def stars(seed, w, h_max, n_field, n_band, band, moon, n_glint, clean_x=None):
    """band = (x0, y0, x1, y1, sigma); moon = (cx, cy, keep-out radius).
    clean_x: stars left of this x are fewer and dimmer (title area)."""
    rng = random.Random(seed)
    bx0, by0, bx1, by1, sigma = band
    dx, dy = bx1 - bx0, by1 - by0
    ln = math.hypot(dx, dy)
    ux, uy = dx / ln, dy / ln
    pts = []

    def ok(x, y):
        if math.hypot(x - moon[0], y - moon[1]) < moon[2]:
            return False
        # fade toward the horizon glow
        return rng.random() < (1 - y / (h_max + 30)) ** 0.7

    while len(pts) < n_field:
        x, y = rng.uniform(0, w), rng.uniform(0, h_max)
        if clean_x is not None and x < clean_x and rng.random() < 0.55:
            continue
        if ok(x, y):
            pts.append((x, y, False))
    k = 0
    while k < n_band:
        t = rng.uniform(-0.05, 1.05)
        off = rng.gauss(0, sigma)
        x = bx0 + dx * t - uy * off
        y = by0 + dy * t + ux * off
        if 0 <= x <= w and 0 <= y <= h_max and ok(x, y):
            pts.append((x, y, True))
            k += 1
    out = []
    glints = []
    for x, y, in_band in pts:
        u = rng.random()
        r = 0.35 + (0.9 if in_band else 1.25) * u ** 3
        op = 0.35 + 0.65 * rng.random()
        col = rng.choice(STAR_COLOURS)
        if clean_x is not None and x < clean_x:
            op *= 0.6
            r = min(r, 1.0)
        glints.append((r * op, x, y, col))
        out.append(f'<circle cx="{f(x)}" cy="{f(y)}" r="{r:.2f}" fill="{col}" opacity="{op:.2f}"/>')
    # brightest few become sparkles (never in the clean area)
    glints.sort(reverse=True)
    chosen = []
    for _, x, y, col in glints:
        if len(chosen) >= n_glint:
            break
        if y > h_max * 0.75:  # no glints in the horizon glow
            continue
        if clean_x is not None and x < clean_x:
            continue
        if any(math.hypot(x - cx, y - cy) < 60 for cx, cy, _ in chosen):
            continue
        chosen.append((x, y, col))
    for x, y in [(c[0], c[1]) for c in chosen]:
        arm = rng.uniform(3.5, 8.0)
        out.append(sparkle(x, y, arm, "#ffffff", rng.uniform(0.75, 1.0)))
    return "\n".join(out)


def skyline(seed, w, base, layers):
    """layers: list of dict(fill, hmin, hmax, wmin, wmax, win_p, win, tower_p, lights)."""
    rng = random.Random(seed)
    out = []
    for L in layers:
        rects = []
        wins = []
        x = -12.0
        while x < w + 12:
            bw = rng.uniform(L["wmin"], L["wmax"])
            bh = rng.uniform(L["hmin"], L["hmax"])
            if rng.random() < L["tower_p"]:
                bh *= rng.uniform(1.5, 2.1)
                bw *= 0.7
            top = base - bh
            rects.append((x, top, bw, bh))
            # stepped crown / antenna
            if rng.random() < 0.3 and bw > 16:
                sw = bw * rng.uniform(0.35, 0.6)
                sh = rng.uniform(3, 8)
                rects.append((x + (bw - sw) / 2, top - sh, sw, sh))
            if rng.random() < 0.22:
                ah = rng.uniform(5, 14)
                rects.append((x + bw * rng.uniform(0.3, 0.7), top - ah, 1.2, ah))
                if L["lights"]:
                    wins.append(f'<circle cx="{f(rects[-1][0] + 0.6)}" cy="{f(top - ah)}" r="1.3" '
                                f'fill="#ff6a6a" opacity="0.85"/>')
            # windows
            ww, wh, gx, gy = L["win"]
            cols = int((bw - 4) // gx)
            rows = int((bh - 5) // gy)
            for cy in range(rows):
                for cx in range(cols):
                    if rng.random() < L["win_p"]:
                        wx = x + 3 + cx * gx
                        wy = top + 4 + cy * gy
                        op = rng.uniform(0.45, 1.0)
                        wins.append(f'<rect x="{f(wx)}" y="{f(wy)}" width="{ww}" height="{wh}" '
                                    f'fill="{rng.choice(WINDOW_COLOURS)}" opacity="{op:.2f}"/>')
            x += bw + rng.uniform(-2, 3)
        d = "".join(f"M{f(rx)} {f(ry)}h{f(rw)}v{f(rh + 20)}h{f(-rw)}Z" for rx, ry, rw, rh in rects)
        out.append(f'<path d="{d}" fill="{L["fill"]}"/>')
        out.append("<g>" + "".join(wins) + "</g>")
    return "\n".join(out)


def replace(text, name, body):
    pat = re.compile(r"(<!-- GEN:%s -->)(.*?)(<!-- /GEN:%s -->)" % (name, name), re.S)
    if not pat.search(text):
        raise SystemExit(f"marker GEN:{name} missing")
    return pat.sub(lambda m: m.group(1) + "\n" + body + "\n" + m.group(3), text)


def patch(path, parts):
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    for name, body in parts.items():
        text = replace(text, name, body)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print("wrote", os.path.basename(path))


FAR = dict(fill="#2c2152", hmin=10, hmax=30, wmin=14, wmax=34, win_p=0.07,
           win=(1.4, 1.6, 4, 5), tower_p=0.08, lights=False)
NEAR = dict(fill="#161330", hmin=10, hmax=40, wmin=16, wmax=46, win_p=0.24,
            win=(2.2, 2.6, 6, 7), tower_p=0.12, lights=True)


def main():
    # 960x544 background: moon at (300, 96), Milky Way from upper right to lower left
    bg = os.path.join(HERE, "vn-bg.svg")
    patch(bg, {
        "stars": stars(20261015, 960, 330, 300, 230, (980, -30, 230, 360, 34), (300, 96, 46), 14),
        "skyline": skyline(4242, 960, 342, [FAR, NEAR]),
    })
    # 840x500 LiveArea: moon at (560, 86), band kept to the right half
    la = os.path.join(HERE, "vn-livearea.svg")
    if not os.path.exists(la):
        return
    with open(os.path.join(HERE, "vn-char.svg"), encoding="utf-8") as fh:
        m = re.search(r"<!-- HEROINE:BEGIN -->(.*?)<!-- HEROINE:END -->", fh.read(), re.S)
    if not m:
        raise SystemExit("HEROINE markers missing in vn-char.svg")
    patch(la, {
        "stars": stars(5150, 840, 372, 230, 170, (880, -20, 420, 380, 30), (560, 86, 44), 10,
                       clean_x=462),
        "skyline": skyline(777, 840, 384, [FAR, NEAR]),
        "heroine": m.group(1).strip(),
    })


if __name__ == "__main__":
    main()
