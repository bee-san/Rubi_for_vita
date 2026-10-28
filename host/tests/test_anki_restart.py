"""Restart/crash tests of the offline Anki queue: every Vita-side action runs
in a fresh OS process.

The overlay's saves (the real Shell Anki thread and queue storage,
vjo-shell-anki-tests --restart-step) stop at each storage boundary of a
save; the Rubi app's exports (settings/anki_export.c and its package
writer, vjo-apkg-tests --restart-step) stop at each boundary of writing,
verifying and clearing; the Rubi app's Cards page (#315, settings/cards.c,
vjo-apkg-tests --restart-step fix|drop|delete) stops at each step of a Fix
sentence, a Drop picture and a Delete. After every stop a fresh process finds
every card either still queued or in a complete package (and archived), never
lost, never half-written, and a later export takes the rest. These check process
crashes and disk recovery, not electrical power loss on physical
Vita/SD2Vita hardware.

The Rubi app's own saves (a card mined on the Look up page, #313: settings/app_queue.c,
vjo-app-queue-tests --restart-step) stop at the same boundaries as the overlay's.

    python3 host/tests/test_anki_restart.py BUILD/vjo-shell-anki-tests BUILD/vjo-apkg-tests BUILD/vjo-app-queue-tests
"""
import json
import os
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile
from pathlib import Path

from testlib import guid_of

SHELL = str(Path(sys.argv.pop(1)).resolve())
APP = str(Path(sys.argv.pop(1)).resolve())
APP_QUEUE = str(Path(sys.argv.pop(1)).resolve())


class Storage(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="vjo-restart-")
        self.use(Path(self.temp.name))

    def tearDown(self):
        self.temp.cleanup()

    def use(self, root):
        self.root = root
        self.root.mkdir(exist_ok=True)
        self.data = self.root / "ux0:data" / "VitaJPOverlay"
        self.queue = self.data / "anki_queue"
        self.archive = self.queue / "exported"
        self.exports = self.data / "anki"

    def run_step(self, binary, action, crash=""):
        result = subprocess.run([binary, "--restart-step", str(self.root), action, crash], text=True,
                                capture_output=True, timeout=120)
        self.assertEqual(result.returncode, 86 if crash else 0, result.stdout + result.stderr)
        return None if crash else json.loads(result.stdout)

    def snapshot(self, directory):
        return {p.name: p.read_bytes() for p in directory.iterdir() if p.is_file()} if directory.exists() else {}

    def records(self, directory):
        return sorted(p.name[:32] for p in directory.iterdir()
                      if p.is_file() and len(p.name) == 37 and p.name.endswith(".json")) if directory.exists() else []

    def package_guids(self):
        """The GUIDs in every complete package (a .tmp one never counts)."""
        guids = []
        for package in sorted(self.exports.glob("*.apkg")) if self.exports.exists() else []:
            with zipfile.ZipFile(package) as z:
                self.assertIsNone(z.testzip(), package.name)
                db = self.root / "check.anki2"
                db.write_bytes(z.read("collection.anki2"))
            conn = sqlite3.connect(db)
            self.assertEqual(conn.execute("pragma integrity_check").fetchall(), [("ok",)])
            guids += [g for (g,) in conn.execute("select guid from notes")]
            conn.close()
            db.unlink()
        return guids

    def check_nothing_lost(self, ids):
        queued, archived = set(self.records(self.queue)), set(self.records(self.archive))
        self.assertEqual(queued | archived, set(ids))  # every card is somewhere
        self.assertFalse(queued & archived)
        exported = set(self.package_guids())
        # a card leaves the queue only when a complete package has it
        for card in archived:
            self.assertIn(guid_of(card), exported)
        # a card with a screenshot keeps it, beside its record or (a move cut short) in the other place
        for card in ids:
            record = self.queue / f"{card}.json"
            if not record.exists():
                record = self.archive / f"{card}.json"
            if json.loads(record.read_text("utf-8"))["picture"]:
                self.assertTrue((self.queue / f"{card}.jpg").exists() or (self.archive / f"{card}.jpg").exists())


