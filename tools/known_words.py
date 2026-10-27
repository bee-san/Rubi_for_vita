#!/usr/bin/env python3
"""known_words.txt for Rubi (#314 stage C; docs/anki.md#known-words): the words you have in Anki.

    python3 tools/known_words.py SOURCE [-o known_words.txt] [--word-field Expression]
                                 [--reading-field ExpressionReading] [--deck NAME] [--note-type NAME] [--reviewed]
                                 [--word-column N] [--reading-column N]

SOURCE is one of:

- an Anki collection: the profile's collection.anki2 (close Anki first, or copy it), or a .colpkg / .apkg whose
  collection.anki21 or collection.anki2 is inside it (Anki 2.1.50's newer collection.anki21b is compressed with zstd:
  export with "Support older Anki versions" ticked, or use the collection itself). Every note whose note type has the
  word field (Expression; any case) gives a line: that field, a TAB and the reading field (ExpressionReading) when
  the note type has it. --deck keeps the notes with a card in that deck or below it, --note-type those of that note
  type, --reviewed those with a card that is not new.
- Anki's File > Export > "Notes in Plain Text" (.txt): its fields have no names, so the word is the note's first
  field and the reading its third when the export's note type column (Include notetype name) names a Lapis note type
  (its ExpressionReading); without one, no reading: the word then matches every card of its spelling. --word-column
  and --reading-column (the note's field numbers, from 1) choose others.

Each field is read as Anki shows it: without HTML (a plain-text export made without HTML is kept as it is), its
character references decoded, a word's furigana (漢字[かんじ], or HTML ruby's readings) without the readings, a
reading in furigana form (漢字[かんじ]) as its kana. Empty words, and words starting with # (the Rubi app reads such
a line as a comment), are left out, and repeated lines kept once. Copy the file to
ux0:data/VitaJPOverlay/known_words.txt on the Vita, then on the Rubi app's Anki page choose Known words: Import. The
next export tags the cards of those words `known`.

Only the standard library (sqlite3, zipfile, csv); nothing is sent anywhere. The collection is opened read-only.
"""
import argparse
import csv
import html
import io
import json
import os
import pathlib
import re
import sqlite3
import sys
import tempfile
import zipfile

TAG = re.compile(r"<[^>]*>")
BREAK = re.compile(r"<br\s*/?>", re.I)
# HTML ruby's readings and fallback parentheses (<ruby>漢字<rt>かんじ</rt></ruby>): what is shown above the word
RUBY_TEXT = re.compile(r"<(rt|rp)\b[^>]*>.*?</\1\s*>", re.I | re.S)
RUBY = re.compile(r"\[[^\]]*\]")
# Anki's furigana syntax, as its {{kana:}} filter reads it: a base (after a space) and its reading in brackets
FURIGANA = re.compile(r" ?([^ >]+?)\[(.+?)\]")
SPACES = re.compile(r"[ \t\r\n\u00a0\u3000]+")


def clean(value, word=False, reading=False, as_html=True):
    """A field as text: no HTML (as_html: a plain-text export's fields are text already), its references decoded,
    spaces collapsed; a word without its furigana's readings (Anki's 漢字[かんじ] and the spaces before each part, or
    HTML ruby's <rt>), a reading in kana (漢字[かんじ] is かんじ, as Anki's kana: filter shows it)."""
    text = value
    if as_html:
        text = html.unescape(TAG.sub("", RUBY_TEXT.sub("", BREAK.sub(" ", text))))
    if word and "[" in text:
        text = RUBY.sub("", text).replace(" ", "")
    elif reading and "[" in text:
        text = FURIGANA.sub(r"\2", text).replace(" ", "")
    return SPACES.sub(" ", text).strip()


class SourceError(Exception):
    pass


