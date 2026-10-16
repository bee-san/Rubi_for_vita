"""tools/package_vpk.py, the one-VPK install's package (docs/one-vpk-install.md): the payload list is
the app's compiled one (include/vjo_payload.h), the manifest is core/payload.h's format with the app's
own facts, and the checks refuse what VitaShell would warn about or the app would refuse. The SELFs are
synthetic, laid out as vita-make-fself writes them, with real import tables; the C side of the rules
(core/payload.c) is host/tests/test_settings_install.c's."""
import hashlib
import io
import json
import re
import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import package_vpk as vpk  # noqa: E402


def make_self(imports=(), strings=b""):
    """A Vita SELF whose one PT_LOAD segment holds a module info naming an import table of
    imports ((library, [NIDs]), 3xx entries of 52 bytes), the libraries' names, their NID tables and
    strings. Segment address 0x81000000, uncompressed."""
    base = 0x81000000
    info_size, entry_size = 92, 52
    body = bytearray(info_size + entry_size * len(imports))
    tail = bytearray()
    entries = []
    for library, nids in imports:
        name_at = len(body) + len(tail)
        tail += library.encode() + b"\0"
        while len(tail) % 4:
            tail += b"\0"
        nids_at = len(body) + len(tail)
        tail += struct.pack(f"<{len(nids)}I", *nids)
        entries.append((name_at, nids_at, len(nids)))
    segment = body + tail + strings
    struct.pack_into("<II", segment, 44, info_size, info_size + entry_size * len(imports))  # import top, end
    for i, (name_at, nids_at, functions) in enumerate(entries):
        at = info_size + entry_size * i
        struct.pack_into("<HHHH", segment, at, entry_size, 1, 0, functions)
        struct.pack_into("<I", segment, at + 20, base + name_at)
        struct.pack_into("<I", segment, at + 28, base + nids_at)
    elf_off, ph_off = 0xA0, 0xA0 + 0x34
    info_off = ph_off + 32
    data_off = info_off + 32
    data = bytearray(b"SCE\0" + b"\0" * (data_off - 4))
    struct.pack_into("<QQ", data, 0x40, elf_off, ph_off)
    struct.pack_into("<Q", data, 0x58, info_off)
    data[elf_off:elf_off + 6] = b"\x7fELF\x01\x01"
    struct.pack_into("<I", data, elf_off + 24, 0)  # e_entry: segment 0, offset 0
    struct.pack_into("<HH", data, elf_off + 42, 32, 1)
    struct.pack_into("<8I", data, ph_off, 1, 0, base, 0, len(segment), len(segment), 5, 0x1000)
    struct.pack_into("<4Q", data, info_off, data_off, len(segment), 1, 2)
    return bytes(data) + bytes(segment)


def app_eboot(version="0.8.0", debug=False, worker="kernel", imports=(("SceShellUtil", [0x7C79BDD7]),)):
    start = b"start: v" + version.encode() + (b" debug" if debug else b"") + b"; %s; heap %d B"
    return make_self(imports, b"\0" + start + b"\0vjoworker=" + worker.encode() + b"\0")


def app_vpk(eboot):
    stamp = f"vjoapp1 api=22 build={hashlib.sha256(eboot).hexdigest()[:16]}\n"
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("sce_sys/param.sfo", b"\0PSF fixture")
        z.writestr("eboot.bin", eboot)
        z.writestr("sce_sys/icon0.png", b"png")
        z.writestr("vjo_app.txt", stamp)
    return out.getvalue(), stamp


LICENCES = {"LICENSE": b"GPL-3.0 fixture\n", "third_party/README.md": b"third-party index\n",
            "third_party/licenses/BearSSL-MIT.txt": b"MIT fixture\n", "docs/plaintext-attribution.txt": b"CC BY-SA 4.0\n",
            "docs/JMdict-LICENSE.html": b"<html>JMdict</html>\n"}


