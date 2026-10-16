#!/usr/bin/env python3
"""Combine tested plugins/settings VPK with pinned OCR and frozen dictionaries."""
import argparse
import hashlib
import io
import json
import re
from pathlib import Path
import shutil
import tempfile
import zipfile

from prepare_ocr import (VOCR_ASSETS, VOCR_GENERATION, VOCR_LM_ASSETS, VOCR_LM_ATTRIBUTION, VOCR_LM_ATTRIBUTION_PIN,
                         VOCR_LM_TAG, VOCR_TAG, VOCR3_ASSETS, VOCR3_TAG, VOCR4_ASSETS, VOCR4_TAG, model_tiers)
from prepare_plaintext import FILENAME, load_manifest, validate

DATA = "ux0/data/VitaJPOverlay/"
SETTING_KEY = re.compile(r"[a-z][a-z0-9_]*")
# The config template's on | off switches and its numbers, as core/config.c parses them: the manifest's
# defaults give them as true/false and as integers, and every other setting as the template writes it.
SWITCHES = ("japanese_only_selection", "frequency_badge", "ocr_skip_blank_lines", "ocr_boost_clock", "log_file",
            "anki_mining", "anki_screenshot")
NUMBERS = ("local_dictionary_results", "stick_lookup_ms", "font_size_ja", "font_size_en", "ocr_settle_ms",
           "ocr_settle_max_ms", "ocr_settle_press_ms", "ocr_cascade_ms", "ocr_threads")
# Settings that can hold a credential (an audio source URL can carry one). The manifest gives them only as
# "set" or "", in its defaults and its install_settings: install_ftp.sh --collect copies both into the zip
# the owner sends. install-vita.sh takes a set one's value from the package's config.ini, which holds it.
# tools/install-vita.sh, migrate_config.py, collect_vita.py and analyse_selftest.py use the same pattern.
SECRET = re.compile(r"api_key$|^anki_audio_url$")
APP_RECORD = DATA + "vjo_app.txt"  # the record of the Overlay app these plugins come with (core/app_stamp.h)
APP_STAMP = re.compile(r"vjoapp1 api=\d{1,6} build=[0-9a-f]{16}")


def app_info(original):
    """The base zip's Overlay app: {"stamp", "title_id", "eboot_bytes", "eboot_sha256"} from JPOverlaySettings.vpk's
    vjo_app.txt and eboot.bin, which must equal the base's ux0/data/VitaJPOverlay/vjo_app.txt (the Vita build writes
    both from one file); None for a base from before the stamps, which has neither."""
    names = set(original.namelist())
    record = original.read(APP_RECORD).decode("utf-8", "replace") if APP_RECORD in names else None
    stamp = eboot = None
    try:
        with zipfile.ZipFile(io.BytesIO(original.read("JPOverlaySettings.vpk"))) as vpk:
            vpk_names = set(vpk.namelist())
            stamp = vpk.read("vjo_app.txt").decode("utf-8", "replace") if "vjo_app.txt" in vpk_names else None
            eboot = vpk.read("eboot.bin") if "eboot.bin" in vpk_names else None
    except zipfile.BadZipFile:
        pass  # not a VPK that can carry a stamp: an error below only when the base has a record
    if record is None and stamp is None:
        return None
    if record is None or stamp is None or record != stamp or not APP_STAMP.fullmatch(stamp.strip()):
        raise ValueError("The Overlay app's stamp: " + APP_RECORD + " and JPOverlaySettings.vpk's vjo_app.txt must be "
                         "one stamp line (vita/CMakeLists.txt writes both)")
    if eboot is None or hashlib.sha256(eboot).hexdigest()[:16] != stamp.split("build=")[1].strip():
        raise ValueError("The Overlay app's stamp is not its VPK's eboot.bin's")
    return {"stamp": stamp.strip(), "title_id": "VJOSET001", "eboot_bytes": len(eboot),
            "eboot_sha256": hashlib.sha256(eboot).hexdigest()}