class SaveRestartTests(Storage):
    """The overlay's enter: a save that stops anywhere leaves the card whole or not there."""

    def test_saves_in_fresh_processes(self):
        for action, count in (("save", 1), ("save-other", 2)):
            saved = self.run_step(SHELL, action)
            self.assertEqual((saved["status"], saved["count"]), ("Saved for Anki", count))
        self.assertEqual(self.run_step(SHELL, "save")["status"], "Already queued")
        records = [json.loads((self.queue / f"{i}.json").read_text("utf-8")) for i in self.records(self.queue)]
        self.assertEqual({r["version"] for r in records}, {4})
        self.assertEqual(sorted(r["spelling"] for r in records), ["好き", "猫"])
        self.assertEqual({(r["game"], r["title_id"], r["platform"]) for r in records}, {("CLANNAD", "PCSG00415", "vita")})
        self.assertFalse([p for p in self.queue.iterdir() if p.name.endswith((".jpg", ".tmp"))])

    def test_interrupted_save_never_damages_previously_saved_cards(self):
        for point in ("json_partial", "json_flushed", "json_renamed", "json_committed"):
            with self.subTest(checkpoint=point):
                self.use(Path(self.temp.name) / point)
                self.run_step(SHELL, "save")
                original = self.snapshot(self.queue)
                self.run_step(SHELL, "save-other", crash=point)
                for name, data in original.items():
                    self.assertEqual((self.queue / name).read_bytes(), data)
                # a final JSON is complete; unfinished .tmp records are ignored
                expected = 2 if point in ("json_renamed", "json_committed") else 1
                self.assertEqual(len(self.records(self.queue)), expected)
                again = self.run_step(SHELL, "save-other")
                self.assertEqual(again["count"], 2)
                self.assertIn(again["status"], ("Saved for Anki", "Already queued"))
                for card in self.records(self.queue):
                    json.loads((self.queue / f"{card}.json").read_text("utf-8"))


class AppSaveRestartTests(Storage):
    """#313, the Rubi app's Look up page: a card it saves, stopped anywhere, is whole or not there, never damages the
    cards saved before, and the next save of it finishes it (or finds it queued)."""

    def test_every_stop_point(self):
        for point in ("json_partial", "json_flushed", "json_renamed", "json_committed"):
            with self.subTest(checkpoint=point):
                self.use(Path(self.temp.name) / ("app-" + point))
                self.assertEqual(self.run_step(APP_QUEUE, "save"), {"status": "Saved", "count": 1})
                original = self.snapshot(self.queue)
                self.run_step(APP_QUEUE, "save-other", crash=point)
                for name, data in original.items():
                    self.assertEqual((self.queue / name).read_bytes(), data)
                self.assertEqual(len(self.records(self.queue)), 2 if point in ("json_renamed", "json_committed") else 1)
                again = self.run_step(APP_QUEUE, "save-other")
                self.assertEqual(again["count"], 2)
                self.assertIn(again["status"], ("Saved", "Already queued"))
                self.assertFalse([p for p in self.queue.iterdir() if p.name.endswith(".tmp")])
                for card in self.records(self.queue):
                    record = json.loads((self.queue / f"{card}.json").read_text("utf-8"))
                    self.assertEqual((record["version"], record["picture"]), (4, False))
                # and the export takes both, as any card (the queue's IDs are the records' SHA-256)
                done = self.run_step(APP, "export")
                self.assertEqual((done["rc"], done["cards"]), (0, 2), done)
                self.assertEqual(set(self.package_guids()), {guid_of(i) for i in self.records(self.archive)})


class ScreenshotSaveRestartTests(Storage):
    """API 23, enter with screenshots on: the game's JPEG first, then the record that names it. A
    stop anywhere leaves the card whole (record and JPEG) or not there (a JPEG alone is never a
    card), and the next save of it replaces a JPEG left alone."""

    def pictures(self):
        names = {p.name for p in self.queue.iterdir() if p.is_file()}
        cards = {i: json.loads((self.queue / f"{i}.json").read_text("utf-8")) for i in self.records(self.queue)}
        for card, record in cards.items():
            self.assertEqual(f"{card}.jpg" in names, record["picture"], card)  # a picture card has its JPEG
        return cards, sorted(n[:32] for n in names if n.endswith(".jpg"))

    def test_every_stop_point(self):
        for point in ("jpeg_saved", "json_partial", "json_flushed", "json_renamed", "json_committed"):
            with self.subTest(checkpoint=point):
                self.use(Path(self.temp.name) / ("shot-" + point))
                self.run_step(SHELL, "save")  # a text-only card saved before
                original = self.snapshot(self.queue)
                self.run_step(SHELL, "save-shot-other", crash=point)
                for name, data in original.items():
                    self.assertEqual((self.queue / name).read_bytes(), data)
                cards, jpgs = self.pictures()
                # the record is the card: before its rename there is none (its JPEG may be left alone)
                self.assertEqual(len(cards), 2 if point in ("json_renamed", "json_committed") else 1)
                self.assertEqual(len(jpgs), 1)
                again = self.run_step(SHELL, "save-shot-other")
                self.assertEqual(again["count"], 2)
                self.assertIn(again["status"], ("Saved for Anki", "Already queued"))
                cards, jpgs = self.pictures()
                self.assertEqual(jpgs, [c for c, r in cards.items() if r["picture"]])  # no JPEG left alone
                self.assertEqual(len(jpgs), 1)


