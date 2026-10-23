"""The release package's OCR models and their metadata, with tiny fixture bytes; no models,
network or device access."""
import contextlib
import hashlib
import re
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import prepare_ocr as prepare
import package_release as release


class PackageModels(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.models = self.root / "models"
        self.models.mkdir()
        self.legacy = b"tiny legacy asset fixture, not OCR weights!"
        old_spec = ("https://fixture.invalid/FL10_w8.vocr", len(self.legacy),
                    hashlib.sha256(self.legacy).hexdigest())
        old = {name: old_spec for name in ("FL10_w8.vocr", "H15_w8.vocr")}
        self.vocr3 = b"tiny VOCR3 asset fixture, not OCR weights!!"
        vocr3_spec = ("https://fixture.invalid/VOCR3", len(self.vocr3), hashlib.sha256(self.vocr3).hexdigest())
        vocr3 = {name: vocr3_spec for name in ("VOCR3_H15_w8.vocr", "VOCR3_FL10_w8.vocr")}
        self.lm = b"tiny LM asset fixture, not a language model!"
        lm_spec = ("https://fixture.invalid/LM", len(self.lm), hashlib.sha256(self.lm).hexdigest())
        lms = {name: lm_spec for name in prepare.VOCR_LM_ASSETS}
        vocr4 = {name: vocr3_spec for name in prepare.VOCR4_ASSETS}
        self.vocr5 = b"tiny VOCR5 asset fixture, not OCR weights!!"
        vocr5_spec = ("https://fixture.invalid/VOCR5", len(self.vocr5), hashlib.sha256(self.vocr5).hexdigest())
        vocr5 = {name: vocr5_spec for name in prepare.VOCR5_ASSETS}
        self.attribution = b"CC BY-SA 4.0 fixture"
        att = {prepare.VOCR_LM_ATTRIBUTION: ("https://fixture.invalid/A", len(self.attribution),
                                             hashlib.sha256(self.attribution).hexdigest())}
        self.enterContext(mock.patch.object(release, "VOCR_LM_ATTRIBUTION_PIN", att))
        self.model_attribution = b"the model sizes' attribution fixture"
        model_att = {prepare.VOCR_MODEL_ATTRIBUTION: ("https://fixture.invalid/M", len(self.model_attribution),
                                                      hashlib.sha256(self.model_attribution).hexdigest())}
        self.enterContext(mock.patch.object(release, "VOCR_MODEL_ATTRIBUTION_PIN", model_att))
        # the text-presence gate's weights and attribution (ocr_text_gate)
        self.gate = b"tiny gate fixture, not gate weights"
        self.gate_attribution = b"the gate's attribution fixture"
        self.enterContext(mock.patch.object(release, "VOCR_GATE_ASSETS", {prepare.VOCR_GATE_MODEL: (
            "https://fixture.invalid/G", len(self.gate), hashlib.sha256(self.gate).hexdigest())}))
        self.enterContext(mock.patch.object(release, "VOCR_GATE_ATTRIBUTION_PIN", {prepare.VOCR_GATE_ATTRIBUTION: (
            "https://fixture.invalid/GA", len(self.gate_attribution), hashlib.sha256(self.gate_attribution).hexdigest())}))
        for module in (prepare, release):
            self.enterContext(mock.patch.object(module, "VOCR_ASSETS", old))
            self.enterContext(mock.patch.object(module, "VOCR3_ASSETS", vocr3))
            self.enterContext(mock.patch.object(module, "VOCR_LM_ASSETS", lms))
            self.enterContext(mock.patch.object(module, "VOCR4_ASSETS", vocr4))
            self.enterContext(mock.patch.object(module, "VOCR5_ASSETS", vocr5))
        self.enterContext(mock.patch.object(prepare.urllib.request, "urlopen",
                                          side_effect=AssertionError("No test network access")))

    def fixture_of(self, name):
        """The fixture bytes setUp pins a recognizer's release table to (its asset's own)."""
        if name in prepare.VOCR5_ASSETS:
            return self.vocr5
        if name in prepare.VOCR3_ASSETS or name in prepare.VOCR4_ASSETS:
            return self.vocr3
        return self.legacy

    def write_models(self, attribution=None):
        for name in ("FL10_w8.vocr", "H15_w8.vocr"):  # v0.2.0's, which a package no longer ships
            (self.models / name).write_bytes(self.legacy)
        for name in ("VOCR3_H15_w8.vocr", "VOCR3_FL10_w8.vocr"):
            (self.models / name).write_bytes(self.vocr3)
        for name in prepare.VOCR_LM_ASSETS:
            (self.models / name).write_bytes(self.lm)
        for name in prepare.VOCR4_ASSETS:
            (self.models / name).write_bytes(self.vocr3)
        for name in prepare.VOCR5_ASSETS:  # the model sizes'
            (self.models / name).write_bytes(self.vocr5)
        (self.models / prepare.VOCR_LM_ATTRIBUTION).write_bytes(attribution or self.attribution)
        (self.models / prepare.VOCR_MODEL_ATTRIBUTION).write_bytes(self.model_attribution)
        (self.models / prepare.VOCR_GATE_MODEL).write_bytes(self.gate)
        (self.models / prepare.VOCR_GATE_ATTRIBUTION).write_bytes(self.gate_attribution)

    def package_fixture(self, settings=None, write_models=True, vpk=b"synthetic package fixture", record=None):
        """The release package of a synthetic base archive and the fixture models (written first
        unless write_models is False); returns its path. vpk: the base's Rubi.vpk, record: its
        ux0/data/VitaJPOverlay/vjo_app.txt (None: none, as before the stamps)."""
        if write_models:
            self.write_models()
        dictionary = self.root / "jmdict.vjdict"
        dictionary.write_bytes(b"VJDICT3\0synthetic dictionary fixture")
        snapshot = {"bytes": dictionary.stat().st_size, "sha256": release.digest(dictionary)}
        license_file = self.root / "fixture-license.txt"
        license_file.write_text("Unit-test fixture")
        base = self.root / "base.zip"
        with zipfile.ZipFile(base, "w") as archive:
            for name in ("ur0/tai/VitaJPOverlay_Kernel.skprx", "ur0/tai/VitaJPOverlay_Shell.suprx",
                         "ur0/tai/VitaJPOverlay_OCR.suprx", "ur0/data/VitaJPOverlay/vitajpoverlay.rco"):
                archive.writestr(name, b"synthetic package fixture")
            archive.writestr("Rubi.vpk", vpk)
            if record is not None:
                archive.writestr(release.APP_RECORD, record)
            archive.writestr("config.ini", b"; the build's own template copy\n")
        output = self.root / "package.zip"
        with mock.patch.object(release, "load_manifest", return_value=snapshot), \
             contextlib.redirect_stdout(io.StringIO()):
            release.package(base, output, self.models, dictionary, license_file, settings=settings)
        return output

    @staticmethod
    def app_vpk(eboot, stamp):
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as z:
            z.writestr("eboot.bin", eboot)
            if stamp is not None:
                z.writestr("vjo_app.txt", stamp)
        return buf.getvalue()

    def test_the_manifest_names_the_overlay_app_and_the_record_must_be_its_stamp(self):
        eboot = b"SCE\0 synthetic eboot.bin"
        stamp = f"vjoapp1 api=21 build={hashlib.sha256(eboot).hexdigest()[:16]}\n"
        package = self.package_fixture(vpk=self.app_vpk(eboot, stamp), record=stamp)
        with zipfile.ZipFile(package) as archive:
            manifest = json.loads(archive.read("package-manifest.json"))
            self.assertEqual(archive.read(release.APP_RECORD).decode(), stamp)
        self.assertEqual(manifest["app"], {"stamp": stamp.strip(), "title_id": "VJOSET001", "eboot_bytes": len(eboot),
                                           "eboot_sha256": hashlib.sha256(eboot).hexdigest()})
        self.assertIn(release.APP_RECORD, manifest["files"])  # --verify checks it like every file
        # a base from before the stamps has neither, and no "app"
        manifest = json.loads(zipfile.ZipFile(self.package_fixture()).read("package-manifest.json"))
        self.assertNotIn("app", manifest)
        # the record and the VPK must agree, and the stamp must be the eboot's
        other = f"vjoapp1 api=20 build={hashlib.sha256(eboot).hexdigest()[:16]}\n"
        for vpk, record in ((self.app_vpk(eboot, stamp), other), (self.app_vpk(eboot, stamp), None),
                            (self.app_vpk(eboot, None), stamp), (b"not a zip", stamp),
                            (self.app_vpk(b"another eboot", stamp), stamp),
                            (self.app_vpk(eboot, "garbage\n"), "garbage\n")):
            with self.subTest(record=record), self.assertRaises(ValueError):
                self.package_fixture(vpk=vpk, record=record)

    def test_bundle_ships_the_three_model_sizes(self):
        """API 21: each model size's pinned recognizer (tools/model_tiers.json: VOCR5-S Small,
        VOCR5-N Normal, the default, VOCR5-L Large, each from vita-vn-ocr v0.3.5-headgate.1) and the LMs; the
        earlier generations' (VOCR3's, VOCR4-W20, v0.2.0's), which no setting selects any more, are
        not shipped."""
        package = self.package_fixture()
        tiers = prepare.model_tiers()["tiers"]
        sizes = [t["model"] for t in tiers]
        # three models, or one twice for two adjacent sizes (test_two_sizes_with_one_model_ship_it_once)
        self.assertEqual(len(set(sizes)) + sum(a == b for a, b in zip(sizes, sizes[1:])), 3)
        with zipfile.ZipFile(package) as archive:
            manifest = json.loads(archive.read("package-manifest.json"))
            for name in sizes:
                self.assertIn(release.DATA + "ocr/" + name, manifest["files"])
                self.assertIn(name, manifest["models"])
                self.assertEqual(archive.read(release.DATA + "ocr/" + name), self.fixture_of(name))
            self.assertEqual(sorted(manifest["models"]), sorted({*sizes, *prepare.VOCR_LM_ASSETS, prepare.VOCR_GATE_MODEL}))
            # API 26: the text-presence gate's weights and their release's attribution ship beside the models
            gate = manifest["models"][prepare.VOCR_GATE_MODEL]
            self.assertEqual((gate["release"], gate["bytes"], gate["role"]),
                             ("bee-san/vita-vn-ocr " + prepare.VOCR_GATE_TAG, len(self.gate),
                              "text-presence gate (ocr_text_gate)"))
            self.assertNotIn("generation", gate)
            self.assertEqual(archive.read(release.DATA + "ocr/" + prepare.VOCR_GATE_MODEL), self.gate)
            self.assertEqual(archive.read(release.DATA + "ocr/" + prepare.VOCR_GATE_ATTRIBUTION), self.gate_attribution)
            for name in ("FL10_w8.vocr", "H15_w8.vocr", "VOCR3_FL10_w8.vocr", "VOCR3_H15_w8.vocr", "VOCR4_FL10_w8.vocr",
                         "VOCR4_H15_w8.vocr", "VOCR4_W20_w8.vocr"):
                if name not in sizes:
                    self.assertNotIn(release.DATA + "ocr/" + name, manifest["files"])
            self.assertIn(release.DATA + "ocr/LM-" + prepare.VOCR_LM_ATTRIBUTION, manifest["files"])
            # the model sizes' attribution (v0.3.5-headgate.1's ATTRIBUTION.md) ships beside them, its own bytes
            self.assertEqual(archive.read(release.DATA + "ocr/" + prepare.VOCR_MODEL_ATTRIBUTION), self.model_attribution)
            self.assertEqual(manifest["files"][release.DATA + "ocr/" + prepare.VOCR_MODEL_ATTRIBUTION]["sha256"],
                             hashlib.sha256(self.model_attribution).hexdigest())
            models = manifest["models"]
            roles = ("Small model size", "Normal model size (default)", "Large model size")
            expected = {}
            for name, tier, role in zip(sizes, ("small", "normal", "large"), roles):
                first, ids, names = expected.get(name, (tier, [], []))
                expected[name] = (first, ids + [tier], names + [role])
            self.assertEqual({name: (m.get("tier"), m.get("tiers"), m["role"]) for name, m in models.items() if m.get("tier")},
                             {name: (first, ids, " and ".join(names)) for name, (first, ids, names) in expected.items()})
            for name in sizes:
                self.assertEqual(models[name]["release"], "bee-san/vita-vn-ocr " + prepare.release_assets(name)[1])
                self.assertEqual((models[name]["bytes"], models[name]["sha256"]),
                                 (len(self.fixture_of(name)), hashlib.sha256(self.fixture_of(name)).hexdigest()))
            # VOCR5's sizes come from its head-gated pre-release (API 39; v0.3.0's recognizers with their gate)
            for name in prepare.VOCR5_ASSETS:
                self.assertEqual(models[name]["release"], "bee-san/vita-vn-ocr v0.3.5-headgate.1")
            self.assertEqual(manifest["defaults"]["vocr_tier"], "normal")
            for key in ("vocr_model", "vocr_fallback_model"):
                self.assertNotIn(key, manifest["defaults"])
            self.assertTrue(manifest["defaults"]["log_file"])
            self.assertFalse(manifest["defaults"]["anki"])
            self.assertNotIn("install_settings", manifest)
            # both template copies are the source template: logging on, Anki off, the Normal size
            for name in ("config.ini", release.DATA + "config.ini"):
                text = archive.read(name).decode()
                self.assertIn("\nlog_file = on\n", text)
                self.assertIn("\nanki_mining = off\n", text)
                self.assertIn("\nvocr_tier = normal\n", text)
                self.assertNotIn("vocr_model", text)

    def test_the_shipped_sizes_are_the_tier_table(self):
        """The packager reads its sizes from tools/model_tiers.json: another table's models are what
        it ships, so a later generation's three blobs replace them through the pins."""
        tiers = prepare.model_tiers()
        # each size's model, once (two sizes may have one: test_two_sizes_with_one_model_ship_it_once)
        self.assertEqual(list(dict.fromkeys(t["model"] for t in tiers["tiers"])),
                         [name for name, _, _, role in release.shipped_models() if role.endswith(("size", "(default)"))])
        changed = json.loads(prepare.MODEL_TIERS.read_text())
        changed["tiers"][0]["model"] = "FL10_w8.vocr"  # Small as v0.2.0's FL10, say
        table = self.root / "model_tiers.json"
        table.write_text(json.dumps(changed))
        with mock.patch.object(prepare, "MODEL_TIERS", table):
            names = [name for name, _, _, _ in release.shipped_models()]
            sizes = list(dict.fromkeys(["FL10_w8.vocr", tiers["tiers"][1]["model"], tiers["tiers"][2]["model"]]))
            self.assertEqual(names[:len(sizes)], sizes)
            # a tier whose new candidate did not beat its incumbent keeps that model: a pin edit
            changed["tiers"][2]["model"] = "VOCR4_W20_w8.vocr"
            table.write_text(json.dumps(changed))
            shipped = {name: (tag, role) for name, _, tag, role in release.shipped_models()}
            role = ("Normal model size (default) and Large model size"  # Normal may be VOCR4-W20 too: one model
                    if changed["tiers"][1]["model"] == "VOCR4_W20_w8.vocr" else "Large model size")
            self.assertEqual(shipped["VOCR4_W20_w8.vocr"], ("v0.2.5-vocr4.1", role))
            # a model no release table pins is refused
            changed["tiers"][2]["model"] = "VOCR9_L_w8.vocr"
            table.write_text(json.dumps(changed))
            with self.assertRaisesRegex(ValueError, "VOCR9_L_w8.vocr is not a pinned recognizer"):
                release.shipped_models()
            # and so is a table without the three sizes, or another default
            changed["tiers"] = changed["tiers"][:2]
            table.write_text(json.dumps(changed))
            with self.assertRaises(ValueError):
                release.shipped_models()

    def test_the_model_sizes_attribution_is_required_and_pinned(self):
        """v0.3.0's ATTRIBUTION.md (the licences of what VOCR5 was trained on, CC BY 4.0 data among them, and
        the weights' terms) ships as ocr/MODEL-ATTRIBUTION.md: a package without it, or with another file
        under its name (the LMs' ATTRIBUTION.md, say), is refused."""
        self.write_models()
        (self.models / prepare.VOCR_MODEL_ATTRIBUTION).unlink()
        with self.assertRaisesRegex(ValueError, "MODEL-ATTRIBUTION.md is missing"):
            self.package_fixture(write_models=False)
        (self.models / prepare.VOCR_MODEL_ATTRIBUTION).write_bytes(self.attribution)  # the LMs' text
        with self.assertRaisesRegex(ValueError, "MODEL-ATTRIBUTION.md checksum failed"):
            self.package_fixture(write_models=False)
        self.assertFalse((self.root / "package.zip").exists())

    def test_the_gates_weights_and_attribution_are_required_and_pinned(self):
        """API 26: the text-presence gate's weights (v0.3.2-textgate.1's textgate_p8_c8_w8.vtd, read at run time
        when ocr_text_gate = on) and that release's ATTRIBUTION.md (its training data's licences, the weights'
        non-commercial terms) ship as ocr/GATE-ATTRIBUTION.md: a package without either, or with another file
        under its name, is refused."""
        for name, other in ((prepare.VOCR_GATE_MODEL, b"another gate"), (prepare.VOCR_GATE_ATTRIBUTION, self.attribution)):
            self.write_models()
            (self.models / name).unlink()
            with self.assertRaisesRegex(ValueError, re.escape(name) + ".* missing|missing: " + re.escape(name)):
                self.package_fixture(write_models=False)
            (self.models / name).write_bytes(other)
            with self.assertRaisesRegex(ValueError, "checksum failed"):
                self.package_fixture(write_models=False)
            self.assertFalse((self.root / "package.zip").exists())

    def test_a_payload_list_the_app_refuses_leaves_no_package(self):
        """The one-VPK payload list is checked before the output is opened, and a refusal while writing
        removes what was written: a list the Rubi app would refuse (a file named twice,
        package_vpk.payload_files) stops the packaging with no partial zip behind."""
        import package_vpk  # noqa: PLC0415
        facts = ("0.8.0", "normal", "kernel", 23, "0123456789abcdef")
        output = self.root / "package.zip"
        output.write_bytes(b"an earlier package")
        with mock.patch.object(package_vpk, "app_facts", return_value=facts), \
             mock.patch.object(package_vpk, "payload_files", side_effect=ValueError("names a file twice")):
            with self.assertRaisesRegex(ValueError, "names a file twice"):
                self.package_fixture()
        self.assertEqual(output.read_bytes(), b"an earlier package")  # refused before the output is opened
        output.unlink()
        # a refusal from inside the payload manifest's writing too (a payload file the zip lacks): what was
        # written is removed
        with mock.patch.object(package_vpk, "app_facts", return_value=facts), \
             mock.patch.object(package_vpk, "build_manifest", side_effect=ValueError("the package has no x")):
            with self.assertRaisesRegex(ValueError, "the package has no x"):
                self.package_fixture()
        self.assertFalse((self.root / "package.zip").exists())

    def test_two_sizes_with_one_model_ship_it_once(self):
        """vita-vn-ocr eval/vocr5_protocol.json amendment 3: Normal is the better of VOCR5-N and
        VOCR4-W20, which Large may keep. Normal and Large then have one model: the package ships it
        once, its manifest entry naming both sizes (tier: the smaller, tiers: both), and the default
        engine is its generation."""
        changed = json.loads(prepare.MODEL_TIERS.read_text())
        changed["tiers"][1]["model"] = changed["tiers"][2]["model"] = "VOCR4_W20_w8.vocr"
        table = self.root / "model_tiers.json"
        table.write_text(json.dumps(changed))
        with mock.patch.object(prepare, "MODEL_TIERS", table):
            shipped = [(name, tag, role) for name, _, tag, role in release.shipped_models()]
            self.assertEqual(shipped[:2], [(changed["tiers"][0]["model"], prepare.release_assets(changed["tiers"][0]["model"])[1],
                                            "Small model size"),
                                           ("VOCR4_W20_w8.vocr", "v0.2.5-vocr4.1",
                                            "Normal model size (default) and Large model size")])
            self.assertEqual([name for name, _, _ in shipped].count("VOCR4_W20_w8.vocr"), 1)
            with zipfile.ZipFile(self.package_fixture()) as archive:
                manifest = json.loads(archive.read("package-manifest.json"))
                names = archive.namelist()
        self.assertEqual(names.count(release.DATA + "ocr/VOCR4_W20_w8.vocr"), 1)
        entry = manifest["models"]["VOCR4_W20_w8.vocr"]
        self.assertEqual((entry["tier"], entry["tiers"], entry["role"], entry["generation"]),
                         ("normal", ["normal", "large"], "Normal model size (default) and Large model size", "VOCR4"))
        self.assertEqual((manifest["defaults"]["vocr_tier"], manifest["defaults"]["ocr_engine"]), ("normal", "VOCR4"))
        self.assertEqual(sum(1 for m in manifest["models"].values() if m.get("tier")), 2)

    def test_a_package_may_start_with_another_size(self):
        package = self.package_fixture({"vocr_tier": "large"})
        with zipfile.ZipFile(package) as archive:
            manifest = json.loads(archive.read("package-manifest.json"))
            self.assertIn("\nvocr_tier = large\n", archive.read("config.ini").decode())
        large = prepare.model_tiers()["tiers"][2]["model"]
        self.assertEqual((manifest["defaults"]["vocr_tier"], manifest["defaults"]["ocr_engine"]),
                         ("large", prepare.VOCR_GENERATION[prepare.release_assets(large)[1]]))
        self.assertEqual(manifest["install_settings"], {"vocr_tier": "large"})
        with self.assertRaisesRegex(ValueError, "'huge' is not a model size"):
            self.package_fixture({"vocr_tier": "huge"})

    def test_manifest_names_each_model_generation_and_the_local_engine(self):
        """The bundled metadata the Rubi app's label comes from: each model's generation (by its
        release), and the local engine's name for the package's default size, VOCR5."""
        package = self.package_fixture()
        with zipfile.ZipFile(package) as archive:
            manifest = json.loads(archive.read("package-manifest.json"))
        generation = {t["model"]: prepare.VOCR_GENERATION[prepare.release_assets(t["model"])[1]]
                      for t in prepare.model_tiers()["tiers"]}
        self.assertEqual({name: entry["generation"] for name, entry in manifest["models"].items() if "generation" in entry},
                         generation)
        normal = prepare.model_tiers()["tiers"][1]["model"]
        for name in prepare.VOCR_LM_ASSETS:  # an LM is no recognizer: no generation
            self.assertNotIn("generation", manifest["models"][name])
        self.assertNotIn("ocr_backend", manifest["defaults"])  # no engine choice: VOCR is the only engine
        self.assertEqual(manifest["defaults"]["ocr_engine"], generation[normal])
        default = next(n for n, m in manifest["models"].items() if m.get("tier") == manifest["defaults"]["vocr_tier"])
        self.assertEqual(manifest["defaults"]["ocr_engine"], manifest["models"][default]["generation"])
        # a release's generation reaches the manifest through its tag, and only that release's models
        with mock.patch.dict(prepare.VOCR_GENERATION, {prepare.VOCR5_TAG: "VOCR6", prepare.VOCR3_TAG: "VOCR9"}), \
             mock.patch.dict(release.VOCR_GENERATION, {prepare.VOCR5_TAG: "VOCR6", prepare.VOCR3_TAG: "VOCR9"}):
            (self.root / "package.zip").unlink()
            package = self.package_fixture()
            with zipfile.ZipFile(package) as archive:
                manifest = json.loads(archive.read("package-manifest.json"))
        moved = {"VOCR5": "VOCR6"}  # each size's model takes its own release's generation
        self.assertEqual(manifest["defaults"]["ocr_engine"], moved.get(generation[normal], generation[normal]))
        for name, g in generation.items():
            self.assertEqual(manifest["models"][name]["generation"], moved.get(g, g), name)
        self.assertNotIn("VOCR9", json.dumps(manifest))  # VOCR3's release ships nothing

    def test_removed_vocr2_is_neither_shipped_nor_claimed(self):
        # a VOCR2 file left in the models directory is not packaged
        (self.models / "VOCR2_FL10_w8.vocr").write_bytes(self.legacy)
        package = self.package_fixture()
        with zipfile.ZipFile(package) as archive:
            manifest = json.loads(archive.read("package-manifest.json"))
            self.assertFalse([n for n in archive.namelist() if "VOCR2" in n])
        self.assertNotIn("ocr_backends", manifest)
        self.assertNotIn("VOCR2", json.dumps(manifest))
        for name in ("VOCR2_ENABLED", "VOCR2_MODEL", "VOCR2_TAG", "VOCR2_ASSETS", "fetch_vocr2"):
            self.assertFalse(hasattr(prepare, name), name)
            self.assertFalse(hasattr(release, name), name)

    def test_package_settings_go_into_both_templates_and_the_manifest(self):
        package = self.package_fixture({"anki_mining": "on", "log_file": "on"})
        with zipfile.ZipFile(package) as archive:
            manifest = json.loads(archive.read("package-manifest.json"))
            self.assertEqual(manifest["install_settings"], {"anki_mining": "on", "log_file": "on"})
            self.assertTrue(manifest["defaults"]["anki"])
            for name in ("config.ini", release.DATA + "config.ini"):
                text = archive.read(name).decode()
                self.assertIn("\nanki_mining = on\n", text)
                self.assertNotIn("anki_mining = off", text)
                self.assertEqual(manifest["files"][name]["sha256"], hashlib.sha256(text.encode()).hexdigest())
        # a key the template does not have, or a value with a line break, is refused before output
        with self.assertRaisesRegex(ValueError, "Not a setting of the config template: anki_minnig"):
            self.package_fixture({"anki_minnig": "on"})
        for bad in ("anki_mining", "anki_mining=on\nlog_file=off", "=on", "Bad Key=1", "anki_deck=a\u2028b",
                    "anki_deck=a\x85b", "anki_deck=a\x7fb"):
            with self.assertRaises(Exception):
                release.parse_setting(bad)
        self.assertEqual(release.parse_setting(" Anki_Mining = on "), ("anki_mining", "on"))
        self.assertEqual(release.parse_setting("anki_field_picture="), ("anki_field_picture", ""))

    def manifest(self, settings=None):
        with zipfile.ZipFile(self.package_fixture(settings)) as archive:
            return json.loads(archive.read("package-manifest.json")), archive.read("config.ini").decode()

    def test_the_manifest_defaults_are_the_config_templates_settings(self):
        """audit-test-build-2 N4: the defaults hold every setting of the config.ini template, each with the
        template's value (a switch as true/false, a number as an integer), so a setting added to the
        template is in the next manifest without another edit; the API 20 job keys included."""
        manifest, shipped_template = self.manifest()
        template = release.template_settings((ROOT / "docs/config.example.ini").read_text(encoding="utf-8"))
        self.assertEqual(release.template_settings(shipped_template), template)
        defaults = manifest["defaults"]
        self.assertEqual(set(defaults) - {"ocr_engine", "anki"}, set(template))
        self.assertEqual(list(defaults)[:len(template)], list(template))  # in the template's order
        for key, value in template.items():
            got = defaults[key]
            if key in release.SWITCHES:
                self.assertIs(got, {"on": True, "off": False}[value], key)
            elif key in release.NUMBERS:
                self.assertEqual(got, int(value), key)
            elif release.SECRET.search(key):
                self.assertEqual(got, "set" if value else "", key)
            else:
                self.assertEqual(got, value, key)
        # every setting the template has is classified as core/config.c parses it
        self.assertEqual({k for k in template if template[k] in ("on", "off") and k != "ocr_refine_lines"},
                         set(release.SWITCHES))
        self.assertEqual({k for k in template if template[k].isdigit()}, set(release.NUMBERS))
        # the API 20 job keys, as the template and core/config.c's defaults have them
        self.assertEqual({k: defaults[k] for k in ("ocr_settle_ms", "ocr_settle_max_ms", "ocr_skip_blank_lines",
                                                   "ocr_refine_lines", "ocr_lm", "ocr_cascade_ms")},
                         {"ocr_settle_ms": 150, "ocr_settle_max_ms": 2000, "ocr_skip_blank_lines": False,
                          "ocr_refine_lines": "psp", "ocr_lm": "1000k", "ocr_cascade_ms": 2000})
        # what the manifest said before is still said the same way, but the engine choice, which
        # went with Google Lens (the template has no ocr_backend line)
        self.assertNotIn("ocr_backend", defaults)
        self.assertEqual({k: defaults[k] for k in ("ocr_engine", "vocr_tier",
                                                   "dictionary", "local_dictionaries", "japanese_only_selection",
                                                   "ocr_mode", "log_file", "anki")},
                         {"ocr_engine": prepare.VOCR_GENERATION[prepare.release_assets(
                             prepare.model_tiers()["tiers"][1]["model"])[1]], "vocr_tier": "normal",
                          "dictionary": "local",
                          "local_dictionaries": "plaintext.vjdict,vndb-names.vjdict", "japanese_only_selection": False,
                          "ocr_mode": "auto", "log_file": True, "anki": False})

    def test_package_settings_reach_the_defaults_and_no_credential_is_one(self):
        manifest, template = self.manifest({"anki_mining": "on", "highlight_mode": "character", "ocr_settle_ms": "0",
                                            "ocr_refine_lines": "off", "ocr_skip_blank_lines": "on"})
        defaults = manifest["defaults"]
        self.assertEqual({k: defaults[k] for k in ("anki_mining", "anki", "highlight_mode", "ocr_settle_ms",
                                                   "ocr_refine_lines", "ocr_skip_blank_lines")},
                         {"anki_mining": True, "anki": True, "highlight_mode": "character", "ocr_settle_ms": 0,
                          "ocr_refine_lines": "off", "ocr_skip_blank_lines": True})
        self.assertEqual(manifest["install_settings"], {"anki_mining": "on", "highlight_mode": "character",
                                                        "ocr_settle_ms": "0", "ocr_refine_lines": "off",
                                                        "ocr_skip_blank_lines": "on"})
        # audit-overlay-test3-cleanup B1, after the removals: the template has no credential left
        # (jpdb.io's and jiten.moe's API keys went with them, anki_audio_url, whose URL can carry a
        # key, with AnkiConnect): each is refused before any output, so none can reach the manifest
        self.assertNotIn("api_key", template)
        self.assertNotIn("anki_audio_url", template)
        for key in ("jpdb_api_key", "jiten_api_key", "api_key", "anki_audio_url"):
            with self.assertRaisesRegex(ValueError, "Not a setting of the config template: " + key):
                self.package_fixture({key: "https://a.invalid/audio?term={term}&key=fixture-secret-key"})
        # a value core/config.c would not read as its type is refused before any output
        for bad in ({"log_file": "yes"}, {"ocr_cascade_ms": "2s"}):
            with self.assertRaises(ValueError):
                self.package_fixture(bad)
        with self.assertRaisesRegex(ValueError, "sets ocr_lm twice"):
            release.template_settings("ocr_lm = 1000k\nocr_lm = off\n")
        with self.assertRaisesRegex(ValueError, "not one of the package's recognizers"):
            release.manifest_defaults("vocr_tier = normal\n", {})
        with self.assertRaisesRegex(ValueError, "not a model size"):
            release.manifest_defaults("vocr_tier = huge\n", {"VOCR3_H15_w8.vocr": {"generation": "VOCR3"}})

    def test_bundle_refuses_an_attribution_that_is_not_the_pinned_one(self):
        # Another release's ATTRIBUTION.md (ctclm.1's lacks the protected-title paragraph) is refused.
        for other in (b"CC BY-SA 4.0 fixture of an older release", b"CC BY-SA 4.0 fixturE"):  # its size; the same size
            self.write_models(attribution=other)
            with self.assertRaisesRegex(ValueError, "ATTRIBUTION.md checksum failed"):
                self.package_fixture(write_models=False)
            self.assertFalse((self.root / "package.zip").exists())

    def test_missing_or_wrong_model_is_refused_before_output(self):
        self.write_models()
        small = prepare.model_tiers()["tiers"][0]["model"]
        (self.models / small).unlink()
        with self.assertRaisesRegex(ValueError, "OCR model is missing: " + small):
            self.package_fixture(write_models=False)
        self.assertFalse((self.root / "package.zip").exists())
        wrong = bytearray(self.fixture_of(small))
        wrong[0] ^= 1
        (self.models / small).write_bytes(bytes(wrong))  # the right size, another hash
        with self.assertRaisesRegex(ValueError, "OCR model checksum failed: " + small):
            self.package_fixture(write_models=False)
        self.assertFalse((self.root / "package.zip").exists())

if __name__ == "__main__":
    unittest.main()
