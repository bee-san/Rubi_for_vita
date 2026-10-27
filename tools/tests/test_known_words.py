"""tools/known_words.py (#314 stage C): known_words.txt from an Anki collection or from Anki's "Notes in Plain Text".

fixtures/known-words/anki-plain*.txt are Anki 26.09.3's own exports (its Python backend, export_note_csv) of a
collection that imported host/tests/test_apkg.c's first and second packages (Rubi's Lapis notes, made-up cards) and
had one Basic note added: anki-plain-notetype.txt with HTML and the guid, note type, deck and tags columns,
anki-plain.txt with none of them. The collections here are made with sqlite3 in Anki's two layouts: schema 18
(notetypes and fields tables, today's Anki) and schema 11 (col.models, Anki 2.1.27 and before, and the Rubi app's
own .apkg). host/tests/test_apkg.py runs the tool on a collection Anki itself made, too."""
import json
import os
import sqlite3
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import known_words as kw  # noqa: E402

FIXTURES = ROOT / "tools/tests/fixtures/known-words"
LAPIS = ["Expression", "ExpressionFurigana", "ExpressionReading", "ExpressionAudio", "SelectionText", "MainDefinition"]


def run(*argv):
    """The tool's main on argv (its output path's lines, its exit code)."""
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "known_words.txt")
        with open(os.devnull, "w") as null:
            stdout = sys.stdout
            sys.stdout = null
            try:
                rc = kw.main([*map(str, argv), "-o", out])
            finally:
                sys.stdout = stdout
        if rc:
            return None, rc
        data = Path(out).read_bytes()
        assert not data.startswith(b"\xef\xbb\xbf")
        lines = data.decode("utf-8").split("\n")
        assert lines[-1] == "" and lines[0].startswith("# known_words.txt for Rubi: ")
        return [tuple(line.split("\t")) for line in lines[1:-1]], rc


def collection(path, schema, notes, decks=None):
    """A collection of `notes`: (note type, its fields, [values], [(deck, card type)]) in Anki's schema 11 or 18."""
    db = sqlite3.connect(path)
    db.execute("create table notes (id integer primary key, guid text, mid integer, mod integer, usn integer, "
               "tags text, flds text, sfld integer, csum integer, flags integer, data text)")
    db.execute("create table cards (id integer primary key, nid integer, did integer, ord integer, mod integer, usn "
               "integer, type integer, queue integer, due integer, ivl integer, factor integer, reps integer, lapses "
               "integer, left integer, odue integer, odid integer, flags integer, data text)")
    types, ids = {}, {}
    for name, fields, _v, _c in notes:
        if name not in ids:
            ids[name] = 1000 + len(ids)
            types[ids[name]] = (name, fields)
    deck_ids = {d: 10 + i for i, d in enumerate(decks or ["Default"])}
    if schema == 18:
        db.execute("create table col (id integer primary key, ver integer)")
        db.execute("insert into col values (1, 18)")
        db.execute("create table notetypes (id integer primary key, name text)")
        db.execute("create table fields (ntid integer, ord integer, name text)")
        db.execute("create table decks (id integer primary key, name text)")
        for i, (name, fields) in types.items():
            db.execute("insert into notetypes values (?, ?)", (i, name))
            for o, f in enumerate(fields):
                db.execute("insert into fields values (?, ?, ?)", (i, o, f))
        for d, i in deck_ids.items():
            db.execute("insert into decks values (?, ?)", (i, d.replace("::", "\x1f")))
    else:
        models = {str(i): {"name": n, "flds": [{"name": f, "ord": o} for o, f in enumerate(fs)]}
                  for i, (n, fs) in types.items()}
        db.execute("create table col (id integer primary key, ver integer, models text, decks text)")
        db.execute("insert into col values (1, 11, ?, ?)",
                   (json.dumps(models), json.dumps({str(i): {"name": d} for d, i in deck_ids.items()})))
    card = 1
    for n, (name, _fields, values, cards) in enumerate(notes, start=1):
        db.execute("insert into notes values (?, '', ?, 0, 0, '', ?, '', 0, 0, '')", (n, ids[name], "\x1f".join(values)))
        for deck, ctype in cards or [((decks or ["Default"])[0], 0)]:
            db.execute("insert into cards (id, nid, did, type) values (?, ?, ?, ?)", (card, n, deck_ids[deck], ctype))
            card += 1
    db.commit()
    db.close()