class ExportRestartTests(Storage):
    """The Rubi app's export: a stop anywhere loses no card and leaves no broken package."""

    # in their order: written in part, flushed, checked (still under its temporary name), given its
    # name, the directory synced and the first card about to move, a JPEG moved, a record moved, later moves
    POINTS = ("apkg_partial", "apkg_flushed", "checked", "apkg_renamed", "verified", "moved_jpg", "moved_json", "moving")

    def test_every_stop_point(self):
        for point in self.POINTS:
            with self.subTest(checkpoint=point):
                self.use(Path(self.temp.name) / point)
                ids = self.run_step(APP, "queue")["ids"]
                before = self.snapshot(self.queue)
                self.run_step(APP, "export", crash=point)
                self.check_nothing_lost(ids)
                if point in ("apkg_partial", "apkg_flushed", "checked", "apkg_renamed", "verified"):
                    # nothing was cleared: the queue is byte for byte as it was
                    self.assertEqual(self.snapshot(self.queue), before)
                # a package under its final name is a checked one: only after "checked"
                packages = sorted(self.exports.glob("*.apkg")) if self.exports.exists() else []
                self.assertEqual(len(packages), 0 if point in ("apkg_partial", "apkg_flushed", "checked") else 1)
                after_crash = self.run_step(APP, "inspect")
                # the next export takes what is still queued; an interrupted one's temporary file goes
                done = self.run_step(APP, "export")
                self.assertEqual(done["rc"], 0, done)
                self.assertEqual(done["cards"], after_crash["queued"])
                self.assertEqual(self.run_step(APP, "inspect")["queued"], 0)
                self.assertFalse(list(self.exports.glob("*.tmp")))
                self.check_nothing_lost(ids)
                self.assertEqual(set(self.package_guids()), {guid_of(i) for i in ids})
                # and all of them again, with the same GUIDs
                everything = self.run_step(APP, "export-all")
                self.assertEqual((everything["rc"], everything["cards"], everything["moved"]), (0, 5, 0))


class ScreenshotRecoveryTests(Storage):
    """Read recovered packages with SQLite, across fresh processes and repeated exports."""

    def test_lost_images_keep_the_text_original_records_and_guids(self):
        ids = self.run_step(APP, "queue")["ids"]
        originals = self.snapshot(self.queue)
        records = {i: json.loads(originals[f"{i}.json"]) for i in ids}
        pictures = [i for i in ids if records[i]["picture"]]
        self.assertEqual(len(pictures), 2)
        (self.queue / f"{pictures[0]}.jpg").unlink()
        damaged = self.queue / f"{pictures[1]}.jpg"
        damaged.write_bytes(b"damaged screenshot")
        for action in ("export", "export-all"):
            done = self.run_step(APP, action)
            self.assertEqual((done["rc"], done["cards"], done["missing_picture"]), (0, 5, 2), done)
            self.assertEqual(self.records(self.queue), [])
            self.assertEqual(damaged.read_bytes(), b"damaged screenshot")
            for i in ids:
                self.assertEqual((self.archive / f"{i}.json").read_bytes(), originals[f"{i}.json"])
            with zipfile.ZipFile(self.exports / done["name"]) as z:
                self.assertIsNone(z.testzip())
                self.assertEqual(json.loads(z.read("media")), {})
                db = self.root / "recovered.anki2"
                db.write_bytes(z.read("collection.anki2"))
            with sqlite3.connect(db) as conn:
                self.assertEqual(conn.execute("pragma integrity_check").fetchone(), ("ok",))
                notes = dict(conn.execute("select guid, flds from notes"))
            self.assertEqual(set(notes), {guid_of(i) for i in ids})
            for i, record in records.items():
                fields = notes[guid_of(i)].split("\x1f")
                self.assertEqual(len(fields), 22)
                self.assertEqual(fields[0], record["spelling"])
                self.assertEqual(fields[2], record["reading"])
                self.assertIn(record["meanings"][0].split(" <")[0], fields[5])
                self.assertTrue(fields[7])  # sentence, with the original word highlighted
                self.assertEqual(fields[10], "")  # no broken image reference
                self.assertEqual(fields[21], record["misc"])


