"""Guard the native install regression in committed and packaged settings art."""
import binascii
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import check_settings_assets as assets


def make_sfo(values):
    """A PSF file (param.sfo) with string values, as vita-mksfoex writes them (UTF-8, NUL-padded to 4 bytes)."""
    keys = sorted(values)
    key_table = b"".join(k.encode() + b"\0" for k in keys)
    key_table += b"\0" * (-len(key_table) % 4)
    data_table, entries, key_at = b"", b"", 0
    for k in keys:
        v = values[k].encode() + b"\0"
        size = len(v) + (-len(v) % 4)
        entries += struct.pack("<HHIII", key_at, 0x0204, len(v), size, len(data_table))
        data_table += v + b"\0" * (size - len(v))
        key_at += len(k) + 1
    header = struct.pack("<4sIIII", b"\0PSF", 0x101, 20 + len(entries), 20 + len(entries) + len(key_table), len(keys))
    return header + entries + key_table + data_table


SFO = make_sfo({"TITLE_ID": "VJOSET001", "TITLE": "Rubi", "STITLE": "Rubi", "APP_VER": "01.00"})


class SettingsAssets(unittest.TestCase):
    def test_committed_and_packaged_assets_meet_native_constraints(self):
        directory = ROOT / "settings/assets"
        self.assertEqual(len(assets.check(directory)), 4)
        with tempfile.TemporaryDirectory() as temporary:
            vpk = Path(temporary) / "settings.vpk"
            with zipfile.ZipFile(vpk, "w") as archive:
                archive.writestr("sce_sys/param.sfo", SFO)
                for name in assets.ASSETS:
                    archive.write(directory / Path(name).name, name)
            self.assertEqual(assets.check(vpk), assets.check(directory))

    def test_vpk_rejects_missing_or_duplicate_native_asset(self):
        with tempfile.TemporaryDirectory() as temporary:
            for duplicate in (False, True):
                vpk = Path(temporary) / "settings.vpk"
                with zipfile.ZipFile(vpk, "w") as archive:
                    archive.writestr("sce_sys/param.sfo", SFO)
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

    def test_the_param_sfo_keeps_the_title_id_and_names_rubi(self):
        """The VPK's param.sfo: TITLE_ID VJOSET001, every earlier build's (another ID installs a second app beside
        the old one), and the LiveArea title Rubi (docs/rename-rubi.md)."""
        self.assertEqual(assets.check_sfo(SFO), {"TITLE_ID": "VJOSET001", "TITLE": "Rubi", "STITLE": "Rubi"})
        for key, value in (("TITLE_ID", "VJOSET002"), ("TITLE", "Overlay"), ("STITLE", "JP Overlay Settings")):
            values = {"TITLE_ID": "VJOSET001", "TITLE": "Rubi", "STITLE": "Rubi", key: value}
            with self.subTest(key=key):
                with self.assertRaisesRegex(ValueError, key):
                    assets.check_sfo(make_sfo(values))
        with tempfile.TemporaryDirectory() as temporary:
            vpk = Path(temporary) / "settings.vpk"
            with zipfile.ZipFile(vpk, "w") as archive:
                archive.writestr("sce_sys/param.sfo", make_sfo({"TITLE_ID": "VJOSET002", "TITLE": "Rubi", "STITLE": "Rubi"}))
                for name in assets.ASSETS:
                    archive.write(ROOT / "settings/assets" / Path(name).name, name)
            with self.assertRaisesRegex(ValueError, "TITLE_ID"):
                assets.check(vpk)
            with zipfile.ZipFile(vpk, "w") as archive:
                for name in assets.ASSETS:
                    archive.write(ROOT / "settings/assets" / Path(name).name, name)
            with self.assertRaisesRegex(ValueError, "param.sfo"):
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

    def test_reject_art_over_the_vita_size_limits(self):
        # The SDK's limits (content information specification): icon0.png 32 KiB, the start-up
        # image pic0.png 600 KiB. Pinned here, so the checker cannot drift from them.
        limits = {"sce_sys/icon0.png": 32768, "sce_sys/pic0.png": 614400}
        self.assertEqual(assets.MAX_BYTES, limits)
        for name, size in (("sce_sys/icon0.png", (128, 128)), ("sce_sys/pic0.png", (960, 544))):
            with self.subTest(name=name):
                original = (ROOT / "settings/assets" / Path(name).name).read_bytes()
                limit = limits[name]
                self.assertLessEqual(len(original), limit)
                # Pad the real image with a valid private chunk after IHDR until it is one byte too large.
                payload = b"\0" * (limit + 1 - len(original) - 12)
                chunk = b"prVt" + payload
                padded = (original[:33] + struct.pack(">I", len(payload)) + chunk +
                          struct.pack(">I", binascii.crc32(chunk) & 0xFFFFFFFF) + original[33:])
                self.assertEqual(len(padded), limit + 1)
                assets.check_png(padded, name, size)
                with self.assertRaisesRegex(ValueError, f"at most {limit}"):
                    assets.check_png(padded, name, size, limit)
                with tempfile.TemporaryDirectory() as temporary:
                    vpk = Path(temporary) / "settings.vpk"
                    with zipfile.ZipFile(vpk, "w") as archive:
                        archive.writestr("sce_sys/param.sfo", SFO)
                        for other in assets.ASSETS:
                            data = (ROOT / "settings/assets" / Path(other).name).read_bytes()
                            archive.writestr(other, padded if other == name else data)
                    with self.assertRaisesRegex(ValueError, f"at most {limit}"):
                        assets.check(vpk)

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

    @staticmethod
    def with_chunk(original, kind, payload):
        """The real image with one valid chunk inserted before its first IDAT."""
        position = 8
        while original[position + 4:position + 8] != b"IDAT":
            position += 12 + struct.unpack_from(">I", original, position)[0]
        chunk = kind + payload
        return (original[:position] + struct.pack(">I", len(payload)) + chunk +
                struct.pack(">I", binascii.crc32(chunk) & 0xFFFFFFFF) + original[position:])

    @staticmethod
    def padded_to(original, size):
        """The real image grown to exactly size bytes with a valid private chunk after IHDR."""
        payload = b"\0" * (size - len(original) - 12)
        chunk = b"prVt" + payload
        return (original[:33] + struct.pack(">I", len(payload)) + chunk +
                struct.pack(">I", binascii.crc32(chunk) & 0xFFFFFFFF) + original[33:])

    def checked_as(self, name, data, temporary):
        """check() of a VPK and of an assets directory with the committed art, data as name."""
        vpk = Path(temporary) / "settings.vpk"
        directory = Path(temporary) / "assets"
        directory.mkdir(exist_ok=True)
        with zipfile.ZipFile(vpk, "w") as archive:
            archive.writestr("sce_sys/param.sfo", SFO)
            for other in assets.ASSETS:
                art = data if other == name else (ROOT / "settings/assets" / Path(other).name).read_bytes()
                archive.writestr(other, art)
                (directory / Path(other).name).write_bytes(art)
        return vpk, directory

    def test_reject_text_chunks_in_any_image(self):
        # A text chunk is what an image editor adds (its name, a comment): no display data, which
        # the SDK's specification asks the icon and the start-up image to hold only.
        texts = {b"tEXt": b"Software\0GIMP 2.10",
                 b"zTXt": b"Comment\0\0" + zlib.compress(b"made in an image editor"),
                 b"iTXt": b"Description\0\0\0\0\0Overlay"}
        for name, size in assets.ASSETS.items():
            original = (ROOT / "settings/assets" / Path(name).name).read_bytes()
            for kind, payload in texts.items():
                with self.subTest(name=name, kind=kind):
                    data = self.with_chunk(original, kind, payload)
                    with self.assertRaisesRegex(ValueError, f"a {kind.decode()} chunk"):
                        assets.check_png(data, name, size, assets.MAX_BYTES.get(name))
                    with tempfile.TemporaryDirectory() as temporary:
                        for source in self.checked_as(name, data, temporary):
                            with self.assertRaisesRegex(ValueError, f"a {kind.decode()} chunk"):
                                assets.check(source)
        # other ancillary chunks are not text: a private one still passes (the size tests use it)
        icon = (ROOT / "settings/assets/icon0.png").read_bytes()
        assets.check_png(self.with_chunk(icon, b"prVt", b"\0" * 8), "icon0.png", (128, 128), 32768)

    def test_art_of_exactly_the_limit_passes(self):
        # The SDK's limits are "or less": an icon0.png of exactly 32,768 B is allowed (C7, `>=`).
        for name, size in (("sce_sys/icon0.png", (128, 128)), ("sce_sys/pic0.png", (960, 544))):
            limit = assets.MAX_BYTES[name]
            with self.subTest(name=name):
                exact = self.padded_to((ROOT / "settings/assets" / Path(name).name).read_bytes(), limit)
                self.assertEqual(len(exact), limit)
                self.assertEqual(assets.check_png(exact, name, size, limit)["bytes"], limit)
                with tempfile.TemporaryDirectory() as temporary:
                    for source in self.checked_as(name, exact, temporary):
                        self.assertEqual(assets.check(source)[name]["bytes"], limit)

    def test_the_directory_check_enforces_the_limits(self):
        # The manual pre-build check of settings/assets applies the same caps as the VPK's (C8).
        for name in ("sce_sys/icon0.png", "sce_sys/pic0.png"):
            limit = assets.MAX_BYTES[name]
            with self.subTest(name=name):
                large = self.padded_to((ROOT / "settings/assets" / Path(name).name).read_bytes(), limit + 1)
                with tempfile.TemporaryDirectory() as temporary:
                    _, directory = self.checked_as(name, large, temporary)
                    with self.assertRaisesRegex(ValueError, f"at most {limit}"):
                        assets.check(directory)
