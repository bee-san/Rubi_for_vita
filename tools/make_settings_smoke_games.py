#!/usr/bin/env python3
"""Synthetic installed games for the Rubi app's Vita3K smoke
(tools/vita3k_settings_smoke.sh): UX0/app/VJOTnnnnn/sce_sys/param.sfo (TITLE) and
icon0.png for N games, with generated titles (Latin, lower case, kana, kanji,
digits; short and long; the letters' edges as host/tests/test_settings_games.c
has them: ゔ, ヴ, ヵ, '{', '~', half-width kana, a byte that is not UTF-8) and
generated icons: 128x128 8-bit palette, a few RGBA, 64x64 and 300x300 ones, and
some missing, damaged (bad CRC) or not PNG at all. No real game names or icons.
Standard library only.

    make_settings_smoke_games.py UX0_DIR [N]
"""
import struct
import sys
import zlib
from pathlib import Path

LATIN = ["Amber", "Bright", "Cobalt", "Distant", "Echo", "Frozen", "Golden", "Hidden", "Iron", "Jade",
         "Kinetic", "Lunar", "Midnight", "Neon", "Orbit", "Paper", "Quiet", "Rapid", "Silent", "Tidal",
         "Upper", "Velvet", "Winter", "Xenon", "Yellow", "Zero"]
NOUNS = ["Archive", "Garden", "Signal", "Harbor", "Lantern", "Circuit"]
KANA = ["あおいそらの", "がっこうの", "かぜの", "さくらの", "ざわめきの", "たいようの", "なつの", "ぱずるの",
        "はるかな", "まよなかの", "やまの", "らせんの", "わすれた", "アーカイブ", "カタログ", "ガラスの",
        "サウンド", "タイム", "ハーモニー", "マップ", "ワールド"]
KANJI = ["空の物語", "海の記憶", "星の欠片", "夢の続き", "森の歌"]
EDGES = ["ゔぁいおりんの".encode(), "ヴァイオリン".encode(), "ヵ月の旅".encode(), b"{Curly} Brace", b"8-bit Quest",
         "ｶﾀｶﾅ ゲーム".encode(), b"~Tilde Tale", b"[Test] Bracket", b"3D Room", "ゖの歌".encode(), "ヶ所めぐり".encode(),
         b"\xff Broken Byte"]


def title(i):
    """The i-th title's bytes."""
    tail = (" - Synthetic Long Edition With Extra Subtitle Words For Wrapping Twice" if i % 7 == 3 else
            "　〜とても長い日本語の副題がついたテスト用の題名で二行を超える〜" if i % 11 == 5 else "")
    k = i % 10
    if k < 4:
        return f"{LATIN[i * 7 % 26]} {NOUNS[i % 6]} {i}{tail}".encode()
    if k == 4:
        t = f"{LATIN[i * 3 % 26]} {NOUNS[(i + 1) % 6]} {i}{tail}"
        return (t[0].lower() + t[1:]).encode()
    if k < 8:
        return f"{KANA[(i // 10 * 3 + k - 5) % 21]}{'ものがたり' if i % 2 else 'きおく'} {i}{tail}".encode()
    if k == 8:
        return f"{KANJI[i % 5]} {i}{tail}".encode()
    return EDGES[i // 10 % 12] + f" {i}{tail}".encode()


def sfo(text):
    value = text + b"\0"
    cap = (len(value) + 3) & ~3
    header = struct.pack("<4sIIII", b"\0PSF", 0x0101, 36, 44, 1)
    entry = struct.pack("<HHIII", 0, 0x0204, len(value), cap, 0)
    return header + entry + b"TITLE\0\0\0" + value.ljust(cap, b"\0")


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def png(w, h, seed, rgba=False):
    bpp = 4 if rgba else 1
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            for c in range(bpp):
                v = (x * 7 + y * 3 + seed * 13 + c * 41) ^ ((x * y) >> 4) ^ ((x ^ seed) & 15)
                raw.append(255 if c == 3 else v & 255)
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6 if rgba else 3, 0, 0, 0))
    if not rgba:
        out += chunk(b"PLTE", bytes(b for i in range(256) for b in (i, 255 - i, (i * 5 + seed) & 255)))
    return out + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")


def main():
    ux0 = Path(sys.argv[1])
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 120
    kinds = {}
    for i in range(n):
        app = ux0 / "app" / f"VJOT{i:05d}" / "sce_sys"
        app.mkdir(parents=True, exist_ok=True)
        (app / "param.sfo").write_bytes(sfo(title(i)))
        icon = app / "icon0.png"
        kind = ("none" if i % 17 == 4 else "damaged" if i % 17 == 9 else "huge" if i % 29 == 13 else
                "not-png" if i % 31 == 17 else "rgba" if i % 23 == 21 else "small" if i % 19 == 18 else "ok")
        kinds[kind] = kinds.get(kind, 0) + 1
        if kind == "none":
            continue
        if kind == "not-png":
            icon.write_bytes(b"GIF89a not a PNG")
            continue
        data = bytearray(png(300, 300, i) if kind == "huge" else png(64, 64, i) if kind == "small" else
                         png(128, 128, i, rgba=kind == "rgba"))
        if kind == "damaged":
            data[len(data) // 2] ^= 0x10  # inside IDAT: its CRC fails
        icon.write_bytes(bytes(data))
    print(f"{n} games: " + ", ".join(f"{v} {k}" for k, v in sorted(kinds.items())))


if __name__ == "__main__":
    main()
