#!/usr/bin/env python3
"""Generate host/tests/hook_re_vectors.inc: test vectors for core/hook_re.c, the regex subset
of LunaTranslator's Vita filters, with Python's re as the reference.

Every pattern core/hook_luna.c hands to the engine (its SUB, SUB_IF, SUBW, SUBW_IF and NAMEPOS
ops, read from the source) is run here with re.sub on inputs made from Luna's own markup and
from the pattern's characters (a fixed pseudo-random sequence), in both of the engine's modes
when Luna runs it on a byte string (bytes: a UTF-8 site's; characters: a CP932 one's), in
characters when Luna runs it on a wide string. host/tests/test_hook_luna.c checks that
vjo_re_sub gives the same bytes for each vector.

ECMAScript (std::regex) and Python's re agree on this subset once three things are spelt out:
'.' is [^\\n\\r] (and U+2028/U+2029 in characters), '$' is the end of the input only (\\Z), and
\\d \\w \\s are ASCII (re.ASCII; bytes patterns already are). A replacement's $1 is \\g<1>.
Python 3.7+ handles empty matches as ECMAScript's String.replace and std::regex_replace do.

    python3 tools/gen_hook_re_vectors.py           # writes host/tests/hook_re_vectors.inc
    python3 tools/gen_hook_re_vectors.py --check   # exits 1 if the file is stale

Standard library only; deterministic (its own random sequence; Python 3.9 or later).
"""
import argparse
import pathlib
import re
import sys
import warnings

# a '[' inside a class: literal in ECMAScript and in re (which only warns about future sets)
warnings.filterwarnings("ignore", category=FutureWarning)

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "core" / "hook_luna.c"
OUT = ROOT / "host" / "tests" / "hook_re_vectors.inc"
RANDOM_PER_PATTERN = 24

# Luna's markup and Japanese text of the kinds its filters clean (vita3k_1.cpp's patterns and
# comments); each pattern also gets random strings over its own characters and these.
SAMPLES = [
    "", "あ", "　", "#n", "#n#n", "\n", "\n\n", "テキスト", "　テキスト　",
    "#Ruby[漢字,かんじ]を読む#n　次の行#Speed[10]", "【　名前　】「台詞」", "【名前】",
    "%Nあ　　い%P%K<br>%CF11F終", "@abc#注#本文$1", "\\n改行\\kの\\c後",
    "#Pos[1,2.5]#Color[3]色#n終わり", "<ruby>ポ|後", "/ruby:漢&かん/字", "#r漢|かん#字@r字@1",
    "た　い　。　！　…　", "#Cmd[x]#n本文!終わり!", "$t漢字@と$[字$/じ$]", "`漢字@", "$C[ffffff]白@$C[]",
    "<CLT 5>強調<CLT>", "[本/ほん]を<a href=\"x\">読<br>む", "<br>　<br>行　<br>　", "a,b,c", "  x  ",
    "#n#Pos[1,-2.5]名前", "台詞#n#Pos[0]名前", "@n@1r@-1r", "<text 本文>", "<name \"名\">",
    "[n]　[n]", "\\[x,1]", "#0a1f;字%r", "%p-1;%C%B", "@_*1w", "@rubyA@B@", "x\ry", "\u2028",
]
EXTRA_CHARS = list("あい漢字。、…？！　\n #n[],.-0129aAz_@$%\\/|<>{}*&`\"')(^")


class Rng:
    """xorshift32: the same sequence on every Python."""

    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFF or 1

    def next(self):
        s = self.s
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        self.s = s
        return s

    def below(self, n):
        return self.next() % n


def c_unescape(lit):
    """The bytes of a C string literal's body (the escapes hook_luna.c uses)."""
    out, i, b = bytearray(), 0, lit.encode("utf-8")
    while i < len(b):
        if b[i] != 0x5C:
            out.append(b[i])
            i += 1
            continue
        c = chr(b[i + 1])
        if c == "x":
            j = i + 2
            while j < len(b) and chr(b[j]) in "0123456789abcdefABCDEF":
                j += 1
            out.append(int(b[i + 2:j], 16) & 0xFF)
            i = j
            continue
        out += {"n": b"\n", "t": b"\t", "r": b"\r", "\\": b"\\", '"': b'"', "'": b"'", "?": b"?"}[c]
        i += 2
    return bytes(out)


