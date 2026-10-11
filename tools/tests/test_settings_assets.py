"""Guard the native install regression in committed and packaged settings art."""
import binascii
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import check_settings_assets as assets


class SettingsAssets(unittest.TestCase):
    def test_committed_and_packaged_assets_meet_native_constraints(self):
        directory = ROOT / "settings/assets"
        self.assertEqual(len(assets.check(directory)), 3)
        with tempfile.TemporaryDirectory() as temporary:
            vpk = Path(temporary) / "settings.vpk"
            with zipfile.ZipFile(vpk, "w") as archive:
                for name in assets.ASSETS:
                    archive.write(directory / Path(name).name, name)
            self.assertEqual(assets.check(vpk), assets.check(directory))

    def test_vpk_rejects_missing_or_duplicate_native_asset(self):
        with tempfile.TemporaryDirectory() as temporary:
            for duplicate in (False, True):
                vpk = Path(temporary) / "settings.vpk"
                with zipfile.ZipFile(vpk, "w") as archive:
                    for name in assets.ASSETS:
                        if name.endswith("icon0.png") and not duplicate:
                            continue
                        data = (ROOT / "settings/assets" / Path(name).name).read_bytes()
                        archive.writestr(name, data)
                        if name.endswith("icon0.png") and duplicate:
                            with self.assertWarns(UserWarning):
                                archive.writestr(name, data)
                with self.assertRaisesRegex(ValueError, "exactly one"):
                    assets.check(vpk)

    def test_reject_rgba_wrong_size_low_bit_depth_and_interlacing(self):
        original = (ROOT / "settings/assets/icon0.png").read_bytes()
        # Change the real committed IHDR and recalculate its CRC, so rejection
        # checks native compatibility rather than an incidental broken checksum.
        for offset, value in ((25, 6), (24, 4), (28, 1), (19, 127)):
            modified = bytearray(original)
            modified[offset] = value
            struct.pack_into(">I", modified, 29, binascii.crc32(modified[12:29]) & 0xFFFFFFFF)
            with self.subTest(offset=offset, value=value):
                with self.assertRaises(ValueError):
                    assets.check_png(modified, "icon0.png", (128, 128))

    def test_reject_corrupt_or_truncated_artwork(self):
        original = (ROOT / "settings/assets/icon0.png").read_bytes()
        modified = bytearray(original)
        modified[35] ^= 1
        for data in (modified, original[:-4]):
            with self.assertRaises(ValueError):
                assets.check_png(data, "icon0.png", (128, 128))

    def test_reject_transparent_icon(self):
        original = (ROOT / "settings/assets/icon0.png").read_bytes()
        # Add a valid palette transparency chunk before the first IDAT.
        position = 8
        while original[position + 4:position + 8] != b"IDAT":
            position += 12 + struct.unpack_from(">I", original, position)[0]
        chunk = b"tRNS\x00"
        transparent = (original[:position] + struct.pack(">I", 1) + chunk +
                       struct.pack(">I", binascii.crc32(chunk) & 0xFFFFFFFF) + original[position:])
        with self.assertRaisesRegex(ValueError, "opaque"):
            assets.check_png(transparent, "icon0.png", (128, 128))
