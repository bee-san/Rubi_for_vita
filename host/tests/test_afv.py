#!/usr/bin/env python3
"""The Rubi app's notes for Anki for Vita, against the .apkg of the same cards.

    python3 host/tests/test_afv.py BUILD/vjo-afv-tests

The C binary (`--write DIR`) queues six cards as the overlay's Enter saves them (two with the same
screenshot, two without one, a second dictionary's entry, HTML to escape, Anki's field separator in
a definition), hands them to Anki for Vita (settings/anki_afv.c, into
DIR/ux0:data/Anki_for_vita/inbox/rubi/) and exports all of them as an .apkg (settings/anki_export.c).
This checks, with Python's zipfile, sqlite3 and json only:

- every inbox file against the file interface's rules, as bee-san/Anki_for_vita's docs/interface.md
  (draft version 1) states them for the app's reader: the encoding, the keys and their limits, the
  id, the GUID, the media names and files, the note type's file;
- every inbox note against the package's note of the same card: the GUID, each of the 22 Lapis
  fields (a field the note leaves out is empty in the package), the tags, the deck, the creation time
  (the package note's modification time), the screenshot's bytes, and the note type file against the
  package's note type.
"""
import json
import pathlib
import re
import sqlite3
import subprocess
import sys
import tempfile
import unittest
import zipfile

FIELDS = [
    "Expression", "ExpressionFurigana", "ExpressionReading", "ExpressionAudio", "SelectionText",
    "MainDefinition", "DefinitionPicture", "Sentence", "SentenceFurigana", "SentenceAudio", "Picture",
    "Glossary", "Hint", "IsWordAndSentenceCard", "IsClickCard", "IsSentenceCard", "IsAudioCard",
    "PitchPosition", "PitchCategories", "Frequency", "FreqSort", "MiscInfo",
]
ID = re.compile(r"^[a-z0-9_-]{1,64}$")
MEDIA = re.compile(r"^[a-z0-9._-]{1,120}$")
DEVICE = re.compile(r"^(con|prn|aux|nul|com[1-9]|lpt[1-9])(\.|$)")
CREATED = (1136073600, 4102444800)


def media_name_ok(name):
    """docs/interface.md's rule for a media name (and a note type file's)."""
    return bool(MEDIA.match(name)) and not name.startswith(".") and not name.endswith(".") \
        and not DEVICE.match(name) and name != "thumbs.db"


def strict_json(data, limit):
    """A file of the interface: valid UTF-8, no NUL, no key twice, at most limit bytes."""
    assert len(data) <= limit, f"{len(data)} bytes, over {limit}"
    text = data.decode("utf-8")  # strict: invalid UTF-8 raises
    assert "\0" not in text

    def no_duplicates(pairs):
        keys = [k for k, _ in pairs]
        assert len(keys) == len(set(keys)), f"a key twice: {keys}"
        return dict(pairs)

    value = json.loads(text, object_pairs_hook=no_duplicates)
    assert "\0" not in json.dumps(value, ensure_ascii=False)  # (an escaped NUL too)
    return value