def c_args(text, i):
    """The arguments of the macro call whose '(' is at text[i]: a list of items, each a bytes
    literal (adjacent literals joined, each unescaped first, as C does) or a bare token."""
    args, cur, lits = [], "", []
    i += 1
    while True:
        c = text[i]
        if c == '"':
            j = i + 1
            while text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            lits.append(c_unescape(text[i + 1:j]))
            i = j + 1
            continue
        if c in ",)":
            args.append(b"".join(lits) if lits else cur.strip())
            cur, lits = "", []
            if c == ")":
                return args
            i += 1
            continue
        cur += c
        i += 1


def patterns():
    """(kind, pattern bytes, replacement bytes) of every regex op in core/hook_luna.c."""
    text = SRC.read_text(encoding="utf-8")
    start = text.index("static const LunaOp f_")
    ops = []
    for m in re.finditer(r"\b(SUB|SUB_IF|SUBW|SUBW_IF|NAMEPOS)\(", text[start:]):
        kind = m.group(1)
        args = c_args(text, start + m.end() - 1)
        if kind.endswith("_IF"):
            args = args[1:]
            kind = kind[:-3]
        if kind == "NAMEPOS":
            ops.append(("NAMEPOS", args[0], None))
        else:
            ops.append((kind, args[0], args[1]))
    seen, out = set(), []
    for op in ops:
        if op not in seen:
            seen.add(op)
            out.append(op)
    return out


def to_python(pat, chars):
    """An ECMAScript pattern of the subset as a Python re pattern (str for characters, else
    bytes)."""
    s = pat.decode("utf-8") if chars else pat.decode("latin-1")
    out, i, in_class = [], 0, False
    while i < len(s):
        c = s[i]
        if c == "\\":
            out.append(s[i:i + 2])
            i += 2
            continue
        if in_class:
            if c == "]":
                in_class = False
            out.append(c)
        elif c == "[":
            in_class = True
            out.append(c)
            if s[i + 1:i + 2] == "^":
                out.append("^")
                i += 1
            if s[i + 1:i + 2] == "]":  # ECMAScript: an empty class; Python would read a literal ]
                raise ValueError("empty class")
        elif c == ".":
            out.append("[^\\n\\r\\u2028\\u2029]" if chars else "[^\\n\\r]")
        elif c == "$":
            out.append("\\Z")
        else:
            out.append(c)
        i += 1
    py = "".join(out)
    if chars:
        return re.compile(py, re.ASCII)
    return re.compile(py.encode("latin-1"))


def to_python_rep(rep, chars):
    s = rep.decode("utf-8") if chars else rep.decode("latin-1")
    s = s.replace("\\", "\\\\")
    s = re.sub(r"\$([1-9&$])", lambda m: "$" if m.group(1) == "$" else "\\g<0>" if m.group(1) == "&" else
               "\\g<%s>" % m.group(1), s)
    return s if chars else s.encode("latin-1")


def literal_tokens(pat):
    """The runs of literal characters in a pattern (escaped punctuation unescaped), and its
    shorthand classes as a sample character each: what makes the pattern match."""
    s, toks, cur, i = pat.decode("utf-8"), [], "", 0
    sample = {"d": "7", "w": "x", "s": " ", "n": "\n", "S": "y", "D": "a", "W": "!"}
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            e = s[i + 1]
            if e in sample:
                if cur:
                    toks.append(cur)
                toks.append(sample[e])
                cur = ""
            else:
                cur += e
            i += 2
            continue
        if c in "()[]|*+?.^$":
            if cur:
                toks.append(cur)
            cur = ""
        else:
            cur += c
        i += 1
    if cur:
        toks.append(cur)
    return sorted(set(toks))


def inputs_for(pat, rng):
    alphabet = sorted(set(pat.decode("utf-8")) | set(EXTRA_CHARS))
    alphabet = [c for c in alphabet if c not in "\x00"]
    toks = literal_tokens(pat) or ["x"]
    out = list(SAMPLES)
    for _ in range(RANDOM_PER_PATTERN):
        parts = []
        for _ in range(rng.below(9)):
            parts.append(toks[rng.below(len(toks))] if rng.below(2) else alphabet[rng.below(len(alphabet))])
        out.append("".join(parts))
    return out


