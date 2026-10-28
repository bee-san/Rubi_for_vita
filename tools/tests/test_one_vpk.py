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
import unittest.mock
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools/tests"))
import package_vpk as vpk  # noqa: E402
import prepare_ocr  # noqa: E402  (API 54: the text spotter's pinned name)
from self_fixture import Segment, fself  # noqa: E402
# the tree's API (include/vjo_api.h): the app's stamp in these fixtures
API = int(re.search(r"#define VJO_API_VERSION (\d+)", (ROOT / "include/vjo_api.h").read_text()).group(1))


def kernel_index():
    """include/vjo_payload.h's VJO_PAYLOAD_KERNEL_INDEX, (15 + VJO_PAYLOAD_SPOT_FILES): API 54's text spotter adds its
    two files before the plugins once its pin is published (include/vjo_vocr_sizes.h VJO_VOCR_SPOT_PUBLISHED)."""
    header = (ROOT / "include/vjo_payload.h").read_text()
    base = int(re.search(r"#define VJO_PAYLOAD_KERNEL_INDEX \((\d+) \+ VJO_PAYLOAD_SPOT_FILES\)", header).group(1))
    return base + (2 if prepare_ocr.VOCR_SPOT_PUBLISHED else 0)


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
    return fself(Segment(bytes(segment), vaddr=base))  # e_entry 0: the module info at the segment's start


def app_eboot(version="0.8.0", debug=False, worker="kernel", imports=(("SceShellUtil", [0x7C79BDD7]),)):
    start = b"start: v" + version.encode() + (b" debug" if debug else b"") + b"; %s; heap %d B"
    return make_self(imports, b"\0" + start + b"\0vjoworker=" + worker.encode() + b"\0")


def app_vpk(eboot):
    stamp = f"vjoapp1 api={API} build={hashlib.sha256(eboot).hexdigest()[:16]}\n"
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("sce_sys/param.sfo", b"\0PSF fixture")
        z.writestr("eboot.bin", eboot)
        z.writestr("sce_sys/icon0.png", b"png")
        z.writestr("vjo_app.txt", stamp)
    return out.getvalue(), stamp


