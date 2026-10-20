#!/usr/bin/env python3
"""Generate core/hook_cp932.inc: the CP932 (Windows Shift-JIS) double-byte table of
core/hook_text.c, compact, for the native hooks' text (docs/native-hooks.md).

Regenerated from the removed tools/generate_cp932.py (commit c35ebf9^), whose table was
every cell of the 60 lead bytes x 188 trail bytes as a uint16_t (22,560 B). This one keeps
the same mapping (Python's cp932 codec: JIS X 0208, the NEC row 13 and the NEC-selected and
IBM extension rows; the user-defined rows F0-F9, Python's private-use U+E000-U+E757, are left
out: they have no glyph to show) in a few kinds of segments over the cells' linear index
(lead * 188 + trail):

  RUN    a stretch of consecutive code points: its first one;
  RAW    uint16_t code points in a pool (0 inside: a cell without a character);
  DELTA  bytes in a stream, each one a signed step from the previous code point, or 0x80 and
         the next code point (2 bytes, little endian); a checkpoint (offset, code point) every
         VJO_CP932_CK cells, so a lookup decodes at most VJO_CP932_CK - 1 steps.

A segment whose code points equal an earlier one's (the NEC-selected IBM kanji, ED40-EEEC,
are the IBM extension's FA5C-FC4B in the same order) shares its pool or stream. The header
of the output records the SHA-256 of the whole mapping (11,280 little-endian uint16_t, 0 for
no character); host/tests/test_hook_text.c decodes every cell and checks it.

    python3 tools/gen_cp932.py            # writes core/hook_cp932.inc
    python3 tools/gen_cp932.py --check    # exits 1 if the file is stale

Standard library only; the output is deterministic.
"""
import argparse
import hashlib
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "core" / "hook_cp932.inc"

LEADS = list(range(0x81, 0xA0)) + list(range(0xE0, 0xFD))  # 60
TRAILS = list(range(0x40, 0x7F)) + list(range(0x80, 0xFD))  # 188
CELLS = len(LEADS) * len(TRAILS)  # 11,280
CK = 64  # cells between two checkpoints of a DELTA segment (a lookup decodes <= CK - 1 steps)
RUN_MIN = 7  # a run this long (or longer) is its own segment
KIND_RAW, KIND_RUN, KIND_DELTA = 0, 1, 2
# The mapping as Python 3.9-3.13's cp932 codec gives it (checked: a codec that decodes any
# cell differently fails --check and the host test, instead of changing the table silently).
MAP_SHA256 = "ed7aa2abc794fd872c1ce4a2c39b87445e83ae05d7ed74f0781ce73c6537cf44"


def mapping():
    values = []
    for lead in LEADS:
        for trail in TRAILS:
            try:
                text = bytes((lead, trail)).decode("cp932")
            except UnicodeDecodeError:
                text = ""
            cp = ord(text) if len(text) == 1 else 0
            if 0xE000 <= cp <= 0xF8FF:  # the user-defined rows: no glyph to show
                cp = 0
            values.append(cp)
    return values


def digest(values):
    return hashlib.sha256(struct.pack("<%dH" % len(values), *values)).hexdigest()


def stretches(values):
    """Maximal runs of cells that have a character: (start, values)."""
    out, i = [], 0
    while i < len(values):
        if not values[i]:
            i += 1
            continue
        j = i
        while j < len(values) and values[j]:
            j += 1
        out.append((i, values[i:j]))
        i = j
    return out


def split_runs(start, vals):
    """A stretch as pieces: ("run", start, vals) for >= RUN_MIN consecutive code points,
    ("lit", start, vals) for the rest."""
    pieces, i, lit = [], 0, 0
    while i < len(vals):
        j = i
        while j + 1 < len(vals) and vals[j + 1] == vals[j] + 1:
            j += 1
        if j - i + 1 >= RUN_MIN:
            if lit < i:
                pieces.append(("lit", start + lit, vals[lit:i]))
            pieces.append(("run", start + i, vals[i:j + 1]))
            lit = j + 1
        i = j + 1
    if lit < len(vals):
        pieces.append(("lit", start + lit, vals[lit:]))
    return pieces


def delta_encode(vals):
    """(stream bytes, checkpoints [(offset, value)]) for vals, or None if it holds a 0."""
    if 0 in vals:
        return None
    stream, cks = bytearray(), []
    for k, v in enumerate(vals):
        if k % CK == 0:
            cks.append((len(stream), v))
            continue
        step = v - vals[k - 1]
        if -128 < step < 128:
            stream.append(step & 0xFF)
        else:
            stream += bytes((0x80, v & 0xFF, v >> 8))
    return bytes(stream), cks