VARIANTS = ("debug", "normal")  # docs/release-variants.md: the debug build (the test builds') and the normal one


def base_variant(original):
    """(variant, app version) of a base zip's modules (tools/check_variant.py): "debug" or "normal",
    from the Shell's and the kernel's strings and the Overlay app's start line; ("debug", None) for a
    base from before the two builds, which are all the debug build; (None, None) when its plugins are
    not Vita SELFs (the tests' placeholders). Raises when the modules disagree."""
    from check_variant import app_version, classify, segments  # packaging only: --verify needs neither
    blobs = {}
    with tempfile.TemporaryDirectory(prefix="vjo-variant-") as tmp:
        try:
            for part, name in (("kernel", "ur0/tai/VitaJPOverlay_Kernel.skprx"),
                               ("shell", "ur0/tai/VitaJPOverlay_Shell.suprx")):
                path = Path(tmp) / part
                path.write_bytes(original.read(name))
                blobs[part] = segments(path)
        except ValueError:
            return None, None
        try:
            with zipfile.ZipFile(io.BytesIO(original.read("JPOverlaySettings.vpk"))) as vpk:
                path = Path(tmp) / "eboot.bin"
                path.write_bytes(vpk.read("eboot.bin"))
                blobs["app"] = segments(path)
        except (KeyError, zipfile.BadZipFile, ValueError):
            pass  # a test base's placeholder VPK: the plugins decide
    variant, seen, why = classify(blobs)
    if variant is None:
        raise ValueError("The base zip's modules are not one build: " + json.dumps({"seen": seen, "why": why}))
    return variant, app_version(blobs["app"])[0] if "app" in blobs else None


def tier_release(name):
    """(asset table, release tag) of a model size's recognizer, by the tables this module sees."""
    for assets, tag in ((VOCR3_ASSETS, VOCR3_TAG), (VOCR4_ASSETS, VOCR4_TAG), (VOCR_ASSETS, VOCR_TAG)):
        if name in assets:
            return assets, tag
    raise ValueError("Not a pinned recognizer: " + name)


def shipped_models():
    """(name, pins, release tag, role) of every shipped weights file: the three model sizes'
    recognizers (tools/model_tiers.json, smallest first: Small, Normal the default, Large; each
    reads its doubtful lines again with the next size up, the confidence cascade), and the beam +
    LM decoder's four LMs. A later generation's three blobs replace the sizes' through the pins."""
    tiers = model_tiers()
    models = []
    for tier in tiers["tiers"]:
        assets, tag = tier_release(tier["model"])
        models.append((tier["model"], assets, tag,
                       tier["name"] + " model size" + (" (default)" if tier["id"] == tiers["default"] else "")))
    # the beam + LM decoder's LM tiers (ocr_lm, the largest that fits up to 1000k by default)
    for name in VOCR_LM_ASSETS:
        models.append((name, VOCR_LM_ASSETS, VOCR_LM_TAG, "character LM (beam + LM decoder)"))
    return models


def one_line(value):
    """No control character and nothing Python's splitlines() (and so migrate_config.py) splits on."""
    return len(("<" + value + ">").splitlines()) == 1 and all(ord(c) >= 32 and c != "\x7f" for c in value)


def parse_setting(text):
    """KEY=VALUE for --set: a key of the config template and a one-line value."""
    key, sep, value = text.partition("=")
    key, value = key.strip().lower(), value.strip()
    if not sep or not SETTING_KEY.fullmatch(key) or not one_line(value):
        raise argparse.ArgumentTypeError("expected KEY=VALUE on one line: " + text)
    return key, value