class OrphanScreenshotRestartTests(Storage):
    """#161: a save stopped during its JPEG's write (its .jpg.tmp) or between the JPEG and its record
    leaves a picture no card names. The next export that writes a package removes it from anki_queue/
    once it is old enough that no save of it can still be running (10 minutes older than the package),
    and never a card's own picture: beside its record, in exported/, or ahead of its record after an
    export stopped between a card's two moves."""

    OLD = 2 * 3600

    def age(self, paths, seconds=OLD):
        t = time.time() - seconds
        for p in paths:
            os.utime(p, (t, t))

    def age_all(self):
        """Every file of the queue and the archive made old: nothing is spared for its age."""
        for d in (self.queue, self.archive):
            if d.exists():
                self.age([p for p in d.iterdir() if p.is_file()])

    def loose(self):
        """anki_queue/'s pictures (.jpg, .jpg.tmp) that no record names."""
        names = {p.name for p in self.queue.iterdir() if p.is_file()}
        archived = set(self.records(self.archive))
        return sorted(n for n in names if n.endswith((".jpg", ".jpg.tmp"))
                      and n[:32] + ".json" not in names and n[:32] not in archived)

    def test_each_orphan_kind_goes_with_the_next_package(self):
        for point, suffix in (("jpeg_tmp", ".jpg.tmp"), ("jpeg_saved", ".jpg")):
            with self.subTest(checkpoint=point):
                self.use(Path(self.temp.name) / ("orphan-" + point))
                self.run_step(SHELL, "save-shot-other", crash=point)  # 好き: its JPEG, no record
                orphans = self.loose()
                self.assertEqual([n[32:] for n in orphans], [suffix])
                self.assertEqual(self.records(self.queue), [])
                self.assertEqual(self.run_step(SHELL, "save-shot")["status"], "Saved for Anki")  # 猫: a card
                card = self.records(self.queue)
                self.assertEqual(len(card), 1)
                # just left: an export keeps it (a save of it might still be running)
                done = self.run_step(APP, "export")
                self.assertEqual((done["rc"], done["cards"]), (0, 1), done)
                self.assertEqual(self.loose(), orphans)
                # old: the next export that writes a package removes it, and nothing else
                self.age_all()
                everything = self.run_step(APP, "export-all")
                self.assertEqual((everything["rc"], everything["cards"]), (0, 1), everything)
                self.assertFalse([p.name for p in self.queue.iterdir() if p.is_file()])
                self.assertTrue((self.archive / f"{card[0]}.jpg").exists())
                self.assertEqual(set(self.package_guids()), {guid_of(card[0])})

    def test_a_card_moved_half_way_keeps_its_picture(self):
        for point in ("moved_jpg", "moved_json", "moving"):
            with self.subTest(checkpoint=point):
                self.use(Path(self.temp.name) / ("half-" + point))
                ids = self.run_step(APP, "queue")["ids"]
                self.run_step(APP, "export", crash=point)
                self.age_all()  # the sweep armed for every file
                done = self.run_step(APP, "export")
                self.assertEqual(done["rc"], 0, done)
                self.check_nothing_lost(ids)
                self.assertEqual(self.records(self.queue), [])
                pictures = [i for i in ids if json.loads((self.archive / f"{i}.json").read_text("utf-8"))["picture"]]
                self.assertEqual(len(pictures), 2)
                for card in pictures:  # in exported/ now, beside its record
                    self.assertTrue((self.archive / f"{card}.jpg").exists(), card)
                everything = self.run_step(APP, "export-all")
                self.assertEqual((everything["rc"], everything["cards"]), (0, 5))

    def test_a_stop_while_removing_loses_nothing(self):
        self.run_step(SHELL, "save-shot-other", crash="jpeg_saved")
        self.run_step(SHELL, "save-shot", crash="jpeg_tmp")
        self.assertEqual(len(self.loose()), 2)
        ids = self.run_step(APP, "queue")["ids"]
        self.age_all()
        self.run_step(APP, "export", crash="swept")  # after the first removal
        self.check_nothing_lost(ids)
        self.assertEqual(len(self.loose()), 1)
        self.assertEqual(self.run_step(APP, "inspect")["queued"], 0)
        # nothing queued: no package, so nothing is removed until the next one
        self.assertEqual(self.run_step(APP, "export")["cards"], 0)
        self.assertEqual(len(self.loose()), 1)
        everything = self.run_step(APP, "export-all")
        self.assertEqual((everything["rc"], everything["cards"]), (0, 5))
        self.assertEqual(self.loose(), [])
        self.check_nothing_lost(ids)