def build(values):
    segs, raw, stream, cks = [], [], bytearray(), []
    shared = {}  # tuple(vals) -> (kind, at)
    for start, vals in stretches(values):
        for kind, pstart, pvals in split_runs(start, vals):
            if kind == "run":
                segs.append((pstart, len(pvals), KIND_RUN, pvals[0]))
                continue
            key = tuple(pvals)
            if key in shared:
                k, at = shared[key]
                segs.append((pstart, len(pvals), k, at))
                continue
            enc = delta_encode(pvals)
            raw_cost = 2 * len(pvals)
            if enc and len(enc[0]) + 4 * len(enc[1]) < raw_cost:
                base = len(stream)
                at = len(cks)
                stream += enc[0]
                cks += [(base + off, v) for off, v in enc[1]]
                k = KIND_DELTA
            else:
                at = len(raw)
                raw += pvals
                k = KIND_RAW
            shared[key] = (k, at)
            segs.append((pstart, len(pvals), k, at))
    assert len(stream) < 0x10000 and len(raw) < 0x10000
    for s in segs:
        assert s[1] < 0x1000 and s[2] < 16
    return segs, raw, bytes(stream), cks


def decode_cell(segs, raw, stream, cks, i):
    """The C lookup (core/hook_text.c), for the self-check below."""
    lo, hi = 0, len(segs)
    while lo < hi:
        mid = (lo + hi) // 2
        if segs[mid][0] <= i:
            lo = mid + 1
        else:
            hi = mid
    if not lo:
        return 0
    start, n, kind, at = segs[lo - 1]
    k = i - start
    if k >= n:
        return 0
    if kind == KIND_RUN:
        return at + k
    if kind == KIND_RAW:
        return raw[at + k]
    off, v = cks[at + k // CK]
    for _ in range(k % CK):
        b = stream[off]
        if b == 0x80:
            v = stream[off + 1] | stream[off + 2] << 8
            off += 3
        else:
            v = (v + (b - 256 if b > 127 else b)) & 0xFFFF
            off += 1
    return v


def c_rows(items, per_line, fmt):
    rows = []
    for i in range(0, len(items), per_line):
        rows.append("    " + ", ".join(fmt(x) for x in items[i:i + per_line]) + ",")
    return "\n".join(rows)


def render():
    values = mapping()
    got = digest(values)
    if got != MAP_SHA256:
        sys.exit(f"Python's cp932 codec gives mapping SHA-256 {got}, expected {MAP_SHA256}: "
                 "check the differences before updating MAP_SHA256 in tools/gen_cp932.py")
    segs, raw, stream, cks = build(values)
    for i, v in enumerate(values):
        assert decode_cell(segs, raw, stream, cks, i) == v, i
    total = 6 * len(segs) + 2 * len(raw) + len(stream) + 4 * len(cks)
    mapped = sum(1 for v in values if v)
    return (
        "/* Generated by tools/gen_cp932.py (Python's cp932 codec, the user-defined rows left out).\n"
        " * Do not edit: change the generator. */\n"
        f"/* {mapped} double-byte characters in {total} B: {len(segs)} segments ({6 * len(segs)} B), "
        f"{len(raw)} pooled code points ({2 * len(raw)} B),\n"
        f" * {len(stream)} B of steps and {len(cks)} checkpoints ({4 * len(cks)} B). */\n"
        f'#define VJO_CP932_MAP_SHA256 "{MAP_SHA256}"\n'
        f"#define VJO_CP932_CELLS {CELLS}\n"
        f"#define VJO_CP932_CK {CK}\n"
        f"#define VJO_CP932_SEGS {len(segs)}\n"
        "/* {first cell, cells | kind << 12, at}: kind 0 RAW (at: the pool's index), 1 RUN (at: the\n"
        " * first code point), 2 DELTA (at: the first checkpoint's index) */\n"
        f"static const uint16_t vjo_cp932_seg[{len(segs)}][3] = {{\n"
        + c_rows(segs, 4, lambda s: "{%d, 0x%04X, %d}" % (s[0], s[1] | s[2] << 12, s[3])) + "\n};\n"
        f"static const uint16_t vjo_cp932_raw[{len(raw)}] = {{\n"
        + c_rows(raw, 12, lambda v: "0x%04X" % v) + "\n};\n"
        f"static const uint8_t vjo_cp932_steps[{len(stream)}] = {{\n"
        + c_rows(list(stream), 16, lambda b: "0x%02X" % b) + "\n};\n"
        "/* {offset into vjo_cp932_steps, code point} */\n"
        f"static const uint16_t vjo_cp932_ck[{len(cks)}][2] = {{\n"
        + c_rows(cks, 6, lambda c: "{%d, 0x%04X}" % c) + "\n};\n"
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="exit 1 if core/hook_cp932.inc is stale")
    ap.add_argument("-o", "--output", type=pathlib.Path, default=OUT)
    args = ap.parse_args()
    text = render()
    if args.check:
        if not args.output.exists() or args.output.read_text(encoding="utf-8") != text:
            sys.exit(f"{args.output} is stale: run python3 tools/gen_cp932.py")
        return
    args.output.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