def c_lit(b):
    """bytes as a C string literal: UTF-8 characters as they are, other bytes and the C
    specials as octal escapes (which never run into the next character)."""
    out, i = [], 0
    while i < len(b):
        x = b[i]
        n = 1 if x < 0x80 else 2 if 0xC2 <= x <= 0xDF else 3 if 0xE0 <= x <= 0xEF else 4 if 0xF0 <= x <= 0xF4 else 0
        chunk = b[i:i + n] if n else b""
        try:
            ch = chunk.decode("utf-8") if n else None
        except UnicodeDecodeError:
            ch = None
        if ch is None or (n == 1 and (x < 0x20 or x == 0x7F or ch in '"\\?')) or ch in "\u2028\u2029":
            out.append("\\%03o" % x)
            i += 1
            continue
        out.append(ch)
        i += n
    return '"' + "".join(out) + '"'


def render():
    rng = Rng(0x48554B53)  # "HUKS"
    pats, inputs, rows, index = [], [], [], {}

    def input_index(t):
        if t not in index:
            index[t] = len(inputs)
            inputs.append(t)
        return index[t]

    for kind, pat, rep in patterns():
        pi = len(pats)
        pats.append((pat, rep))
        modes = (1, 0) if kind == "SUB" else (0,)  # 1: bytes (a UTF-8 site's), 0: characters
        texts = inputs_for(pat, rng)
        for bytes_mode in modes:
            rx = to_python(pat, chars=not bytes_mode)
            for t in texts:
                ii = input_index(t)
                if kind == "NAMEPOS":
                    subj = t.encode("utf-8") if bytes_mode else t
                    m = rx.search(subj)
                    if not m:
                        span = None
                    else:
                        if bytes_mode:
                            pos = lambda k: k
                        else:
                            pos = lambda k: len(t[:k].encode("utf-8"))
                        g = m.span(1) if m.group(1) is not None else (-1, -1)
                        span = (pos(m.start()), pos(m.end()), pos(g[0]) if g[0] >= 0 else -1,
                                pos(g[1]) if g[1] >= 0 else -1)
                    rows.append("    {%d, %d, %d, NULL, %d, {%s}}," % (
                        pi, bytes_mode, ii, 1 if span else 0, ", ".join(str(x) for x in (span or (-1,) * 4))))
                    continue
                prep = to_python_rep(rep, chars=not bytes_mode)
                got = rx.sub(prep, t.encode("utf-8")) if bytes_mode else rx.sub(prep, t).encode("utf-8")
                want = "NULL" if got == t.encode("utf-8") else c_lit(got)
                rows.append("    {%d, %d, %d, %s, 0, {0}}," % (pi, bytes_mode, ii, want))
    return (
        "/* Generated by tools/gen_hook_re_vectors.py: core/hook_luna.c's patterns on Luna-like and\n"
        " * pseudo-random inputs, the expected results from Python's re. Do not edit. */\n"
        f"#define VJO_RE_PATS {len(pats)}\n#define VJO_RE_INPUTS {len(inputs)}\n#define VJO_RE_VECTORS {len(rows)}\n"
        "/* {pattern, its replacement (NULL: a search)} */\n"
        "static const char *const vjo_re_pats[VJO_RE_PATS][2] = {\n"
        + "\n".join("    {%s, %s}," % (c_lit(p), c_lit(r) if r is not None else "NULL") for p, r in pats) + "\n};\n"
        "static const char *const vjo_re_inputs[VJO_RE_INPUTS] = {\n"
        + "\n".join("    %s," % c_lit(t.encode("utf-8")) for t in inputs) + "\n};\n"
        "/* {pattern, bytes mode, input, the result (NULL: the input unchanged), a search's found and its\n"
        " *  match and group 1 spans} */\n"
        "static const struct {\n"
        "    unsigned char pat, bytes;\n"
        "    unsigned short in;\n"
        "    const char *want;\n"
        "    int found;\n"
        "    long span[4];\n"
        "} vjo_re_vectors[VJO_RE_VECTORS] = {\n" + "\n".join(rows) + "\n};\n"
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="exit 1 if host/tests/hook_re_vectors.inc is stale")
    ap.add_argument("-o", "--output", type=pathlib.Path, default=OUT)
    args = ap.parse_args()
    text = render()
    if args.check:
        if not args.output.exists() or args.output.read_text(encoding="utf-8") != text:
            sys.exit(f"{args.output} is stale: run python3 tools/gen_hook_re_vectors.py")
        return
    args.output.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