def apply_settings(text, settings):
    """The config template with each setting's line replaced; every key must be in it."""
    out, seen = [], set()
    for line in text.splitlines():
        m = re.match(r"^(\w+)\s*=", line)
        if m and m.group(1) in settings:
            line = ("%s = %s" % (m.group(1), settings[m.group(1)])).rstrip()
            seen.add(m.group(1))
        out.append(line)
    missing = sorted(set(settings) - seen)
    if missing:
        raise ValueError("Not a setting of the config template: " + ", ".join(missing))
    return "\n".join(out) + "\n"


def template_settings(text):
    """key -> value of every setting of a config template, in its order."""
    out = {}
    for line in text.splitlines():
        m = re.match(r"^(\w+)\s*=\s*(.*?)\s*$", line)
        if m:
            if m.group(1) in out:
                raise ValueError("The config template sets " + m.group(1) + " twice")
            out[m.group(1)] = m.group(2)
    return out


def manifest_defaults(text, shipped):
    """The manifest's defaults: every setting of the package's config template (its --set settings
    applied), the on | off switches as true/false, the numbers as integers and a credential only as
    "set" or ""; then ocr_engine, the generation of its vocr_tier's model (tools/model_tiers.json; the
    table's default size when the template names none): the local engine's name in the Overlay app;
    and anki, anki_mining's value under its earlier name."""
    defaults = {}
    for key, value in template_settings(text).items():
        if SECRET.search(key):
            value = "set" if value else ""
        elif key in SWITCHES:
            if value.lower() not in ("on", "off"):
                raise ValueError(f"{key}: expected on or off, not {value!r}")
            value = value.lower() == "on"
        elif key in NUMBERS:
            if not re.fullmatch(r"\d+", value):
                raise ValueError(f"{key}: expected a number, not {value!r}")
            value = int(value)
        defaults[key] = value
    tiers = model_tiers()
    tier = str(defaults.get("vocr_tier", tiers["default"])).lower()
    model = {t["id"]: t["model"] for t in tiers["tiers"]}.get(tier)
    if model is None:
        raise ValueError(f"vocr_tier: {tier!r} is not a model size of tools/model_tiers.json")
    generation = shipped.get(model, {}).get("generation")
    if generation is None:
        raise ValueError(f"vocr_tier: {tier}'s model {model!r} is not one of the package's recognizers")
    return {**defaults, "ocr_engine": generation, "anki": defaults.get("anki_mining") is True}


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def verify(root):
    manifest = json.loads((root / "package-manifest.json").read_text())
    for name, expected in manifest["files"].items():
        path = root / name
        if not path.resolve().is_relative_to(root.resolve()):
            raise ValueError("Invalid package path: " + name)
        if path.stat().st_size != expected["bytes"] or digest(path) != expected["sha256"]:
            raise ValueError("Package checksum failed: " + name)
    print(f"Verified {len(manifest['files'])} package files")


def package(base, output, models, dictionary=None, license_file=None, version="0.7.12", source_revision=None,
            settings=None, variant=None):
    source = Path(__file__).resolve().parents[1]
    license_file = license_file or source / "docs/JMdict-LICENSE.html"
    if dictionary is not None:
        return package_with_dictionary(base, output, models, dictionary, license_file, version, source_revision,
                                       settings, variant)
    # The cross-build already contains the exact frozen dictionary. Packaging
    # needs no private-repository access and cannot substitute a fresh snapshot.
    with tempfile.TemporaryDirectory(prefix="vjo-release-") as tmp:
        dictionary = Path(tmp) / FILENAME
        with zipfile.ZipFile(base) as archive, archive.open(DATA + "dictionaries/" + FILENAME) as src, dictionary.open("wb") as dst:
            shutil.copyfileobj(src, dst)
        return package_with_dictionary(base, output, models, dictionary, license_file, version, source_revision,
                                       settings, variant)


