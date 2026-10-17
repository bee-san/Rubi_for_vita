#!/usr/bin/env python3
"""Generate core/lapis_notetype.inc: the Lapis note type (third_party/lapis,
release 1.7.0, GPL-3.0) as the schema-11 JSON object the Rubi app's .apkg
export puts into its collection's `col.models` (core/apkg.c).

The note type keeps Lapis's own ID, name, fields and template name, so Anki
imports the export's notes into a Lapis note type a profile already has
(Anki matches note types by ID and by field and template names). Its
modification time is older than any Lapis install, so an import never
overwrites a user's styling (Anki's default "update if newer").

    python3 tools/gen_lapis.py            # writes core/lapis_notetype.inc
    python3 tools/gen_lapis.py --check    # exits 1 if the file is stale

Standard library only; the output is deterministic.
"""
import argparse
import hashlib
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "third_party" / "lapis"
OUT = ROOT / "core" / "lapis_notetype.inc"

# third_party/lapis/README.md: tag 1.7.0's src/ files, verbatim.
PINNED = {
    "front.html": "7bb9993df961d35e7f49b3edec9f7b0f43987f114f3cfdc627a10219c62aef49",
    "back.html": "1d92182a7626b8a14fd80bc82ee07e71a7a387533be3a4124d25fe8b3090a137",
    "styling.css": "51590e7545d43896cb15e494db49b611a9c6e7bbf05c8e0debbf1f98f7ae8920",
}

NOTETYPE_ID = 1667218449922  # Lapis's own (build/genapkg.py LAPIS_TEMPLATE_ID; the release's)
# Older than any Lapis install (the ID's own time, 2022-10-31): with Anki's
# default "update note types if newer", an import never replaces a profile's
# Lapis templates or styling. A profile without Lapis gets these.
NOTETYPE_MTIME = 1667218449
DECK_ID = 1760534400000  # the export's deck, "Vita JP Overlay" (Anki matches decks by name)
DECK_NAME = "Vita JP Overlay"

FONT = "Hiragino Sans Pr6N"
# (name, size, collapsed): the 1.7.0 release's fields, in order.
FIELDS = [
    ("Expression", 50, False),
    ("ExpressionFurigana", 20, False),
    ("ExpressionReading", 20, False),
    ("ExpressionAudio", 10, False),
    ("SelectionText", 20, False),
    ("MainDefinition", 20, False),
    ("DefinitionPicture", 15, False),
    ("Sentence", 35, False),
    ("SentenceFurigana", 15, False),
    ("SentenceAudio", 10, False),
    ("Picture", 20, False),
    ("Glossary", 20, True),
    ("Hint", 20, False),
    ("IsWordAndSentenceCard", 20, False),
    ("IsClickCard", 20, False),
    ("IsSentenceCard", 20, False),
    ("IsAudioCard", 20, False),
    ("PitchPosition", 20, False),
    ("PitchCategories", 20, False),
    ("Frequency", 20, False),
    ("FreqSort", 20, False),
    ("MiscInfo", 15, False),
]
# The release's: Anki's default LaTeX header and footer, and the template's
# requirement (any of Expression, Sentence, Hint, IsClickCard, IsAudioCard).
LATEX_PRE = (
    "\\documentclass[12pt]{article}\n\\special{papersize=3in,5in}\n\\usepackage[utf8]{inputenc}\n"
    "\\usepackage{amssymb,amsmath}\n\\pagestyle{empty}\n\\setlength{\\parindent}{0in}\n\\begin{document}\n"
)
LATEX_POST = "\\end{document}"
REQ = [[0, "any", [0, 7, 12, 14, 16]]]


def source(name):
    data = (SRC / name).read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != PINNED[name]:
        sys.exit(f"{SRC / name}: SHA-256 {digest}, expected {PINNED[name]} (see third_party/lapis/README.md)")
    text = data.decode("utf-8")
    return text[:-1] if text.endswith("\n") else text  # Anki keeps no final newline


def notetype():
    return {
        "id": NOTETYPE_ID,
        "name": "Lapis",
        "type": 0,
        "mod": NOTETYPE_MTIME,
        "usn": -1,
        "sortf": 0,
        "did": DECK_ID,
        "tmpls": [
            {
                "name": "Mining",
                "ord": 0,
                "qfmt": source("front.html"),
                "afmt": source("back.html"),
                "bqfmt": "{{Expression}}",
                "bafmt": "",
                "did": None,
                "bfont": "",
                "bsize": 0,
            }
        ],
        "flds": [
            {
                "name": name,
                "ord": i,
                "sticky": False,
                "rtl": False,
                "font": FONT,
                "size": size,
                "media": [],
                "description": "",
                "plainText": False,
                "collapsed": collapsed,
                "excludeFromSearch": False,
            }
            for i, (name, size, collapsed) in enumerate(FIELDS)
        ],
        "css": source("styling.css"),
        "latexPre": LATEX_PRE,
        "latexPost": LATEX_POST,
        "latexsvg": False,
        "req": REQ,
        "tags": [],
        "vers": [],
    }


def c_literal(text, width=100):
    """ASCII text as concatenated C string literals."""
    out, line = [], ""
    for ch in text:
        piece = {"\\": "\\\\", '"': '\\"'}.get(ch, ch)
        if len(line) + len(piece) > width:
            out.append(line)
            line = ""
        line += piece
    out.append(line)
    return "\n".join(f'    "{part}"' for part in out)


def render():
    body = json.dumps(notetype(), ensure_ascii=True, separators=(",", ":"))
    assert "\n" not in body and all(ord(c) < 128 for c in body)
    names = " \\\n    ".join(f'X({i}, "{name}")' for i, (name, _, _) in enumerate(FIELDS))
    return (
        "/* Generated by tools/gen_lapis.py from third_party/lapis (Lapis 1.7.0,\n"
        " * github.com/donkuri/lapis, GPL-3.0). Do not edit: change the generator. */\n"
        f"#define VJO_LAPIS_NOTETYPE_ID {NOTETYPE_ID}LL\n"
        f"#define VJO_LAPIS_NOTETYPE_MTIME {NOTETYPE_MTIME}\n"
        f"#define VJO_LAPIS_DECK_ID {DECK_ID}LL\n"
        f'#define VJO_LAPIS_DECK_NAME "{DECK_NAME}"\n'
        f"#define VJO_LAPIS_FIELD_COUNT {len(FIELDS)}\n"
        f"#define VJO_LAPIS_FIELD_NAMES(X) \\\n    {names}\n"
        "/* The note type as schema-11 JSON (Anki's NotetypeSchema11), ASCII. */\n"
        f"static const char vjo_lapis_notetype_json[] =\n{c_literal(body)};\n"
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="exit 1 if core/lapis_notetype.inc is stale")
    ap.add_argument("-o", "--output", type=pathlib.Path, default=OUT)
    args = ap.parse_args()
    text = render()
    if args.check:
        if not args.output.exists() or args.output.read_text(encoding="utf-8") != text:
            sys.exit(f"{args.output} is stale: run python3 tools/gen_lapis.py")
        return
    args.output.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