def release_zip(path, eboot=None, kernel_imports=(), shell_imports=(), variant="normal", settings=None, corrupt=None,
                licences=LICENCES):
    """A package_release.py-shaped zip: the app, the payload's files and a manifest of their hashes."""
    app, stamp = app_vpk(eboot or app_eboot(debug=variant == "debug"))
    files = {}
    for dest, kind in vpk.payload_files():
        name = vpk.zip_path(dest)
        if name.endswith("Kernel.skprx"):
            data = make_self(kernel_imports)
        elif name.endswith("Shell.suprx"):
            data = make_self(shell_imports)
        elif name.endswith("OCR.suprx"):
            data = make_self()
        else:
            data = (name + " payload\n").encode() * (50 if name.endswith(".vjdict") else 1)
        files[name] = data
    manifest = {"version": "0.8.0", "variant": variant, "files": {}}
    if settings:
        manifest["install_settings"] = settings
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("JPOverlaySettings.vpk", app)
        z.writestr("ux0/data/VitaJPOverlay/vjo_app.txt", stamp)
        for name, data in list(files.items()) + list(licences.items()):
            written = b"damaged" if name == corrupt else data
            z.writestr(name, written)
            manifest["files"][name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        z.writestr("docs/console-test.md", b"not a licence\n")  # a file the VPK does not carry
        z.writestr("package-manifest.json", json.dumps(manifest))
    return path


class BuildCheck(unittest.TestCase):
    def test_the_vita_builds_modules_are_checked(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "eboot.bin").write_bytes(app_eboot())
            (tmp / "shell.suprx").write_bytes(make_self([("SceVshBridge", [0x38612972])]))
            (tmp / "kernel.skprx").write_bytes(make_self())
            self.assertEqual(vpk.check_build(tmp / "eboot.bin", [tmp / "shell.suprx", tmp / "kernel.skprx"], "kernel"), [])
            self.assertTrue(vpk.check_build(tmp / "eboot.bin", [], "lines"))  # the header says otherwise
            (tmp / "shell.suprx").write_bytes(make_self([("SceVshBridge", [0x3C522C35])]))
            self.assertTrue(any("brick" in p for p in vpk.check_build(tmp / "eboot.bin", [tmp / "shell.suprx"], "kernel")))


class PayloadList(unittest.TestCase):
    def test_the_python_list_is_the_compiled_one(self):
        files = vpk.payload_files()
        self.assertEqual(len(files), 14)
        header = (ROOT / "include/vjo_payload.h").read_text()
        self.assertIn("#define VJO_PAYLOAD_KERNEL_INDEX 9", header)
        self.assertEqual(files[9], ("ur0:tai/VitaJPOverlay_Kernel.skprx", "copy"))
        self.assertEqual(files[8], ("ux0:data/VitaJPOverlay/dictionaries/plaintext.vjdict", "move"))
        self.assertEqual(files[-1], ("ux0:data/VitaJPOverlay/config.ini", "template"))
        # the models are the package's shipped ones (package_release.py shipped_models), nothing else
        import package_release
        shipped = {name for name, *_ in package_release.shipped_models()} | {"LM-ATTRIBUTION.md"}
        self.assertEqual({d.rsplit("/", 1)[1] for d, _ in files if "/ocr/" in d}, shipped)
        # the app's own record is never in the payload (the install writes it, core/app_stamp.h)
        self.assertNotIn("ux0:data/VitaJPOverlay/vjo_app.txt", [d for d, _ in files])


class Package(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def test_a_vpk_of_a_release_zip(self):
        release = release_zip(self.root / "r.zip", settings={"anki_mining": "on"})
        out = self.root / "VitaJPOverlay-0.8.0.vpk"
        text = vpk.write_vpk(release, out)
        self.assertEqual(vpk.verify(out), [])
        with zipfile.ZipFile(out) as z:
            names = z.namelist()
            self.assertEqual(names[:2], ["sce_sys/param.sfo", "eboot.bin"])
            self.assertEqual(z.read("payload/manifest.txt").decode(), text)
            self.assertTrue(all(i.compress_type == zipfile.ZIP_DEFLATED for i in z.infolist() if i.filename.startswith("payload/")))
            # the licences, at their zip paths under licenses/, after the payload: the app reads none of them
            self.assertEqual({n[len("licenses/"):]: z.read(n) for n in names if n.startswith("licenses/")}, LICENCES)
            self.assertGreater(min(names.index("licenses/" + n) for n in LICENCES), names.index("payload/manifest.txt"))
            self.assertNotIn("licenses/docs/console-test.md", names)
        lines = text.splitlines()
        self.assertEqual(lines[:7], ["vjopayload 1", "version 0.8.0", "app_version 0.8.0", "variant normal", "api 22",
                                     "worker kernel", "build " + lines[6].split()[1]])
        self.assertIn("set anki_mining on", lines)
        self.assertNotIn("set log_file on", lines)
        self.assertTrue(re.fullmatch(r"file ur0:tai/VitaJPOverlay_Kernel\.skprx \d+ [0-9a-f]{64} copy", lines[7 + 9]))
        record = vpk.installed_record(text)
        self.assertRegex(record, r"^vjoinst1 version=0\.8\.0 variant=normal api=22 build=[0-9a-f]{16} manifest=[0-9a-f]{64}\n$")
        self.assertIn("manifest=" + hashlib.sha256(text.encode()).hexdigest(), record)

    def test_the_debug_build_turns_its_log_on(self):
        release = release_zip(self.root / "d.zip", variant="debug")
        text = vpk.write_vpk(release, self.root / "d.vpk")
        self.assertIn("\nvariant debug\n", text)
        self.assertIn("\nset log_file on\n", text)

    def test_what_vitashell_would_warn_of_is_refused(self):
        cases = [("a Shell plugin with vshIoMount", dict(shell_imports=[("SceVshBridge", [0x3C522C35])]), "brick"),
                 ("a kernel plugin with vshIoUmount", dict(kernel_imports=[("SceVshBridge", [0x35BC26AC])]), "brick"),
                 ("a plugin with SceShellSvc", dict(shell_imports=[("SceShellSvc", [1])]), "(a plugin) imports SceShellSvc"),
                 ("a plugin with ScePromoterUtil", dict(kernel_imports=[("ScePromoterUtil", [1])]), "ScePromoterUtil"),
                 ("an app with a mount call", dict(eboot=app_eboot(imports=[("SceVshBridge", [0x3C522C35])])), "brick")]
        for what, kwargs, why in cases:
            with self.subTest(what=what):
                release = release_zip(self.root / "x.zip", **kwargs)
                with self.assertRaises(ValueError) as caught:
                    vpk.write_vpk(release, self.root / "x.vpk")
                self.assertIn(why, str(caught.exception))
                self.assertFalse((self.root / "x.vpk").exists())
        # the Shell's own SceVshBridge import (_vshKernelSearchModuleByName) is fine
        release = release_zip(self.root / "ok.zip", shell_imports=[("SceVshBridge", [0x38612972])])
        vpk.write_vpk(release, self.root / "ok.vpk")
        self.assertEqual(vpk.verify(self.root / "ok.vpk"), [])

    def test_a_damaged_or_foreign_package_is_refused(self):
        with self.assertRaises(ValueError) as caught:
            vpk.write_vpk(release_zip(self.root / "c.zip", corrupt="ur0/tai/VitaJPOverlay_OCR.suprx"), self.root / "c.vpk")
        self.assertIn("not the zip manifest's bytes", str(caught.exception))
        with self.assertRaises(ValueError) as caught:
            vpk.write_vpk(release_zip(self.root / "w.zip", eboot=app_eboot().replace(b"vjoworker=kernel", b"vjoworkerXkernel")),
                          self.root / "w.vpk")
        self.assertIn("vjoworker", str(caught.exception))
        # a VPK edited after the fact: a payload file changed, the order changed, a file added
        out = self.root / "v.vpk"
        vpk.write_vpk(release_zip(self.root / "v.zip"), out)
        with zipfile.ZipFile(out) as z:
            entries = [(i, z.read(i)) for i in z.infolist()]
        for change, why in (("damage", "not the manifest's bytes"), ("reorder", "VitaShell's scan must stop"),
                            ("add", "not the app's list"), ("stamp", "not the app's stamp")):
            edited = self.root / f"{change}.vpk"
            with zipfile.ZipFile(edited, "w", zipfile.ZIP_DEFLATED) as z:
                items = list(entries)
                if change == "reorder":
                    items = items[2:] + items[:2]
                for info, data in items:
                    if change == "damage" and info.filename.endswith("OCR.suprx"):
                        data = data + b"x"
                    if change == "stamp" and info.filename == "vjo_app.txt":
                        data = b"vjoapp1 api=21 build=0000000000000000\n"
                    z.writestr(info.filename, data)
                if change == "add":
                    z.writestr("payload/ux0/data/VitaJPOverlay/games.ini", b"default = enabled\n")
            with self.subTest(change=change):
                self.assertTrue(any(why in p for p in vpk.verify(edited)), vpk.verify(edited))

    def test_the_licences_are_required(self):
        """A release zip without the GPL, the dictionary's attribution or JMdict's licence makes no VPK, and a
        VPK that lacks one, or has another file under licenses/, fails --verify."""
        for missing in ("LICENSE", "docs/plaintext-attribution.txt", "docs/JMdict-LICENSE.html"):
            with self.subTest(missing=missing):
                release = release_zip(self.root / "l.zip", licences={k: v for k, v in LICENCES.items() if k != missing})
                with self.assertRaisesRegex(ValueError, "must carry its licences"):
                    vpk.write_vpk(release, self.root / "l.vpk")
                self.assertFalse((self.root / "l.vpk").exists())
        with self.assertRaisesRegex(ValueError, "not the zip manifest's bytes"):
            vpk.write_vpk(release_zip(self.root / "m.zip", corrupt="docs/JMdict-LICENSE.html"), self.root / "m.vpk")
        good = self.root / "good.vpk"
        vpk.write_vpk(release_zip(self.root / "g.zip"), good)
        for drop, extra in (("licenses/LICENSE", None), (None, "licenses/payload.bin")):
            out = self.root / "bad.vpk"
            with zipfile.ZipFile(good) as src, zipfile.ZipFile(out, "w") as z:
                for info in src.infolist():
                    if info.filename != drop:
                        z.writestr(info, src.read(info))
                if extra:
                    z.writestr(extra, b"x")
            problems = vpk.verify(out)
            self.assertTrue(any(("must carry its licences" if drop else "not a licence") in p for p in problems), problems)

    def test_package_release_records_the_payload_manifest(self):
        """package_release.py writes the payload manifest into the zip (install-vita.sh's installed.txt),
        from the app's own mark: it is the VPK's."""
        import package_release
        release = release_zip(self.root / "base.zip")
        text = vpk.payload_manifest(release)[0]
        for recorded, ok in ((text, True), (text.replace("worker kernel", "worker lines"), False)):
            copy = self.root / f"with-{ok}.zip"
            with zipfile.ZipFile(release) as src, zipfile.ZipFile(copy, "w") as z:
                for info in src.infolist():
                    z.writestr(info, src.read(info))
                z.writestr("payload-manifest.txt", recorded)
            if ok:
                self.assertEqual(vpk.payload_manifest(copy)[0], text)
            else:
                with self.assertRaises(ValueError):
                    vpk.payload_manifest(copy)
        self.assertTrue(hasattr(package_release, "app_info"))


if __name__ == "__main__":
    unittest.main()
