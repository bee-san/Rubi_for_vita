"""Restart/crash tests of the offline Anki queue: every Vita-side action runs
in a fresh OS process.

The overlay's saves (the real Shell Anki thread and queue storage,
vjo-shell-anki-tests --restart-step) stop at each storage boundary of a
save; the Overlay app's exports (settings/anki_export.c and its package
writer, vjo-apkg-tests --restart-step) stop at each boundary of writing,
verifying and clearing. After every stop a fresh process finds every card
either still queued or in a complete package (and archived), never lost,
never half-written, and a later export takes the rest. These check process
crashes and disk recovery, not electrical power loss on physical
Vita/SD2Vita hardware.

    python3 host/tests/test_anki_restart.py BUILD/vjo-shell-anki-tests BUILD/vjo-apkg-tests
"""
import json
import sqlite3
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

SHELL = str(Path(sys.argv.pop(1)).resolve())
APP = str(Path(sys.argv.pop(1)).resolve())
BASE91 = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&()*+,-./:;<=>?@[]^_`{|}~"


def guid_of(queue_id):
    v, out = int(queue_id[:16], 16), ""
    while True:
        out = BASE91[v % 91] + out
        v //= 91
        if not v:
            return out


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


class ExportRestartTests(Storage):
    """The Overlay app's export: a stop anywhere loses no card and leaves no broken package."""

    # in their order: written in part, flushed, checked (still under its temporary name), given its
    # name, the directory synced and the first card about to move, a JPEG moved, a record moved, later moves
    POINTS = ("apkg_partial", "apkg_flushed", "checked", "apkg_renamed", "verified", "moved_jpg", "moved_json", "moving")

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


if __name__ == "__main__":
    unittest.main(verbosity=2)
