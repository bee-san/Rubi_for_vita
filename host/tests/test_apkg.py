#!/usr/bin/env python3
"""The Overlay app's .apkg packages, opened as Anki would open them.

    python3 host/tests/test_apkg.py BUILD/vjo-apkg-tests

The C test binary writes the packages (`--write DIR`: first, second, all and
many, from records the overlay's Enter writes and from earlier builds'), and
this checks them with Python's zipfile and sqlite3: the ZIP (stored entries,
CRC-32s, no `meta`, the media map), the SQLite collection (PRAGMA
integrity_check, schema 11, the Lapis note type exactly as
third_party/lapis/ has it, the deck), every note (22 fields, its GUID from its
card's queue ID, sort field and checksum as Anki computes them, its card) and
that a later package gives the same note the same GUID and fields.

Then, when the `anki` package (Anki's own Python library and Rust backend,
tools/requirements-anki.txt) can be imported, here or by VJO_ANKI_PYTHON, the
packages are imported into new collections the way Anki's import does it, and
the result is checked: the notes and the Lapis note type, the rendered card,
a second import that adds nothing, the cumulative package that adds only the
new notes, a profile whose own Lapis (newer, restyled) is kept, and the
screenshots in the media folder. VJO_REQUIRE_ANKI=1 makes a missing `anki` a
failure (CI sets it).
"""
import hashlib
import json
import os
import pathlib
import re
import sqlite3
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
LAPIS = ROOT / "third_party" / "lapis"
FIELDS = [
    "Expression", "ExpressionFurigana", "ExpressionReading", "ExpressionAudio", "SelectionText",
    "MainDefinition", "DefinitionPicture", "Sentence", "SentenceFurigana", "SentenceAudio", "Picture",
    "Glossary", "Hint", "IsWordAndSentenceCard", "IsClickCard", "IsSentenceCard", "IsAudioCard",
    "PitchPosition", "PitchCategories", "Frequency", "FreqSort", "MiscInfo",
]
BASE91 = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&()*+,-./:;<=>?@[]^_`{|}~"


def guid_of(queue_id):
    """genanki's guid_for: base91 of a 64-bit number, here the queue ID's first 64 bits."""
    v = int(queue_id[:16], 16)
    out = ""
    while True:
        out = BASE91[v % 91] + out
        v //= 91
        if not v:
            return out


def anki_checksum(text):
    """Anki's field_checksum: the first 32 bits of the SHA-1 of the field without HTML."""
    return int(hashlib.sha1(text.encode("utf-8")).hexdigest()[:8], 16)


def strip_html(html):
    text = re.sub(r"<[^>]*>", "", html)
    for a, b in (("&lt;", "<"), ("&gt;", ">"), ("&quot;", '"'), ("&amp;", "&")):
        text = text.replace(a, b)
    return text


class Package:
    def __init__(self, path):
        self.path = path
        self.zip = zipfile.ZipFile(path)
        self.dir = tempfile.TemporaryDirectory()
        db = pathlib.Path(self.dir.name) / "collection.anki2"
        db.write_bytes(self.zip.read("collection.anki2"))
        self.db = sqlite3.connect(db)
        self.media = json.loads(self.zip.read("media"))

    def notes(self):
        return self.db.execute("select id, guid, mid, mod, usn, tags, flds, sfld, csum, flags, data "
                               "from notes order by id").fetchall()

    def models(self):
        return json.loads(self.db.execute("select models from col").fetchone()[0])


class TestSqliteWriter(unittest.TestCase):
    """core/sqlw.c's files of every tree shape (vjo-apkg-tests --sqlw): one-row leaves on both sides
    of an interior page's 273 children and of two such pages, overflow chains from 4,062 bytes,
    many small rows. SQLite itself checks each file, and every row's bytes are compared."""

    def test_every_shape_is_a_valid_database(self):
        with tempfile.TemporaryDirectory() as out:
            subprocess.run([BINARY, "--sqlw", out], check=True, capture_output=True)
            files = sorted(pathlib.Path(out).glob("rows-*.sqlite"))
            self.assertEqual(len(files), 17)
            for path in files:
                rows, size = map(int, re.match(r"rows-(\d+)-(\d+)\.sqlite$", path.name).groups())
                with self.subTest(path.name):
                    conn = sqlite3.connect(path)
                    try:
                        self.assertEqual(conn.execute("pragma integrity_check").fetchall(), [("ok",)])
                        got = conn.execute("select id, v from t order by id").fetchall()
                    finally:
                        conn.close()
                    self.assertEqual([r for r, _ in got], list(range(1, rows + 1)))
                    for r, v in got:
                        self.assertEqual(v, "".join(chr(97 + (r * 7 + i) % 26) for i in range(size)), r)


class TestPackages(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.out = pathlib.Path(cls.tmp.name)
        subprocess.run([BINARY, "--write", str(cls.out)], check=True)
        cls.expected = json.loads((cls.out / "expected.json").read_text())
        cls.pkg = {name: Package(cls.out / f"{name}.apkg") for name in ("first", "second", "all", "many")}

    def test_zip(self):
        for name, p in self.pkg.items():
            names = p.zip.namelist()
            self.assertEqual(names[0], "collection.anki2", name)
            self.assertEqual(names[-1], "media", name)
            self.assertNotIn("meta", names)  # no meta: Anki reads it as Version::Legacy1
            self.assertIsNone(p.zip.testzip(), name)  # every CRC-32
            for info in p.zip.infolist():
                self.assertEqual(info.compress_type, zipfile.ZIP_STORED)
            self.assertEqual(sorted(p.media), [str(k) for k in range(len(p.media))])
            self.assertEqual(names[1:-1], [str(k) for k in range(len(p.media))])
            for key, file in p.media.items():
                data = p.zip.read(key)
                self.assertEqual(data[:3], b"\xff\xd8\xff")
                self.assertEqual(file, "vitajp_%s.jpg" % hashlib.sha256(data).hexdigest()[:32])

    def test_sqlite_schema_and_notetype(self):
        for name, p in self.pkg.items():
            self.assertEqual(p.db.execute("pragma integrity_check").fetchall(), [("ok",)], name)
            self.assertEqual(p.db.execute("pragma page_size").fetchone()[0], 4096)
            tables = [r[0] for r in p.db.execute("select name from sqlite_master where type='table'")]
            self.assertEqual(tables, ["col", "notes", "cards", "revlog", "graves"])
            ver, conf, decks_json, dconf, tags = p.db.execute(
                "select ver, conf, decks, dconf, tags from col").fetchone()
            self.assertEqual(ver, 11)
            conf = json.loads(conf)
            self.assertEqual(conf["curModel"], str(self.expected["notetype_id"]))
            self.assertEqual(conf["nextPos"], len(p.notes()) + 1)
            self.assertEqual(json.loads(dconf)["1"]["name"], "Default")
            self.assertEqual(json.loads(tags), {})
            models = p.models()
            self.assertEqual(list(models), [str(self.expected["notetype_id"])])
            lapis = models[str(self.expected["notetype_id"])]
            self.assertEqual(lapis["name"], "Lapis")
            self.assertEqual([f["name"] for f in lapis["flds"]], FIELDS)
            self.assertEqual([f["ord"] for f in lapis["flds"]], list(range(22)))
            self.assertEqual([t["name"] for t in lapis["tmpls"]], ["Mining"])
            # the templates and styling are Lapis 1.7.0's files (without their final newline, as Anki keeps them)
            self.assertEqual(lapis["tmpls"][0]["qfmt"], (LAPIS / "front.html").read_text().rstrip("\n"))
            self.assertEqual(lapis["tmpls"][0]["afmt"], (LAPIS / "back.html").read_text().rstrip("\n"))
            self.assertEqual(lapis["css"], (LAPIS / "styling.css").read_text().rstrip("\n"))
            decks = json.loads(p.db.execute("select decks from col").fetchone()[0])
            self.assertEqual(decks[str(self.expected["deck_id"])]["name"], self.expected["deck"])
            self.assertEqual(p.db.execute("select count(*) from revlog").fetchone()[0], 0)
            self.assertEqual(p.db.execute("select count(*) from graves").fetchone()[0], 0)

    def test_notes_and_cards(self):
        nt = self.expected["notetype_id"]
        for name, p in self.pkg.items():
            notes = p.notes()
            cards = p.db.execute("select id, nid, did, ord, type, queue, due from cards order by id").fetchall()
            self.assertEqual(len(cards), len(notes), name)
            self.assertEqual(len({n[1] for n in notes}), len(notes))  # GUIDs unique
            for (nid, guid, mid, mod, usn, tags, flds, sfld, csum, flags, data), card in zip(notes, cards):
                fields = flds.split("\x1f")
                self.assertEqual(len(fields), 22)
                self.assertEqual(mid, nt)
                self.assertEqual(tags, " vita-jp-overlay ")
                self.assertEqual(sfld, strip_html(fields[0]))
                self.assertEqual(csum, anki_checksum(strip_html(fields[0])))
                self.assertGreater(nid, 1_600_000_000_000)
                self.assertEqual(card[:6], (nid, nid, self.expected["deck_id"], 0, 0, 0))
                self.assertTrue(fields[7].count("<b>") <= 1)  # Sentence: the word in <b>
                self.assertTrue(fields[20].isdigit())  # FreqSort: a rank, or 9999999
                self.assertEqual(fields[3], "")  # no audio
                if fields[10]:
                    self.assertRegex(fields[10], r'^<img src="vitajp_[0-9a-f]{32}\.jpg">$')
                    self.assertIn(fields[10][10:-2], p.media.values())
            ids = [n[0] for n in notes]
            self.assertEqual(ids, sorted(ids))
            self.assertEqual([c[6] for c in cards], list(range(1, len(cards) + 1)))  # due: their order
        self.assertEqual(len(self.pkg["many"].notes()), self.expected["many"])

    def test_guids_come_from_the_queue_ids(self):
        first = {n[1] for n in self.pkg["first"].notes()}
        self.assertEqual(first, {guid_of(i) for i in self.expected["first"]})
        second = {n[1] for n in self.pkg["second"].notes()}
        self.assertEqual(second, {guid_of(i) for i in self.expected["second"]})

    def test_a_later_package_gives_the_same_notes(self):
        everything = {n[1]: n for n in self.pkg["all"].notes()}
        for name in ("first", "second"):
            for note in self.pkg[name].notes():
                same = everything[note[1]]
                self.assertEqual(same[1:], note[1:], "the same GUID, type, time, tags and fields")
        self.assertEqual(len(everything), len(self.expected["first"]) + len(self.expected["second"]))

    def test_fields_of_known_cards(self):
        by_word = {n[7]: n[6].split("\x1f") for n in self.pkg["first"].notes()}
        cat = by_word["猫"]
        self.assertEqual(cat[1], "猫[ねこ]")
        self.assertEqual(cat[2], "ねこ")
        self.assertIn('data-dictionary="JMdict"', cat[5])
        self.assertIn("<b>猫</b>が好き", cat[7] + cat[7].replace("<br>", ""))
        self.assertEqual(cat[19], '<ul style="text-align: left;"><li>Jiten: 1234</li></ul>')
        self.assertEqual(cat[20], "1234")
        self.assertEqual(cat[21], "SOME GAME (2026-10-11 17:11)")
        self.assertTrue(cat[10].startswith('<img src="vitajp_'))
        dog = by_word["犬"]
        self.assertEqual(dog[20], "9999999")  # no frequency: last when sorted, as Yomitan's cards
        self.assertIn('data-dictionary="Daijirin4"', dog[11])  # every dictionary's entry
        self.assertIn("「犬だ！」<br>と言った".replace("犬", "<b>犬</b>"), dog[7])
        room = by_word["部屋"]  # a v0.8.0-test.1 record (version 2)
        self.assertIn('data-dictionary="Daijirin4 Plain"', room[5])
        self.assertEqual(room[20], "9999999")


ANKI_CHECK = r'''
import json, os, shutil, sqlite3, sys, tempfile, pathlib, zipfile
from anki.collection import Collection, ImportAnkiPackageRequest, ImportAnkiPackageOptions
from anki.import_export_pb2 import ImportAnkiPackageUpdateCondition as Cond
out = pathlib.Path(sys.argv[1]); expected = json.loads((out / "expected.json").read_text())
def new_col():
    d = tempfile.mkdtemp()
    return Collection(os.path.join(d, "collection.anki2"))
def imp(col, path):
    # Anki's defaults: no merging, notes and note types updated only if newer
    opts = ImportAnkiPackageOptions(merge_notetypes=False, update_notes=Cond.IMPORT_ANKI_PACKAGE_UPDATE_CONDITION_IF_NEWER,
                                    update_notetypes=Cond.IMPORT_ANKI_PACKAGE_UPDATE_CONDITION_IF_NEWER,
                                    with_scheduling=False, with_deck_configs=False)
    path = path if os.path.isabs(str(path)) else out / f"{path}.apkg"
    return col.import_anki_package(ImportAnkiPackageRequest(package_path=str(path), options=opts)).log
r = {}
col = new_col()
log = imp(col, "first")
r["first_new"] = len(log.new); r["first_found"] = log.found_notes
lapis = [m for m in col.models.all() if m["name"] == "Lapis"]
r["lapis"] = [(m["id"], [f["name"] for f in m["flds"]]) for m in lapis]
r["notes"] = col.note_count(); r["cards"] = col.card_count()
r["deck"] = col.decks.by_name(expected["deck"]) is not None
r["deck_cards"] = len(col.find_cards(f'"deck:{expected["deck"]}"'))
card = col.get_card(col.find_cards('"Expression:猫" "MiscInfo:_*"')[0])   # the version-3 card
r["question"] = card.question(); r["answer"] = card.answer()
r["media"] = sorted(os.listdir(col.media.dir()))
r["again_new"] = len(imp(col, "first").new)   # the same package again
# the user edits a note and its card is in review (10 days); importing the same card again changes neither
edited = col.get_note(col.find_notes('"Expression:猫" "MiscInfo:_*"')[0])
edited["MainDefinition"] = "my own words"; col.update_note(edited)
c = edited.cards()[0]; c.type = c.queue = 2; c.ivl = 10; c.due = 100; c.factor = 2500; col.update_card(c)
log = imp(col, "all")                            # everything: only the second's notes are new
r["edit_kept"] = col.get_note(edited.id)["MainDefinition"] == "my own words"
c = col.get_card(c.id); r["review_kept"] = [c.type, c.queue, c.ivl, c.due, len(col.get_note(edited.id).cards())]
r["all_new"] = len(log.new); r["all_dup"] = len(log.duplicate); r["all_updated"] = len(log.updated)
r["notes_after_all"] = col.note_count()
r["notetypes"] = sorted(m["name"] for m in col.models.all())
r["media_after_all"] = sorted(f for f in os.listdir(col.media.dir()) if f.startswith("vitajp_"))
log = imp(col, "many"); r["many_new"] = len(log.new)
col.close()
# A profile that has Lapis from its own package, restyled since (newer): a
# package of that note type alone (first.apkg's, without notes, its styling
# and time changed) is imported first, then ours.
tmp = pathlib.Path(tempfile.mkdtemp())
with zipfile.ZipFile(out / "first.apkg") as z:
    (tmp / "collection.anki2").write_bytes(z.read("collection.anki2"))
db = sqlite3.connect(tmp / "collection.anki2")
models = json.loads(db.execute("select models from col").fetchone()[0])
for m in models.values():
    m["css"] = "/* mine */"; m["mod"] = 2000000000
db.execute("update col set models = ?", (json.dumps(models),))
# one note of its own (a note type comes with the notes that use it)
keep = db.execute("select min(id) from notes").fetchone()[0]
db.execute("delete from notes where id != ?", (keep,)); db.execute("delete from cards where nid != ?", (keep,))
db.execute("update notes set guid = 'mine-1', mod = 2000000000, flds = 'mine' || substr(flds, instr(flds, char(31)))")
db.commit(); db.close()
with zipfile.ZipFile(tmp / "mine.apkg", "w") as z:
    z.write(tmp / "collection.anki2", "collection.anki2"); z.writestr("media", "{}")
col = new_col()
imp(col, str(tmp / "mine.apkg"))
imp(col, "first")
lapis = [m for m in col.models.all() if m["name"].startswith("Lapis")]
r["own_after"] = [(x["id"] == expected["notetype_id"], x["css"] == "/* mine */", x["name"]) for x in lapis]
r["own_notes"] = col.note_count() - 1
col.close()
print(json.dumps(r, ensure_ascii=False))
'''


def anki_python():
    try:
        import anki  # noqa: F401
        return sys.executable
    except ImportError:
        return os.environ.get("VJO_ANKI_PYTHON")


@unittest.skipUnless(anki_python() or os.environ.get("VJO_REQUIRE_ANKI") == "1",
                     "the anki package is not installed (tools/requirements-anki.txt)")
class TestAnkiImport(unittest.TestCase):
    """The packages imported by Anki's own backend (the `anki` package)."""

    @classmethod
    def setUpClass(cls):
        python = anki_python()
        if not python:
            raise AssertionError("VJO_REQUIRE_ANKI=1, but the anki package cannot be imported")
        cls.tmp = tempfile.TemporaryDirectory()
        out = pathlib.Path(cls.tmp.name)
        subprocess.run([BINARY, "--write", str(out)], check=True)
        cls.expected = json.loads((out / "expected.json").read_text())
        done = subprocess.run([python, "-c", ANKI_CHECK, str(out), json.dumps(FIELDS)], capture_output=True,
                              text=True)
        if done.returncode:
            raise AssertionError(f"the import failed:\n{done.stderr}")
        cls.r = json.loads(done.stdout.strip().splitlines()[-1])
        version = subprocess.run([python, "-c", "import anki.buildinfo as b; print(b.version)"],
                                 capture_output=True, text=True).stdout.strip()
        print(f"anki {version}: {cls.r['first_new']} + {cls.r['all_new']} + {cls.r['many_new']} notes imported")

    def test_first_package(self):
        r = self.r
        n = len(self.expected["first"])
        self.assertEqual((r["first_found"], r["first_new"], r["notes"], r["cards"], r["deck_cards"]), (n,) * 5)
        self.assertEqual(r["lapis"], [[self.expected["notetype_id"], FIELDS]])
        self.assertTrue(r["deck"])
        self.assertEqual(len([f for f in r["media"] if f.startswith("vitajp_")]), 1)
        self.assertEqual(r["media_after_all"], [f for f in r["media"] if f.startswith("vitajp_")])

    def test_rendered_card(self):
        self.assertIn("猫", self.r["question"])
        for text in ("<b>猫</b>", "JMdict", "SOME GAME (2026-10-11 17:11)", "vitajp_", "1234"):
            self.assertIn(text, self.r["answer"])

    def test_imports_again_add_nothing_and_all_adds_only_new_notes(self):
        r = self.r
        self.assertEqual(r["again_new"], 0)
        self.assertEqual(r["all_new"], len(self.expected["second"]))
        self.assertEqual(r["all_dup"], len(self.expected["first"]))
        self.assertEqual(r["all_updated"], 0)
        self.assertEqual(r["notes_after_all"], len(self.expected["first"]) + len(self.expected["second"]))
        self.assertEqual(r["notetypes"].count("Lapis"), 1)
        self.assertEqual(r["many_new"], self.expected["many"])

    def test_an_edited_and_reviewed_note_is_left_alone(self):
        # (the package's note is never newer than one the user edited: Anki's IfNewer keeps the edit)
        self.assertTrue(self.r["edit_kept"])
        self.assertEqual(self.r["review_kept"], [2, 2, 10, 100, 1])  # still in review, one card

    def test_a_profiles_own_lapis_is_kept(self):
        r = self.r
        self.assertEqual(r["own_after"], [[True, True, "Lapis"]])
        self.assertEqual(r["own_notes"], len(self.expected["first"]))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    BINARY = sys.argv.pop(1)
    unittest.main(verbosity=2)