class TestInbox(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = pathlib.Path(cls.tmp.name)
        subprocess.run([BINARY, "--write", str(cls.dir)], check=True, capture_output=True, timeout=120)
        cls.expected = json.loads((cls.dir / "expected.json").read_text())
        cls.inbox = cls.dir / cls.expected["inbox"]
        package = zipfile.ZipFile(cls.dir / cls.expected["package"])
        db = cls.dir / "collection.anki2"
        db.write_bytes(package.read("collection.anki2"))
        cls.db = sqlite3.connect(db)
        cls.package = package
        cls.media_map = json.loads(package.read("media"))
        cls.notes = {}
        for path in sorted(cls.inbox.glob("*.json")):
            cls.notes[path.stem] = strict_json(path.read_bytes(), 65536)

    @classmethod
    def tearDownClass(cls):
        cls.db.close()
        cls.tmp.cleanup()

    def test_every_card_has_its_note(self):
        self.assertEqual(sorted(self.notes), sorted(self.expected["ids"]))
        self.assertEqual(list(self.inbox.glob("*.tmp")), [])
        self.assertEqual(list((self.inbox / "media").glob("*.tmp")), [])

    def test_the_notes_follow_the_interface(self):
        for name, n in self.notes.items():
            with self.subTest(id=name):
                self.assertIs(type(n["v"]), int)
                self.assertEqual(n["v"], 1)
                self.assertEqual(n["id"], name)
                self.assertRegex(n["id"], ID)
                self.assertTrue(1 <= len(n["guid"]) <= 32 and all(0x21 <= ord(c) <= 0x7E for c in n["guid"]))
                nt = n["notetype"]
                self.assertEqual((nt["name"], nt["id"], nt["definition"]), ("Lapis", 1667218449922, "lapis-1.7.0.json"))
                self.assertTrue(media_name_ok(nt["definition"]) and nt["definition"].endswith(".json"))
                self.assertTrue((self.inbox / "notetypes" / nt["definition"]).is_file())
                self.assertEqual(n["deck"], "Vita JP Overlay")
                self.assertLessEqual(len(n["fields"]), 64)
                self.assertTrue(all(isinstance(k, str) and isinstance(v, str) for k, v in n["fields"].items()))
                self.assertTrue(set(n["fields"]) <= set(FIELDS), set(n["fields"]) - set(FIELDS))
                self.assertLessEqual(len(n["tags"]), 32)
                for tag in n["tags"]:
                    self.assertTrue(1 <= len(tag.encode()) <= 100)
                    self.assertFalse(re.search(r"[\s\u3000\x00-\x1f\x7f]", tag), tag)
                self.assertLessEqual(len(n.get("media", [])), 32)
                for m in n.get("media", []):
                    self.assertTrue(media_name_ok(m), m)
                    self.assertTrue((self.inbox / "media" / m).is_file(), m)
                    self.assertTrue(any(m in v for v in n["fields"].values()), f"{m} is used by no field")
                self.assertIs(type(n["created"]), int)
                self.assertTrue(CREATED[0] <= n["created"] <= CREATED[1])
                dup = n["duplicate_check"]["fields"]
                self.assertTrue(1 <= len(dup) <= 4 and all(f in n["fields"] for f in dup), dup)
                self.assertEqual(dup, ["Expression", "ExpressionReading"])

    def test_the_note_type_file_is_one_anki_takes(self):
        data = (self.inbox / "notetypes" / "lapis-1.7.0.json").read_bytes()
        nt = strict_json(data, 262144)
        self.assertEqual(nt["type"], 0)  # standard, not cloze
        names = [f["name"] for f in nt["flds"]]
        self.assertTrue(1 <= len(names) <= 64 and len(set(names)) == len(names) and all(names))
        self.assertEqual(names, FIELDS)
        self.assertTrue(1 <= len(nt["tmpls"]) <= 32)
        self.assertEqual((nt["id"], nt["name"]), (1667218449922, "Lapis"))

    def test_each_note_is_the_package_s(self):
        rows = self.db.execute("select guid, mid, mod, tags, flds, id from notes").fetchall()
        by_guid = {r[0]: r for r in rows}
        self.assertEqual(len(by_guid), len(rows))
        models = json.loads(self.db.execute("select models from col").fetchone()[0])
        decks = json.loads(self.db.execute("select decks from col").fetchone()[0])
        nt_file = json.loads((self.inbox / "notetypes" / "lapis-1.7.0.json").read_text())
        names_in_package = set(self.media_map.values())
        for name, n in self.notes.items():
            with self.subTest(id=name):
                self.assertIn(n["guid"], by_guid, "no note of the package has its GUID")
                guid, mid, mod, tags, flds, nid = by_guid[n["guid"]]
                self.assertEqual(mid, n["notetype"]["id"])
                self.assertEqual(models[str(mid)], nt_file)  # the package's note type is the file's
                values = flds.split("\x1f")
                self.assertEqual(len(values), len(FIELDS))
                for i, field in enumerate(FIELDS):
                    self.assertEqual(n["fields"].get(field, ""), values[i], field)
                for field in ("Expression", "ExpressionReading"):
                    self.assertIn(field, n["fields"])  # (given even when empty: duplicate_check names them)
                for field, value in n["fields"].items():
                    if field not in ("Expression", "ExpressionReading"):
                        self.assertNotEqual(value, "", f"{field}: an empty field given")
                self.assertEqual(tags.split(), n["tags"])
                self.assertEqual(mod, n["created"])
                did = self.db.execute("select did from cards where nid = ?", (nid,)).fetchone()[0]
                self.assertEqual(decks[str(did)]["name"], n["deck"])
                for m in n.get("media", []):
                    self.assertIn(m, names_in_package)
                    key = next(k for k, v in self.media_map.items() if v == m)
                    self.assertEqual((self.inbox / "media" / m).read_bytes(), self.package.read(key))
                    self.assertIn(f'<img src="{m}">', n["fields"]["Picture"])
                if not n.get("media"):
                    self.assertNotIn("Picture", n["fields"])

    def test_one_screenshot_is_one_file(self):
        shared = [n["media"][0] for n in self.notes.values() if n.get("media")]
        self.assertEqual(len(shared), 4)
        self.assertEqual(len(set(shared)), 3)  # two cards with the same screenshot
        self.assertEqual(sorted(p.name for p in (self.inbox / "media").iterdir()), sorted(set(shared)))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    BINARY = sys.argv.pop(1)
    unittest.main()