class CardsRestartTests(Storage):
    """#315: the Cards page's Fix sentence, Drop picture and Delete stopped at each of their storage steps, in a
    fresh process (the queue's card with a picture whose ID is the smallest). Then an export straight away,
    whose recovery runs first, and one 10 minutes later, whose sweep removes the pictures no card names. Each
    time: no card is lost, the export packages each card once (the old card or the new one, never both), no
    record names a missing picture, no intent is left, and a change stopped after its new record is finished
    (the old card in anki_queue/deleted/), one stopped before it undone."""

    STEPS = {
        "fix": ("intent_tmp", "intent", "picture_tmp", "picture", "record_tmp", "record", "record_synced",
                "retired_json", "retired_jpg", "intent_removed"),
        "drop": ("intent_tmp", "intent", "record_tmp", "record", "record_synced", "retired_json", "retired_jpg",
                 "intent_removed"),
        "delete": ("retired_json", "retired_jpg"),
    }

    def deleted(self):
        return self.queue / "deleted"

    def outcome(self, action):
        """The change made without a stop, in a copy of the memory card: its old and new IDs."""
        here = self.root
        copy = here.parent / (here.name + "-whole")
        shutil.copytree(here, copy)
        self.use(copy)
        try:
            done = self.run_step(APP, action)
        finally:
            self.use(here)
        self.assertEqual(done["rc"], 0, done)
        return done

    def age_all(self, seconds=2 * 3600):
        t = time.time() - seconds
        for d in (self.queue, self.archive):
            if d.exists():
                for p in d.iterdir():
                    if p.is_file():
                        os.utime(p, (t, t))

    def no_record_names_a_missing_picture(self):
        for d in (self.queue, self.archive):
            for card in self.records(d):
                if json.loads((d / f"{card}.json").read_text("utf-8"))["picture"]:
                    self.assertTrue((self.queue / f"{card}.jpg").exists() or (self.archive / f"{card}.jpg").exists(),
                                    f"{d.name}/{card}.json names a picture that is not there")

    def loose(self):
        names = {p.name for p in self.queue.iterdir() if p.is_file()}
        archived = set(self.records(self.archive))
        return sorted(n for n in names if n.endswith((".jpg", ".jpg.tmp", ".json.tmp"))
                      and n[:32] + ".json" not in names and n[:32] not in archived)

    def test_every_stop_point(self):
        for action, points in self.STEPS.items():
            for point in points:
                with self.subTest(action=action, checkpoint=point):
                    self.use(Path(self.temp.name) / f"{action}-{point}")
                    ids = self.run_step(APP, "queue")["ids"]
                    whole = self.outcome(action)
                    old, new = whole["old"], whole["new"]
                    others = set(ids) - {old}
                    self.assertIn(old, ids)
                    self.assertNotEqual(old, new) if action != "delete" else None
                    self.run_step(APP, action, crash=point)
                    # never neither: the other cards are there, and the old card or the new one
                    queued = set(self.records(self.queue))
                    self.assertLessEqual(others, queued)
                    if action != "delete":
                        self.assertTrue(old in queued or new in queued)
                    self.no_record_names_a_missing_picture()
                    # an export straight away: its recovery first, then each card once
                    done = self.run_step(APP, "export")
                    self.assertEqual(done["rc"], 0, done)
                    finished = action == "delete" or points.index(point) >= points.index("record")
                    kept = set() if action == "delete" else {new if finished else old}
                    self.assertEqual(done["cards"], len(others | kept), done)
                    guids = self.package_guids()
                    self.assertEqual(sorted(guids), sorted(guid_of(i) for i in others | kept))
                    self.assertEqual(set(self.records(self.archive)), others | kept)
                    self.assertEqual(self.records(self.queue), [])
                    self.assertFalse(self.deleted().exists() and list(self.deleted().glob("*.fix*")), "an intent left")
                    self.assertEqual(finished, (self.deleted() / f"{old}.json").exists())
                    self.no_record_names_a_missing_picture()
                    # 10 minutes later: the sweep leaves no picture no card names; every card again, each once
                    self.age_all()
                    everything = self.run_step(APP, "export-all")
                    self.assertEqual((everything["rc"], everything["cards"]), (0, len(others | kept)), everything)
                    self.assertEqual(self.loose(), [])
                    self.no_record_names_a_missing_picture()
                    # the fixed card kept its picture; the dropped one has none
                    for card in kept:
                        record = json.loads((self.archive / f"{card}.json").read_text("utf-8"))
                        self.assertEqual(record["picture"], not (action == "drop" and finished))
                        if action == "fix" and finished:
                            self.assertTrue(record["sentence"].startswith("直した："))


if __name__ == "__main__":
    unittest.main(verbosity=2)