LICENCES = {"LICENSE": b"GPL-3.0 fixture\n", "third_party/README.md": b"third-party index\n",
            "third_party/licenses/BearSSL-MIT.txt": b"MIT fixture\n", "docs/plaintext-attribution.txt": b"CC BY-SA 4.0\n",
            "docs/JMdict-LICENSE.html": b"<html>JMdict</html>\n",
            **{name: b"VNDB licence fixture\n" for name in vpk.REQUIRED_LICENSES[3:]}}


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
        z.writestr("Rubi.vpk", app)
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
        import package_release
        files = vpk.payload_files()
        # the bundled PSP/Vita names are a separate payload file; API 47: VOCR6-V and VERTICAL-ATTRIBUTION.md;
        # the SC3 hooks' three charsets (docs/native-hooks.md "SC3 text"); VOCR6 (every size since v0.4.1-vocr6.2):
        # VOCR6-ATTRIBUTION.md
        # API 54: the text spotter's two once published
        spot = 2 if package_release.VOCR_SPOT_PUBLISHED else 0
        self.assertEqual(len(files), 25 + spot)
        self.assertEqual(sorted(d for d, _ in files if "/hooks/" in d),
                         ["ux0:data/VitaJPOverlay/hooks/%s.sc3" % t for t in ("PCSG00146", "PCSG00352", "PCSG00555")]
                         + ["ux0:data/VitaJPOverlay/hooks/profiles.vjhp"])
        self.assertTrue(all(k == "copy" for d, k in files if "/hooks/" in d))
        header = (ROOT / "include/vjo_payload.h").read_text()
        self.assertRegex(header, r"#define VJO_PAYLOAD_SPOT_FILES 2\n#else\n#define VJO_PAYLOAD_SPOT_LIST\(X\)\n"
                                 r"#define VJO_PAYLOAD_SPOT_FILES 0\n")
        kernel = kernel_index()
        self.assertEqual(kernel, 20 + spot)
        self.assertEqual(files[kernel], ("ur0:tai/VitaJPOverlay_Kernel.skprx", "copy"))
        self.assertEqual(files[kernel - 1], ("ux0:data/VitaJPOverlay/dictionaries/plaintext.vjdict", "move"))
        self.assertEqual([d for d, _ in files[kernel:kernel + 4]],  # VJO_PAYLOAD_PLUGINS: the plugins and the RCO
                         ["ur0:tai/VitaJPOverlay_Kernel.skprx", "ur0:tai/VitaJPOverlay_Shell.suprx",
                          "ur0:tai/VitaJPOverlay_OCR.suprx", "ur0:data/VitaJPOverlay/vitajpoverlay.rco"])
        self.assertEqual(files[-1], ("ux0:data/VitaJPOverlay/config.ini", "template"))
        # the models are the package's shipped ones (package_release.py shipped_models), nothing else
        shipped = {name for name, *_ in package_release.shipped_models()} | {"LM-ATTRIBUTION.md",
                                                                            package_release.VOCR_MODEL_ATTRIBUTION,
                                                                            package_release.VOCR6_ATTRIBUTION,
                                                                            package_release.VOCR_GATE_ATTRIBUTION,
                                                                            package_release.VOCR_VERTICAL_ATTRIBUTION}
        if package_release.VOCR_SPOT_PUBLISHED:
            shipped.add(package_release.VOCR_SPOT_ATTRIBUTION)
        self.assertEqual({d.rsplit("/", 1)[1] for d, _ in files if "/ocr/" in d}, shipped)
        # the app's own record is never in the payload (the install writes it, core/app_stamp.h)
        self.assertNotIn("ux0:data/VitaJPOverlay/vjo_app.txt", [d for d, _ in files])

    def test_the_spotter_joins_the_payload_once_published(self):
        """API 54: VJO_VOCR_SPOT_PUBLISHED 1 (include/vjo_vocr_sizes.h) puts the spotter's weights and attribution
        after the vertical model's, as the compiler's #if branch does; the placeholder's 0 leaves them out."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "include").mkdir()
            (root / "include/vjo_payload.h").write_text((ROOT / "include/vjo_payload.h").read_text())
            sizes = (ROOT / "include/vjo_vocr_sizes.h").read_text()
            for flag, extra in (("0", []), ("1", ["ux0:data/VitaJPOverlay/ocr/" + prepare_ocr.VOCR_SPOT_MODEL,
                                                  "ux0:data/VitaJPOverlay/ocr/SPOTTER-ATTRIBUTION.md"])):
                (root / "include/vjo_vocr_sizes.h").write_text(
                    re.sub(r"^#define VJO_VOCR_SPOT_PUBLISHED [01]$", "#define VJO_VOCR_SPOT_PUBLISHED " + flag, sizes,
                           flags=re.M))
                with unittest.mock.patch.object(vpk, "ROOT", root):
                    files = [d for d, _ in vpk.payload_files()]
                vert = files.index("ux0:data/VitaJPOverlay/ocr/VERTICAL-ATTRIBUTION.md")  # API 47's, after the gate's
                self.assertEqual(files[vert + 1:vert + 1 + len(extra)], extra)
                # then the SC3 hooks' charsets (docs/native-hooks.md "SC3 text")
                self.assertEqual(files[vert + 1 + len(extra)], "ux0:data/VitaJPOverlay/hooks/profiles.vjhp")
                self.assertEqual(files[vert + 2 + len(extra)], "ux0:data/VitaJPOverlay/hooks/PCSG00146.sc3")
                self.assertEqual(len(files), 25 + len(extra))

    def test_a_file_named_twice_is_refused(self):
        """core/payload.c refuses a manifest naming a file twice ("a file named twice"), so the packaging
        refuses the list first: two model sizes pinned to one model (Normal and Large both VOCR4-W20, which
        vita-vn-ocr's VOCR5 protocol allows) would otherwise make a VPK the app cannot install."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "include").mkdir()
            header = (ROOT / "include/vjo_payload.h").read_text()
            if "VJO_VOCR_TIER3_NAME" not in header:  # a tree that already names its shared model once: list it twice
                header = header.replace("X(VJO_PAYLOAD_OCR VJO_VOCR_TIER2_NAME, VJO_PAYLOAD_COPY)",
                                        "X(VJO_PAYLOAD_OCR VJO_VOCR_TIER2_NAME, VJO_PAYLOAD_COPY) "
                                        "X(VJO_PAYLOAD_OCR VJO_VOCR_TIER3_NAME, VJO_PAYLOAD_COPY)", 1)
            (root / "include/vjo_payload.h").write_text(header)
            sizes = (ROOT / "include/vjo_vocr_sizes.h").read_text()
            shared = re.sub(r"#define VJO_VOCR_TIER3_NAME \w+", "#define VJO_VOCR_TIER3_NAME VJO_VOCR4_W20_NAME",
                            re.sub(r"#define VJO_VOCR_TIER2_NAME \w+", "#define VJO_VOCR_TIER2_NAME VJO_VOCR4_W20_NAME", sizes))
            # Normal and Large one model, whatever this tree's pins (a tree that shares already reads the same)
            for tier in (2, 3):
                self.assertIn(f"#define VJO_VOCR_TIER{tier}_NAME VJO_VOCR4_W20_NAME", shared)
            (root / "include/vjo_vocr_sizes.h").write_text(shared)
            with unittest.mock.patch.object(vpk, "ROOT", root):
                with self.assertRaisesRegex(ValueError, "VOCR4_W20_w8.vocr twice"):
                    vpk.payload_files()


class Package(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def test_a_vpk_of_a_release_zip(self):
        release = release_zip(self.root / "r.zip", settings={"anki_mining": "on"})
        out = self.root / "Rubi-0.8.0.vpk"
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
        self.assertEqual(lines[:7], ["vjopayload 1", "version 0.8.0", "app_version 0.8.0", "variant normal", f"api {API}",
                                     "worker kernel", "build " + lines[6].split()[1]])
        self.assertIn("set anki_mining on", lines)
        self.assertNotIn("set log_file on", lines)
        kernel = kernel_index()
        self.assertTrue(re.fullmatch(r"file ur0:tai/VitaJPOverlay_Kernel\.skprx \d+ [0-9a-f]{64} copy", lines[7 + kernel]))
        record = vpk.installed_record(text)
        self.assertRegex(record, rf"^vjoinst1 version=0\.8\.0 variant=normal api={API} build=[0-9a-f]{{16}} manifest=[0-9a-f]{{64}}\n$")
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

    def test_verify_is_as_strict_as_the_app(self):
        """--verify refuses what core/payload.c refuses: another kind, an unknown or reordered line, a setting
        the app does not set; and a version the file name does not have (code audit note 4)."""
        good = self.root / "Rubi-0.8.0.vpk"
        vpk.write_vpk(release_zip(self.root / "g.zip"), good)
        self.assertEqual(vpk.verify(good), [])
        text = zipfile.ZipFile(good).read("payload/manifest.txt").decode()
        dictionary = next(l for l in text.splitlines() if "plaintext.vjdict" in l)
        # each change is the one named: count=1, as "version 0.8.0\n" is also the end of "app_version 0.8.0\n"
        # (code audit note 3, N22: the version case changed both lines, so app_version's check refused it and
        # the file-name check was never reached)
        self.assertEqual(text.count("version 0.8.0\n"), 2)
        cases = [("a kind", text.replace(dictionary, dictionary.replace(" move", " copy")), "is not the one"),
                 # an added valid setting at the end is what a package with it carries: only the release's
                 # SHA256SUMS tells them apart; one where this tool never writes it is refused
                 ("a set line out of place", text.replace("version 0.8.0\n", "version 0.8.0\nset anki_mining on\n", 1),
                  "is not the one"),
                 ("a setting the app does not set", text + "set games on\n", "not a setting the app accepts"),
                 ("an unknown line", text + "colour red\n", "is not the one"),
                 ("a version", text.replace("version 0.8.0\n", "version 9.9.9\n", 1),
                  "the file is named for version 0.8.0, the manifest says 9.9.9")]
        for what, changed, why in cases:
            with self.subTest(what=what):
                out = self.root / "Rubi-0.8.0.vpk.bad"
                with zipfile.ZipFile(good) as src, zipfile.ZipFile(out, "w") as z:
                    for info in src.infolist():
                        z.writestr(info, changed if info.filename == "payload/manifest.txt" else src.read(info))
                named = out.with_name("Rubi-0.8.0.vpk")
                good.rename(self.root / "keep.vpk")
                out.rename(named)
                try:
                    problems = vpk.verify(named)
                    self.assertTrue(any(why in p for p in problems), (what, problems))
                    if what == "a version":  # only the version line differs: app_version is the app's
                        self.assertEqual(len(problems), 1, problems)
                finally:
                    named.unlink()
                    (self.root / "keep.vpk").rename(good)

    def test_the_licences_are_required(self):
        """A release zip without the GPL, the dictionary's attribution or JMdict's licence makes no VPK, and a
        VPK that lacks one, or has another file under licenses/, fails --verify."""
        for missing in vpk.REQUIRED_LICENSES:
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