def open_collection(path, tmp):
    """A sqlite3 connection to the collection at path (a .colpkg/.apkg's, extracted into tmp), read-only."""
    with open(path, "rb") as f:
        magic = f.read(16)
    if magic.startswith(b"PK"):
        with zipfile.ZipFile(path) as z:
            names = z.namelist()
            if "collection.anki21b" in names and "collection.anki21" not in names:
                # (its collection.anki2, when there is one, is Anki's stub that asks for a newer Anki)
                raise SourceError("this package has only collection.anki21b (zstd); export it again with "
                                  "\"Support older Anki versions\" ticked, or use the profile's collection.anki2")
            for name in ("collection.anki21", "collection.anki2"):
                if name in names:
                    out = os.path.join(tmp, name)
                    with open(out, "wb") as f:
                        f.write(z.read(name))
                    path = out
                    break
            else:
                raise SourceError("no collection.anki21 or collection.anki2 in this package")
    elif not magic.startswith(b"SQLite format 3"):
        raise SourceError("not an Anki collection")
    # (a file: URI percent-encodes '#', '?', '%' and spaces: a raw path would cut there and open it read-write)
    return sqlite3.connect(pathlib.Path(path).resolve().as_uri() + "?mode=ro", uri=True)


def note_types(db):
    """{note type id: (name, [field names in order])}, from schema 18's tables or schema 11's col.models."""
    tables = {r[0] for r in db.execute("select name from sqlite_master where type = 'table'")}
    if "notetypes" in tables and "fields" in tables:
        names = dict(db.execute("select id, name from notetypes"))
        fields = {}
        for ntid, _ord, name in db.execute("select ntid, ord, name from fields order by ntid, ord"):
            fields.setdefault(ntid, []).append(name)
        return {i: (names[i], fields.get(i, [])) for i in names}
    models = json.loads(db.execute("select models from col").fetchone()[0])
    return {int(i): (m["name"], [f["name"] for f in sorted(m["flds"], key=lambda f: f["ord"])])
            for i, m in models.items()}


def deck_names(db):
    """{deck id: name with :: between its levels}."""
    tables = {r[0] for r in db.execute("select name from sqlite_master where type = 'table'")}
    if "decks" in tables:
        return {i: n.replace("\x1f", "::") for i, n in db.execute("select id, name from decks")}
    decks = json.loads(db.execute("select decks from col").fetchone()[0])
    return {int(i): d["name"] for i, d in decks.items()}


def index_of(names, wanted):
    low = [n.lower() for n in names]
    return low.index(wanted.lower()) if wanted and wanted.lower() in low else None


def from_collection(db, args):
    types = note_types(db)
    decks = deck_names(db)
    keep_deck = None
    if args.deck:
        want = args.deck.lower()
        keep_deck = {i for i, n in decks.items() if n.lower() == want or n.lower().startswith(want + "::")}
        if not keep_deck:
            raise SourceError(f"no deck named {args.deck!r}")
    cards = {}
    for nid, did, ctype in db.execute("select nid, did, type from cards"):
        c = cards.setdefault(nid, [False, False])
        c[0] |= keep_deck is None or did in keep_deck
        c[1] |= ctype != 0
    found = 0
    for nid, mid, flds in db.execute("select id, mid, flds from notes order by id"):
        name, fields = types.get(mid, ("", []))
        if args.note_type and name.lower() != args.note_type.lower():
            continue
        w, r = index_of(fields, args.word_field), index_of(fields, args.reading_field)
        if w is None:
            continue
        found += 1
        deck_ok, reviewed = cards.get(nid, [keep_deck is None, False])
        if not deck_ok or (args.reviewed and not reviewed):
            continue
        values = flds.split("\x1f")
        word = clean(values[w], word=True) if w < len(values) else ""
        reading = clean(values[r], reading=True) if r is not None and r < len(values) else ""
        yield word, reading
    if not found:
        raise SourceError(f"no note type with a field named {args.word_field!r}: choose one with --word-field")