def package_with_dictionary(base, output, models, dictionary, license_file, version, source_revision, settings=None,
                            variant=None):
    source = Path(__file__).resolve().parents[1]
    if source_revision is not None and not re.fullmatch(r"[0-9a-f]{40}", source_revision):
        raise ValueError("Expected an exact 40-character source revision")
    settings = dict(settings or {})
    conversion = load_manifest()
    validate(dictionary, conversion)
    with tempfile.TemporaryDirectory(prefix="vjo-config-") as tmp:
        # The package's config template (both copies: the installer's and the one a manual
        # install copies to the Vita), with this package's --set settings applied.
        template = Path(tmp) / "config.ini"
        template.write_text(apply_settings((source / "docs/config.example.ini").read_text(encoding="utf-8"), settings),
                            encoding="utf-8")
        assets = {DATA + "dictionaries/" + FILENAME: dictionary,
                  "docs/plaintext-dictionaries.json": source / "docs/plaintext-dictionaries.json",
                  "docs/plaintext-attribution.txt": source / "docs/plaintext-attribution.txt",
                  "docs/JMdict-LICENSE.html": license_file,
                  "config.ini": template,
                  DATA + "config.ini": template,
                  "install-vita.sh": source / "tools/install-vita.sh"}
        return write_package(base, output, models, assets, conversion, version, source_revision, settings, variant)