class PlainText(unittest.TestCase):
    def test_anki_s_export_with_its_note_type_column(self):
        """Lapis notes: the word and its ExpressionReading (the third field); the Basic note: its first field, no HTML,
        no reading; each word once."""
        words, rc = run(FIXTURES / "anki-plain-notetype.txt")
        self.assertEqual(rc, 0)
        self.assertEqual(words, [("猫", "ねこ"), ("部屋", "へや"), ("犬", "いぬ"), ("食べる", "たべる"),
                                 ("犬小屋", "いぬごや"), ("本",)])

    def test_anki_s_export_without_columns(self):
        """No note type column: the first field alone (it then matches every card of the spelling), or with
        --reading-column the reading too."""
        words, _ = run(FIXTURES / "anki-plain.txt")
        self.assertEqual(words, [("猫",), ("部屋",), ("犬",), ("食べる",), ("犬小屋",), ("本",)])
        words, _ = run(FIXTURES / "anki-plain.txt", "--reading-column", "3")
        self.assertEqual(words, [("猫", "ねこ"), ("部屋", "へや"), ("犬", "いぬ"), ("食べる", "たべる"),
                                 ("犬小屋", "いぬごや"), ("本",)])

    def test_filters_and_columns(self):
        words, _ = run(FIXTURES / "anki-plain-notetype.txt", "--note-type", "basic")
        self.assertEqual(words, [("本",)])
        words, _ = run(FIXTURES / "anki-plain-notetype.txt", "--deck", "Vita JP Overlay")
        self.assertEqual(len(words), 5)
        words, _ = run(FIXTURES / "anki-plain-notetype.txt", "--deck", "Default")
        self.assertEqual(words, [("本",)])
        # the furigana field as the word: its readings left out
        words, _ = run(FIXTURES / "anki-plain-notetype.txt", "--word-column", "2", "--note-type", "Lapis")
        self.assertEqual(words[:2], [("猫", "ねこ"), ("部屋", "へや")])

    def test_a_filter_needs_its_column(self):
        """audit-rubi-rm4 F3: without the deck or note type column, --deck and --note-type fail (exit 2) and say
        which column to include, rather than keep every note or none."""
        self.assertEqual(run(FIXTURES / "anki-plain.txt", "--deck", "Nope")[1], 2)
        self.assertEqual(run(FIXTURES / "anki-plain.txt", "--note-type", "Lapis")[1], 2)
        p = subprocess.run([sys.executable, str(ROOT / "tools/known_words.py"), str(FIXTURES / "anki-plain.txt"),
                            "--deck", "Nope", "-o", os.devnull], capture_output=True, text=True)
        self.assertEqual(p.returncode, 2)
        self.assertIn('"Include deck name"', p.stderr)
        p = subprocess.run([sys.executable, str(ROOT / "tools/known_words.py"), str(FIXTURES / "anki-plain.txt"),
                            "--note-type", "Lapis", "-o", os.devnull], capture_output=True, text=True)
        self.assertIn('"Include notetype name"', p.stderr)

    def test_plain_text_without_html_is_kept_as_text(self):
        """audit-rubi-rm4 N5: an export made without HTML (#html:false) is text already: a literal <b> or &amp; in
        it is the field's own."""
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "notes.txt"
            src.write_text("#separator:tab\n#html:false\n<b>\tx\n a&amp;b \tx\n", encoding="utf-8")
            self.assertEqual(run(src)[0], [("<b>",), ("a&amp;b",)])

    def test_quoting_and_html(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "notes.txt"
            src.write_text("#separator:tab\n#html:true\n#notetype column:1\n"
                           'Lapis\t"<b>猫</b>&amp;\nx"\tz\tね&#x3053;\n'
                           "Lapis\t \u3000\tz\tなし\n"
                           "Lapis\t今日[きょう]\tz\tきょう\t\n", encoding="utf-8")
            words, _ = run(src)
            self.assertEqual(words, [("猫& x", "ねこ"), ("今日", "きょう")])

    def test_ruby_furigana_and_hash_words(self):
        """audit-rubi-rm4 F4 and N5: HTML ruby's readings (<rt>, <rp>) are not part of the word; a reading in
        furigana form is its kana; a word starting with # is left out (the app reads such a line as a comment)."""
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "notes.txt"
            src.write_text("#separator:tab\n#html:true\n#notetype column:1\n"
                           "Lapis\t<ruby>漢字<RP>(</RP><rt>かんじ</rt><rp>)</rp></ruby>\tz\tかんじ\n"
                           "Lapis\t食べる\tz\t 食[た]べる\n"
                           "Lapis\t日本語\tz\t日本[にほん] 語[ご]\n"
                           "Lapis\t#タグ\tz\tたぐ\n", encoding="utf-8")
            words, _ = run(src)
            self.assertEqual(words, [("漢字", "かんじ"), ("食べる", "たべる"), ("日本語", "にほんご")])
            p = subprocess.run([sys.executable, str(ROOT / "tools/known_words.py"), str(src), "-o",
                                str(Path(tmp) / "out.txt")], capture_output=True, text=True)
            self.assertEqual(p.returncode, 0)
            self.assertIn("1 word(s) starting with # left out", p.stderr)


class Collections(unittest.TestCase):
    NOTES = [
        ("Lapis", LAPIS, ["猫", "猫[ねこ]", "ねこ", "", "", "cat"], [("Mining", 2)]),
        ("Lapis", LAPIS, ["<span>食べる</span>", "食[た]べる", "たべる", "", "", ""], [("Mining", 0)]),
        ("Lapis", LAPIS, ["猫", "猫[ねこ]", "ネコ", "", "", ""], [("Mining::Old", 2)]),
        ("Lapis", LAPIS, ["", "", "", "", "", ""], [("Mining", 2)]),
        ("Basic", ["Front", "Back"], ["front", "back"], [("Other", 2)]),
        ("Mine", ["expression", "Meaning"], ["行く", "to go"], [("Other", 2)]),
    ]
    DECKS = ["Mining", "Mining::Old", "Other"]

    def check(self, schema):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "collection.anki2"
            collection(path, schema, self.NOTES, self.DECKS)
            words, rc = run(path)
            self.assertEqual(rc, 0)
            # every note type with an Expression field (any case); its reading when it has ExpressionReading
            self.assertEqual(words, [("猫", "ねこ"), ("食べる", "たべる"), ("猫", "ネコ"), ("行く",)])
            self.assertEqual(run(path, "--deck", "mining")[0], [("猫", "ねこ"), ("食べる", "たべる"), ("猫", "ネコ")])
            self.assertEqual(run(path, "--deck", "Mining::Old")[0], [("猫", "ネコ")])
            self.assertEqual(run(path, "--reviewed")[0], [("猫", "ねこ"), ("猫", "ネコ"), ("行く",)])
            self.assertEqual(run(path, "--note-type", "mine")[0], [("行く",)])
            self.assertEqual(run(path, "--word-field", "Front", "--reading-field", "Back")[0], [("front", "back")])
            self.assertEqual(run(path, "--reading-field", "")[0], [("猫",), ("食べる",), ("行く",)])
            self.assertEqual(run(path, "--word-field", "Nothing")[1], 2)
            self.assertEqual(run(path, "--deck", "Nowhere")[1], 2)
            # in a package (.apkg, .colpkg) too
            pkg = Path(tmp) / "deck.apkg"
            with zipfile.ZipFile(pkg, "w") as z:
                z.write(path, "collection.anki21" if schema == 18 else "collection.anki2")
                z.writestr("media", "{}")
            self.assertEqual(run(pkg)[0], words)

    def test_schema_18(self):
        self.check(18)

    def test_schema_11(self):
        self.check(11)

    def test_paths_a_uri_would_cut(self):
        """audit-rubi-rm4 F2: a collection under a path with '#', '?', '%' or a space is opened read-only, as it is:
        nothing is created beside it."""
        notes = [("Lapis", LAPIS, ["猫", "猫[ねこ]", "ねこ", "", "", ""], [("Mining", 2)])]
        for folder in ("a b#c", "p%41q", "q?x", "User 1"):
            with tempfile.TemporaryDirectory() as tmp:
                d = Path(tmp) / folder
                d.mkdir()
                path = d / "collection.anki2"
                collection(path, 18, notes, ["Mining"])
                before = sorted(p.name for p in Path(tmp).iterdir())
                self.assertEqual(run(path)[0], [("猫", "ねこ")], folder)
                self.assertEqual(sorted(p.name for p in Path(tmp).iterdir()), before, folder)
                self.assertEqual(sorted(p.name for p in d.iterdir()), ["collection.anki2"], folder)

    def test_what_it_cannot_read(self):
        with tempfile.TemporaryDirectory() as tmp:
            pkg = Path(tmp) / "new.colpkg"
            with zipfile.ZipFile(pkg, "w") as z:
                z.writestr("collection.anki21b", b"\x28\xb5\x2f\xfd")
                z.writestr("collection.anki2", b"")  # (an empty stub, as Anki writes beside a .anki21b: skipped)
            p = subprocess.run([sys.executable, str(ROOT / "tools/known_words.py"), str(pkg), "-o",
                                str(Path(tmp) / "out.txt")], capture_output=True, text=True)
            self.assertEqual(p.returncode, 2)
            self.assertFalse((Path(tmp) / "out.txt").exists())
            other = Path(tmp) / "other.zip"
            with zipfile.ZipFile(other, "w") as z:
                z.writestr("readme.txt", "x")
            self.assertEqual(run(other)[1], 2)


if __name__ == "__main__":
    unittest.main()