def from_plain_text(path, args):
    """Anki's "Notes in Plain Text": its # headers, then a row per note (tab-separated, quoted as CSV)."""
    with open(path, encoding="utf-8-sig", newline="") as f:
        text = f.read()
    meta, body = {}, io.StringIO()
    lines = text.splitlines(keepends=True)
    i = 0
    while i < len(lines) and lines[i].startswith("#"):
        key, _, value = lines[i][1:].rstrip("\r\n").partition(":")
        meta[key.strip().lower()] = value.strip()
        i += 1
    body.write("".join(lines[i:]))
    body.seek(0)
    sep = {"tab": "\t", "comma": ",", "semicolon": ";", "space": " ", "pipe": "|", "colon": ":"}.get(
        meta.get("separator", "tab"), "\t")
    special = {}
    for key in ("guid column", "notetype column", "deck column", "tags column"):
        if key in meta and meta[key].isdigit():
            special[int(meta[key]) - 1] = key.split()[0]
    # a filter needs its column: never keep every note silently, or none
    if args.deck and "deck" not in special.values():
        raise SourceError("--deck needs the export's deck column: export again with \"Include deck name\" ticked")
    if args.note_type and "notetype" not in special.values():
        raise SourceError("--note-type needs the export's note type column: export again with \"Include notetype "
                          "name\" ticked")
    as_html = meta.get("html", "true").lower() != "false"
    for row in csv.reader(body, delimiter=sep, quotechar='"'):
        if not row:
            continue
        notetype = next((row[c] for c, k in special.items() if k == "notetype" and c < len(row)), "")
        if args.note_type and notetype.lower() != args.note_type.lower():
            continue
        deck = next((row[c] for c, k in special.items() if k == "deck" and c < len(row)), None)
        if args.deck and deck is not None and not (deck.lower() == args.deck.lower() or
                                                    deck.lower().startswith(args.deck.lower() + "::")):
            continue
        fields = [v for c, v in enumerate(row) if c not in special]
        w = args.word_column - 1
        r = args.reading_column - 1 if args.reading_column else (2 if "lapis" in notetype.lower() else None)
        word = clean(fields[w], word=True, as_html=as_html) if 0 <= w < len(fields) else ""
        reading = clean(fields[r], reading=True, as_html=as_html) if r is not None and 0 <= r < len(fields) else ""
        yield word, reading


def known_words(source, args):
    """The (word, reading) pairs of source, each once, in its order; and the words left out because the Rubi app
    would read their line as a comment (a word starting with '#')."""
    seen, out, hashes = set(), [], 0
    with tempfile.TemporaryDirectory() as tmp:
        with open(source, "rb") as f:
            magic = f.read(16)
        if magic.startswith(b"SQLite format 3") or magic.startswith(b"PK"):
            db = open_collection(source, tmp)
            try:
                pairs = list(from_collection(db, args))
            finally:
                db.close()
        else:
            pairs = list(from_plain_text(source, args))
    for word, reading in pairs:
        word = word.replace("\t", " ")
        reading = reading.replace("\t", " ")
        if not word or (word, reading) in seen:
            continue
        seen.add((word, reading))
        if word.startswith("#"):
            hashes += 1
            continue
        out.append((word, reading))
    return out, hashes


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("source", help="an Anki collection, .colpkg or .apkg, or a Notes in Plain Text export")
    p.add_argument("-o", "--output", default="known_words.txt", help="where to write it (default known_words.txt)")
    p.add_argument("--word-field", default="Expression", help="a collection's word field (default Expression)")
    p.add_argument("--reading-field", default="ExpressionReading",
                   help="a collection's reading field (default ExpressionReading; '' for none)")
    p.add_argument("--deck", help="only notes with a card in this deck (or below it)")
    p.add_argument("--note-type", help="only notes of this note type")
    p.add_argument("--reviewed", action="store_true", help="only notes with a card that is not new (a collection)")
    p.add_argument("--word-column", type=int, default=1, help="a plain text export's word field, from 1 (default 1)")
    p.add_argument("--reading-column", type=int, default=0,
                   help="a plain text export's reading field, from 1 (default: 3 for a Lapis note type, else none)")
    args = p.parse_args(argv)
    try:
        words, hashes = known_words(args.source, args)
    except (SourceError, sqlite3.DatabaseError, zipfile.BadZipFile, UnicodeDecodeError, OSError) as e:
        print(f"known_words.py: {args.source}: {e}", file=sys.stderr)
        return 2
    with open(args.output, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# known_words.txt for Rubi: {len(words)} words from {os.path.basename(args.source)} "
                "(tools/known_words.py)\n")
        for word, reading in words:
            f.write(f"{word}\t{reading}\n" if reading else f"{word}\n")
    with_reading = sum(1 for _, r in words if r)
    print(f"{args.output}: {len(words)} words, {with_reading} with a reading; copy it to "
          "ux0:data/VitaJPOverlay/known_words.txt, then Known words: Import on the Rubi app's Anki page")
    if hashes:
        print(f"{hashes} word(s) starting with # left out: the Rubi app reads such a line as a comment",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