def write_package(base, output, models, assets, conversion, version, source_revision, settings, variant=None):
    shipped = {}
    tiers = model_tiers()
    tier_of = {tier["model"]: tier["id"] for tier in tiers["tiers"]}
    for name, pins, tag, role in shipped_models():
        path = models / name
        if not path.is_file():
            raise ValueError("OCR model is missing: " + name)
        _, size, sha = pins[name]
        if path.stat().st_size != size or digest(path) != sha:
            raise ValueError("OCR model checksum failed: " + name)
        assets[DATA + "ocr/" + name] = path
        # The generation the Overlay app names the local engine after (core/vocr_models.c): a
        # recognizer's only; an LM is not an engine. A model size's recognizer also names its tier.
        shipped[name] = {"release": "bee-san/vita-vn-ocr " + tag,
                         **({"generation": VOCR_GENERATION[tag]} if name.endswith(".vocr") else {}),
                         **({"tier": tier_of[name]} if name in tier_of else {}),
                         "bytes": size, "sha256": sha, "role": role}
    # The LMs are CC BY-SA 4.0: their attribution ships with them.
    attribution = models / VOCR_LM_ATTRIBUTION
    if not attribution.is_file():
        raise ValueError("The LMs' " + VOCR_LM_ATTRIBUTION + " is missing (release " + VOCR_LM_TAG + ")")
    _, size, sha = VOCR_LM_ATTRIBUTION_PIN[VOCR_LM_ATTRIBUTION]
    if attribution.stat().st_size != size or digest(attribution) != sha:
        raise ValueError("The LMs' " + VOCR_LM_ATTRIBUTION + " checksum failed: not release " + VOCR_LM_TAG + "'s")
    assets[DATA + "ocr/LM-" + VOCR_LM_ATTRIBUTION] = attribution
    # The defaults are the package's config template's (the same file as both config.ini copies).
    manifest = {"version": version, "files": {}, "dictionary": conversion,
                "defaults": manifest_defaults(assets["config.ini"].read_text(encoding="utf-8"), shipped),
                "models": shipped}
    if settings:
        # Applied by install-vita.sh after its own settings (tools/install-vita.sh); a credential's value is in
        # the package's config.ini only.
        manifest["install_settings"] = {k: ("set" if v else "") if SECRET.search(k) else v for k, v in settings.items()}
    if source_revision:
        manifest["source_revision"] = source_revision
    # The base's checks first, so that a refused base leaves no output behind.
    with zipfile.ZipFile(base) as original:
        if original.testzip() is not None:
            raise ValueError("Base archive is corrupt")
        required = {"ur0/tai/VitaJPOverlay_Kernel.skprx", "ur0/tai/VitaJPOverlay_Shell.suprx",
                    "ur0/data/VitaJPOverlay/vitajpoverlay.rco", "JPOverlaySettings.vpk",
                    "ur0/tai/VitaJPOverlay_OCR.suprx"}
        if not required.issubset(original.namelist()):
            raise ValueError("Base archive is missing plugins, RCO or settings VPK")
        app = app_info(original)
        if app:
            manifest["app"] = app  # install_ftp.sh --check-app and --collect compare the Vita's app with it
        # The build (docs/release-variants.md): what the modules are, never what a name says.
        found, app_version = base_variant(original)
        if variant is not None and found is not None and found != variant:
            raise ValueError(f"The base zip is the {found} build, not the {variant} one")
        if found or variant:
            manifest["variant"] = found or variant
        if app_version is not None:
            manifest["app_version"] = app_version
        # Where the game OCR worker comes from (docs/one-vpk-install.md): the Overlay app's own vjoworker=
        # mark, never this checkout's header. install-vita.sh passes it to the patcher; an older base
        # without the mark is the per-title rules'.
        import package_vpk  # packaging only: --verify (install-vita.sh) needs it not
        app_vpk = original.read("JPOverlaySettings.vpk")
        try:
            facts = package_vpk.app_facts(app_vpk)
        except ValueError:
            facts = None  # an app from before the one-VPK install (or a test's placeholder)
        manifest["worker"] = facts[2] if facts else "lines"
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(base) as original, zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for info in original.infolist():
            if info.is_dir() or info.filename in assets or info.filename == "package-manifest.json":
                continue
            data = original.read(info)
            archive.writestr(info, data)
            manifest["files"][info.filename] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        for name, path in assets.items():
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o100755 if name.endswith(".sh") else 0o100644) << 16
            with path.open("rb") as stream, archive.open(info, "w") as target:
                shutil.copyfileobj(stream, target)
            manifest["files"][name] = {"bytes": path.stat().st_size, "sha256": digest(path)}
        if facts:
            # The one-VPK install's payload manifest of these same files (tools/package_vpk.py): the line
            # install-vita.sh records as installed.txt is then the one the Overlay app writes.
            entries = {n: (f["bytes"], f["sha256"]) for n, f in manifest["files"].items()}
            text = package_vpk.build_manifest(version, app_vpk, entries, manifest.get("install_settings"))
            info = zipfile.ZipInfo("payload-manifest.txt", (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, text)
            manifest["files"]["payload-manifest.txt"] = {"bytes": len(text), "sha256": hashlib.sha256(text.encode()).hexdigest()}
        archive.writestr("package-manifest.json", json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"file": str(output), "bytes": output.stat().st_size, "sha256": digest(output)}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base", type=Path, nargs="?")
    parser.add_argument("-o", "--output", type=Path)
    parser.add_argument("--models", type=Path)
    parser.add_argument("--dictionary", type=Path)
    parser.add_argument("--license-file", type=Path)
    parser.add_argument("--verify", type=Path)
    parser.add_argument("--version", default="0.7.12")
    parser.add_argument("--source-revision", help="Exact tested overlay commit for the package manifest")
    parser.add_argument("--variant", choices=VARIANTS,
                        help="The base zip's build, checked against its modules (default: what they are); "
                             "the manifest records it (docs/release-variants.md)")
    parser.add_argument("--set", dest="settings", action="append", type=parse_setting, default=[], metavar="KEY=VALUE",
                        help="A config.ini setting of this package (repeatable): written into its config template "
                             "and applied by install-vita.sh, e.g. --set anki_mining=on for a test build")
    args = parser.parse_args()
    if args.verify:
        verify(args.verify)
    elif all((args.base, args.output, args.models)):
        package(args.base, args.output, args.models, args.dictionary, args.license_file, args.version, args.source_revision,
                dict(args.settings), args.variant)
    else:
        parser.error("Provide --verify DIRECTORY, or base, --output and --models (dictionary is already in the base)")


if __name__ == "__main__":
    main()
