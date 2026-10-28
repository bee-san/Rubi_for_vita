"""The Rubi app's LiveArea art is what tools/make_settings_assets.py makes of settings/assets/icon-source.png."""
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

try:
    import make_settings_assets as art
    import check_settings_assets as assets
except ImportError:  # (no Pillow)
    art = None


@unittest.skipUnless(art, "Pillow draws the art")
class MakeSettingsAssets(unittest.TestCase):
    def test_check_rebuilds_the_committed_bytes(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = art.main(["--check"])
        self.assertEqual(rc, 0, out.getvalue())
        self.assertEqual(out.getvalue().splitlines(), [f"{n}: the committed bytes" for n in art.NAMES])

    def test_out_writes_art_the_console_accepts(self):
        with tempfile.TemporaryDirectory() as temporary:
            built = Path(temporary) / "assets"
            self.assertEqual(art.main(["--out", str(built)]), 0)
            self.assertEqual(sorted(p.name for p in built.iterdir()), sorted(art.NAMES))
            self.assertEqual(len(assets.check(built)), 4)  # the checks every packaged image passes

    def test_check_names_a_changed_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            committed = Path(temporary)
            for name in art.NAMES:
                (committed / name).write_bytes((art.DEST / name).read_bytes())
            (committed / "bg.png").write_bytes(b"not the art")
            dest, art.DEST = art.DEST, committed
            out = io.StringIO()
            try:
                with contextlib.redirect_stdout(out):
                    rc = art.main(["--check"])
            finally:
                art.DEST = dest
        self.assertEqual(rc, 1)
        self.assertIn("bg.png: DIFFERS", out.getvalue())
        self.assertIn("icon0.png: the committed bytes", out.getvalue())


if __name__ == "__main__":
    unittest.main()
