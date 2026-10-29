#!/usr/bin/env python3
"""The Rubi app's .apkg packages, opened as Anki would open them.

    python3 host/tests/test_apkg.py BUILD/vjo-apkg-tests

The C test binary writes the packages (`--write DIR`: first, second, all,
many, app, richer and order, from records the overlay's Enter writes, the Rubi
app's Look up page saves and earlier builds'), and
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

from testlib import guid_of

ROOT = pathlib.Path(__file__).resolve().parents[2]
LAPIS = ROOT / "third_party" / "lapis"
FIELDS = [
    "Expression", "ExpressionFurigana", "ExpressionReading", "ExpressionAudio", "SelectionText",
    "MainDefinition", "DefinitionPicture", "Sentence", "SentenceFurigana", "SentenceAudio", "Picture",
    "Glossary", "Hint", "IsWordAndSentenceCard", "IsClickCard", "IsSentenceCard", "IsAudioCard",
    "PitchPosition", "PitchCategories", "Frequency", "FreqSort", "MiscInfo",
]


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


def fold(text):
    """core/word_key.h's fold: katakana as hiragana, the middle dot left out."""
    out = []
    for ch in text:
        cp = ord(ch)
        if cp in (0x30FB, 0xFF65):
            continue
        if 0x30A1 <= cp <= 0x30F6 or cp in (0x30FD, 0x30FE):
            cp -= 0x60
        out.append(chr(cp))
    return "".join(out)


def word_key(spelling, reading=None):
    """core/word_key.c's keys: the word's (spelling TAB reading), or, without a reading, the spelling's alone."""
    text = fold(spelling) + ("\t" + (fold(reading) or fold(spelling)) if reading is not None else "")
    return hashlib.sha256(text.encode("utf-8")).hexdigest()[:16]


def known_keys(path):
    """The app's reading of a known_words.txt (vjo-apkg-tests --known-keys): [(line, rc, key)]."""
    out = subprocess.run([BINARY, "--known-keys", str(path)], check=True, capture_output=True, text=True).stdout
    return [(int(a), int(b), c[0] if c else "") for a, b, *c in (line.split(" ") for line in out.splitlines())]


