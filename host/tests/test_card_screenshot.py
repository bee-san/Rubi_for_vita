"""A mined Anki card carries the game's screenshot, end to end on the host (docs/anki.md, "The screenshot"):

1. the actual game worker and kernel transport (host/tests/test_game_shot.c --shot) read a game's frame
   (docs/screenshot.jpg, a 960 x 544 game screen) through the display hook and write its JPEG beside the card, as the
   kernel's file worker does (anki_queue/<id>.jpg);
2. the actual Shell's Enter with Card mining and Screenshots on (host/tests/test_shell_anki.c --restart-step
   save-shot, shell/anki.c and its queue) saves the card: its game-worker job is answered with that JPEG
   (VJO_TEST_SHOT_JPEG), and the record says "picture": true;
3. the actual Rubi app's Export cards (host/tests/test_apkg.c --restart-step export, settings/anki_export.c and
   core/apkg.c) writes the .apkg;
4. here: the package's one note has the Lapis Picture field `<img src="vitajp_<32 hex>.jpg">`, the media map names
   that file, its bytes are the game worker's JPEG (named by its SHA-256), and Pillow decodes it as the frame
   (960 x 544, PSNR over 30 dB against it).

    python3 host/tests/test_card_screenshot.py BUILD/vjo-game-shot-tests BUILD/vjo-shell-anki-tests BUILD/vjo-apkg-tests

Each binary runs in a fresh process, as each part runs in its own process on the Vita. Host execution: no console
claim (docs/offline-testing.md)."""
import hashlib
import json
import math
import os
import re
import sqlite3
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
SHOT = str(Path(sys.argv.pop(1)).resolve())
SHELL = str(Path(sys.argv.pop(1)).resolve())
APP = str(Path(sys.argv.pop(1)).resolve())
PICTURE = 10  # the Lapis note's Picture field (host/tests/test_apkg.py FIELDS)


def psnr(a, b):
    pa, pb = a.tobytes(), b.tobytes()
    err = sum((x - y) * (x - y) for x, y in zip(pa, pb)) / len(pa)
    return 99.0 if not err else 10 * math.log10(255 * 255 / err)


class MinedCardCarriesTheScreenshot(unittest.TestCase):
    def test_the_game_workers_jpeg_reaches_the_cards_picture(self):
        with tempfile.TemporaryDirectory(prefix="vjo-card-shot-") as tmp:
            tmp = Path(tmp)
            frame = Image.open(ROOT / "docs" / "screenshot.jpg").convert("RGB")
            self.assertEqual(frame.size, (960, 544))
            (tmp / "frame.rgba").write_bytes(frame.convert("RGBA").tobytes())
            shot_dir = tmp / "game"
            shot_dir.mkdir()
            # 1. the game worker's JPEG of the frame (the default trim: this frame has no black bars)
            done = subprocess.run([SHOT, "--shot", str(tmp / "frame.rgba"), "960", "544", "abgr", str(shot_dir), "trim"],
                                  capture_output=True, text=True, timeout=300)
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            r = json.loads(done.stdout.strip().splitlines()[-1])
            jpeg = Path(r["path"]).read_bytes()
            self.assertEqual(len(jpeg), r["bytes"])
            self.assertTrue(jpeg.startswith(b"\xff\xd8\xff") and jpeg.endswith(b"\xff\xd9"))
            # 2. Enter on a word with Card mining and Screenshots on: the card and that JPEG in the queue
            card = tmp / "card"
            card.mkdir()
            env = dict(os.environ, VJO_TEST_SHOT_JPEG=r["path"])
            done = subprocess.run([SHELL, "--restart-step", str(card), "save-shot", ""], capture_output=True, text=True,
                                  timeout=120, env=env)
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            self.assertEqual(json.loads(done.stdout)["status"], "Saved for Anki")
            queue = card / "ux0:data" / "VitaJPOverlay" / "anki_queue"
            records = sorted(queue.glob("*.json"))
            self.assertEqual(len(records), 1)
            record = json.loads(records[0].read_text("utf-8"))
            self.assertIs(record["picture"], True)
            self.assertEqual((queue / (records[0].stem + ".jpg")).read_bytes(), jpeg)
            # 3. the Rubi app's Export cards
            done = subprocess.run([APP, "--restart-step", str(card), "export", ""], capture_output=True, text=True,
                                  timeout=120)
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            packages = sorted((card / "ux0:data" / "VitaJPOverlay" / "anki").glob("*.apkg"))
            self.assertEqual(len(packages), 1)
            # 4. the note's Picture is the game's screen
            with zipfile.ZipFile(packages[0]) as z:
                media = json.loads(z.read("media"))
                (tmp / "c.anki2").write_bytes(z.read("collection.anki2"))
                files = {name: z.read(key) for key, name in media.items()}
            conn = sqlite3.connect(tmp / "c.anki2")
            notes = [flds.split("\x1f") for (flds,) in conn.execute("select flds from notes")]
            conn.close()
            self.assertEqual(len(notes), 1)
            m = re.fullmatch(r'<img src="(vitajp_[0-9a-f]{32}\.jpg)">', notes[0][PICTURE])
            self.assertIsNotNone(m, notes[0][PICTURE])
            name = m.group(1)
            self.assertEqual(name, "vitajp_%s.jpg" % hashlib.sha256(jpeg).hexdigest()[:32])
            self.assertIn(name, files)
            self.assertEqual(files[name], jpeg)
            (tmp / "picture.jpg").write_bytes(files[name])
            with Image.open(tmp / "picture.jpg") as im:
                im.load()
                self.assertEqual((im.format, im.size), ("JPEG", (960, 544)))
                quality = psnr(im.convert("RGB"), frame)
            print(f"card screenshot: {len(jpeg)} B JPEG, quality {r['quality']}, PSNR {quality:.1f} dB against the frame")
            self.assertGreater(quality, 30.0)


if __name__ == "__main__":
    unittest.main()