class TestKnownWords(unittest.TestCase):
    """#314 stage C: tools/known_words.py's file, read as the Rubi app reads it (settings/anki_export.c
    vjo_known_line): every line of it a word, with the key the tool's word and reading have."""

    def test_the_tools_file_is_the_apps(self):
        tool = ROOT / "tools" / "known_words.py"
        fixture = ROOT / "tools" / "tests" / "fixtures" / "known-words" / "anki-plain-notetype.txt"
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "known_words.txt"
            subprocess.run([sys.executable, str(tool), str(fixture), "-o", str(out)], check=True, capture_output=True)
            lines = out.read_text(encoding="utf-8").split("\n")[:-1]
            got = known_keys(out)
        self.assertEqual(got[0][1], 0)  # the tool's comment line
        self.assertEqual(len(got), len(lines))
        for (number, rc, key), line in zip(got[1:], lines[1:]):
            word, _, reading = line.partition("\t")
            self.assertEqual((rc, key), (1, word_key(word, reading or None)), (number, line))
        self.assertEqual(len(got), 7)  # the comment and six words

    def test_the_apps_reading_of_odd_lines(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "known_words.txt"
            path.write_bytes("\ufeff# mine\r\n猫\tネコ\r\n\r\n  犬  \r\n\tのみ\r\n本\tほん\textra".encode("utf-8"))
            got = known_keys(path)
        self.assertEqual(got, [(1, 0, ""), (2, 1, word_key("猫", "ねこ")), (3, 0, ""), (4, 1, word_key("犬")),
                               (5, -1, ""), (6, 1, word_key("本", "ほん"))])


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
        cls.pkg = {name: Package(cls.out / f"{name}.apkg")
                   for name in ("first", "second", "all", "many", "app", "richer", "order")}

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
            # Kept through the Rubi rename (docs/rename-rubi.md): Anki finds a deck by its name, so another name
            # would put new cards in a second deck beside the one the owner's collection already has.
            self.assertEqual((self.expected["deck"], self.expected["deck_id"]), ("Vita JP Overlay", 1760534400000))
            self.assertEqual(p.db.execute("select count(*) from revlog").fetchone()[0], 0)
            self.assertEqual(p.db.execute("select count(*) from graves").fetchone()[0], 0)

    def test_notes_and_cards(self):
        nt = self.expected["notetype_id"]
        duplicates = {guid_of(i) for i in self.expected["duplicates"]}
        for name, p in self.pkg.items():
            notes = p.notes()
            game = self.game_tags(name)
            cards = p.db.execute("select id, nid, did, ord, type, queue, due from cards order by id").fetchall()
            self.assertEqual(len(cards), len(notes), name)
            self.assertEqual(len({n[1] for n in notes}), len(notes))  # GUIDs unique
            for (nid, guid, mid, mod, usn, tags, flds, sfld, csum, flags, data), card in zip(notes, cards):
                fields = flds.split("\x1f")
                self.assertEqual(len(fields), 22)
                self.assertEqual(mid, nt)
                # every tag between spaces: #314's duplicate tag on a card whose word an earlier card has, then #311's
                # game tag on a record with a game, in GameSentenceMiner's form
                want = (" vita-jp-overlay " + ("duplicate " if guid in duplicates else "") +
                        (game[guid] + " " if guid in game else ""))
                self.assertEqual(tags, want, (name, guid))
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

    def test_notes_are_in_their_times_order(self):
        """The package's order is its cards' times, then their IDs (core/apkg.c vjo_apkg_sort), and a note's ID is its
        time in milliseconds, or one more than the note before for cards of the same second (vjo_apkg_note_id), in
        every package. order.apkg (#528's audit 3, MT20: no time sort after Export all's one entry per card): three
        cards whose IDs are not in their times' order, the first archived too, each once, in their times' order."""
        for name, p in self.pkg.items():
            notes = p.notes()  # by ID
            mods = [n[3] for n in notes]
            self.assertEqual(mods, sorted(mods), name)
            previous = 0
            for nid, guid, _mid, mod, *_ in notes:
                self.assertEqual(nid, max(mod * 1000, previous + 1), (name, guid))
                previous = nid
        order = self.pkg["order"].notes()
        self.assertEqual([n[1] for n in order], [guid_of(i) for i in self.expected["order"]])
        self.assertEqual([n[0] for n in order], [n[3] * 1000 for n in order])
        self.assertEqual([n[3] - order[0][3] for n in order], [0, 60, 120])
        self.assertNotEqual(sorted(self.expected["order"]), self.expected["order"])  # IDs not in their times' order

    def test_duplicates_are_tagged(self):
        """#314: Duplicates (Tag, the default): the version 3 猫 after the version 1 one (no time: earlier), and 犬
        from another line in the second package after the first's; in the cumulative package, the same two."""
        def tagged(p):
            return {n[1] for n in p.notes() if "duplicate" in n[5].split()}
        dup = [guid_of(i) for i in self.expected["duplicates"]]
        self.assertEqual(tagged(self.pkg["first"]), {dup[0]})
        self.assertEqual(tagged(self.pkg["second"]), {dup[1]})
        self.assertEqual(tagged(self.pkg["all"]), set(dup))
        self.assertEqual(tagged(self.pkg["many"]), set())

    def game_tags(self, name):
        """#311: each note's game tag (GUID: tag), in GameSentenceMiner's form: the version 3 records' SOME GAME, none
        for the version 1 and 2 records; of the app's cards (#313) the earlier line's CLANNAD, none for a typed word."""
        if name == "app":
            return {guid_of(self.expected["app"][1]): "Game::CLANNAD"}
        old = {guid_of(i) for i in self.expected["first"][2:]}
        return {n[1]: "Game::SOMEGAME" for n in self.pkg[name].notes() if n[1] not in old}

    def test_richer_notes(self):
        """#311: a Sentence card (IsSentenceCard x), its game's tag, and with the bundled dictionary readings over
        its sentence in Anki's furigana syntax, the mined word in <b> with its own reading."""
        (note,) = self.pkg["richer"].notes()
        fields = note[6].split("\x1f")
        self.assertEqual(note[1], guid_of(self.expected["richer"]))
        self.assertEqual(note[5], " vita-jp-overlay Game::SOMEGAME ")
        self.assertEqual([fields[i] for i in (13, 14, 15, 16)], ["", "", "x", ""])
        self.assertEqual(fields[7], "今日は<b>学校</b>へ行って、友達と一緒に勉強した。")
        if self.expected["readings"]:
            self.assertEqual(fields[8], "今日は<b>学校[がっこう]</b>へ 行[い]って、 友達[ともだち]と 一緒[いっしょ]に"
                                        " 勉強[べんきょう]した。")
        else:
            self.assertEqual(fields[8], "")

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

    def test_cards_the_app_saved(self):
        """#313: cards of the Rubi app's Look up page (settings/app_queue.c), as any card: their GUIDs from their
        records' IDs, no picture; a typed word with no sentence and the date it was saved, a word of an earlier line
        with the line and its game; the other dictionary's entry in the glossary."""
        notes = self.pkg["app"].notes()
        self.assertEqual({n[1] for n in notes}, {guid_of(i) for i in self.expected["app"]})
        self.assertEqual(self.pkg["app"].media, {})
        by_misc = {n[6].split("\x1f")[21]: n[6].split("\x1f") for n in notes}
        typed, line = by_misc["2026-10-27 15:04"], by_misc["CLANNAD (2026-10-28 00:30)"]
        for fields, word in ((typed, ("猫", "猫[ねこ]", "ねこ")), (line, ("犬", "犬[いぬ]", "いぬ"))):
            self.assertEqual((fields[0], fields[1], fields[2], fields[10]), word + ("",))
            self.assertIn('data-dictionary="JMdict"', fields[5])
            self.assertIn('data-dictionary="VNDB names"', fields[11])
            self.assertEqual(fields[20], "1234")
        self.assertEqual((typed[7], typed[8]), ("", ""))
        self.assertEqual(line[7], "<b>犬</b>が鳴いた")

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
    d = tempfile.mkdtemp(dir=out)  # in the packages' directory, which the test removes
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
# API 23: the card's screenshot as Anki shows it: the answer's <img>, the file it names in the
# collection's media (copied out for Pillow), and Anki's own media check
import re
m = re.search(r'<img src="(vitajp_[0-9a-f]{32}[.]jpg)">', r["answer"])
r["img"] = m.group(1) if m else None
if m:
    shutil.copyfile(os.path.join(col.media.dir(), m.group(1)), out / "imported.jpg")
check = col.media.check()
r["media_missing"] = list(check.missing); r["media_unused"] = list(check.unused)
# #314: Anki's own tags of the notes: the duplicate one has Anki's duplicate tag beside vita-jp-overlay
r["dup_tags"] = [sorted(col.get_note(n).tags) for n in col.find_notes("tag:duplicate")]
r["plain_tags"] = sorted({tuple(sorted(col.get_note(n).tags)) for n in col.find_notes("-tag:duplicate")})
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
# #311: the sentence card: its front the sentence, its back the readings (Anki's furigana filter), its tags
log = imp(col, "richer"); r["richer_new"] = len(log.new)
card = col.get_card(col.find_cards('"Expression:学校"')[0])
r["richer_question"] = card.question(); r["richer_answer"] = card.answer()
r["richer_tags"] = sorted(card.note().tags)
r["game_tag_notes"] = len(col.find_notes('"tag:Game::SOMEGAME"'))
col.close()
# #313: the Rubi app's cards, in a new profile: both added, no picture on either
col = new_col()
log = imp(col, "app"); r["app_new"] = len(log.new)
answers = [col.get_card(c).answer() for c in col.find_cards('"deck:Vita JP Overlay"')]
r["app_answers"] = len(answers); r["app_img"] = sum("<img" in a for a in answers)
r["app_line"] = sum("<b>犬</b>が鳴いた" in a and "CLANNAD (2026-10-28 00:30)" in a for a in answers)
col.close()
# A profile that has Lapis from its own package, restyled since (newer): a
# package of that note type alone (first.apkg's, without notes, its styling
# and time changed) is imported first, then ours.
tmp = pathlib.Path(tempfile.mkdtemp(dir=out))
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
# #314 stage C: a collection of the first package alone, closed, for tools/known_words.py, and Anki's own
# "Notes in Plain Text" export of it
col = new_col()
imp(col, "first")
col.export_note_csv(out_path=str(out / "notes.txt"), limit=None, with_html=True, with_tags=True, with_deck=True,
                    with_notetype=True, with_guid=True)
r["collection_path"] = col.path
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

    def test_the_duplicate_tag_in_anki(self):
        """#314: Anki's import keeps the duplicate note's tags, and only vita-jp-overlay on the others, beside #311's
        game tag on the version 3 records' notes (the version 1 and 2 records name no game)."""
        self.assertEqual(self.r["dup_tags"], [["Game::SOMEGAME", "duplicate", "vita-jp-overlay"]])
        self.assertEqual(self.r["plain_tags"], [["Game::SOMEGAME", "vita-jp-overlay"], ["vita-jp-overlay"]])

    def test_known_words_from_ankis_own_collection(self):
        """#314 stage C: tools/known_words.py on the collection Anki 26.09.3 made from the first package (closed
        first, as the tool asks), and on Anki's own "Notes in Plain Text" export of it: the same words."""
        tool = ROOT / "tools" / "known_words.py"
        with tempfile.TemporaryDirectory() as tmp:
            outs = []
            for source in (self.r["collection_path"], pathlib.Path(self.tmp.name) / "notes.txt"):
                out = pathlib.Path(tmp) / f"known-{len(outs)}.txt"
                subprocess.run([sys.executable, str(tool), str(source), "-o", str(out)], check=True,
                               capture_output=True)
                outs.append(out.read_text(encoding="utf-8").split("\n")[1:-1])
        self.assertEqual(outs[0], outs[1])
        self.assertEqual(sorted(outs[0]), sorted(["猫\tねこ", "犬\tいぬ", "部屋\tへや"]))

    def test_a_sentence_card_with_readings_and_its_game_tag(self):
        """#311: Lapis's Sentence card shows the sentence on its front (its {{kanji:Sentence}}); the back renders
        SentenceFurigana's readings as <ruby> (with the bundled dictionary); the note has its game's tag."""
        r = self.r
        self.assertEqual(r["richer_new"], 1)
        self.assertIn("front-sentence", r["richer_question"])
        self.assertIn("<b>学校</b>へ行って", r["richer_question"])
        self.assertNotIn('class="front-vocab"', r["richer_question"])
        if self.expected["readings"]:
            for ruby in ("<ruby><rb>学校</rb><rt>がっこう</rt></ruby>", "<ruby><rb>友達</rb><rt>ともだち</rt></ruby>"):
                self.assertIn(ruby, r["richer_answer"])
        self.assertEqual(r["richer_tags"], ["Game::SOMEGAME", "vita-jp-overlay"])
        self.assertGreater(r["game_tag_notes"], 700)  # every version-3 card's

    def test_rendered_card(self):
        self.assertIn("猫", self.r["question"])
        for text in ("<b>猫</b>", "JMdict", "SOME GAME (2026-10-11 17:11)", "vitajp_", "1234"):
            self.assertIn(text, self.r["answer"])

    def test_the_screenshot_is_shown_on_the_card(self):
        """API 23: the card's answer shows its screenshot: an <img> of a file Anki has in its media,
        byte for byte the JPEG the game worker's encoder made (named by its SHA-256), a picture
        Pillow decodes; Anki's media check finds nothing missing."""
        from PIL import Image
        r = self.r
        self.assertRegex(r["img"] or "", r"^vitajp_[0-9a-f]{32}[.]jpg$")
        self.assertIn(f'<img src="{r["img"]}">', r["answer"])
        self.assertIn(r["img"], r["media"])
        self.assertEqual(r["media_missing"], [])
        data = (pathlib.Path(self.tmp.name) / "imported.jpg").read_bytes()
        self.assertEqual("vitajp_%s.jpg" % hashlib.sha256(data).hexdigest()[:32], r["img"])
        with Image.open(pathlib.Path(self.tmp.name) / "imported.jpg") as im:
            im.load()
            self.assertEqual((im.format, im.size), ("JPEG", (960, 544)))

    def test_imports_again_add_nothing_and_all_adds_only_new_notes(self):
        r = self.r
        self.assertEqual(r["again_new"], 0)
        self.assertEqual(r["all_new"], len(self.expected["second"]))
        self.assertEqual(r["all_dup"], len(self.expected["first"]))
        self.assertEqual(r["all_updated"], 0)
        self.assertEqual(r["notes_after_all"], len(self.expected["first"]) + len(self.expected["second"]))
        self.assertEqual(r["notetypes"].count("Lapis"), 1)
        self.assertEqual(r["many_new"], self.expected["many"])

    def test_the_apps_cards_import(self):
        """#313: the cards the Look up page saved are imported as Anki imports any: two notes, two cards, no
        picture; the earlier line's card shows the line and its game."""
        r = self.r
        self.assertEqual((r["app_new"], r["app_answers"], r["app_img"], r["app_line"]), (2, 2, 0, 1))

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
