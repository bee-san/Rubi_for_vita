#!/usr/bin/env python3
"""Combine tested plugins/settings VPK with pinned OCR and plain JMdict data."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import shutil
import zipfile

from prepare_ocr import VOCR_ASSETS

DATA = "ux0/data/VitaJPOverlay/"


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


def package(base, output, models, dictionary, license_file, version="0.7.6", meiki_model=None, source_revision=None):
    source = Path(__file__).resolve().parents[1]
    if source_revision is not None and not re.fullmatch(r"[0-9a-f]{40}", source_revision):
        raise ValueError("Expected an exact 40-character source revision")
    conversion = json.loads(dictionary.with_suffix(".json").read_text())
    snapshot = json.loads((source / "docs/jmdict-source.json").read_text())
    if conversion["source_sha256"] != snapshot["sha256"] or digest(dictionary) != conversion["sha256"]:
        raise ValueError("JMdict does not match its pinned source and conversion manifest")
    with dictionary.open("rb") as stream:
        if stream.read(8) != b"VJDICT2\0":
            raise ValueError("Expected a version 2 disk-backed dictionary")
    assets = {DATA + "dictionaries/jmdict.vjdict": dictionary,
              "docs/jmdict-conversion.json": dictionary.with_suffix(".json"),
              "docs/JMdict-LICENSE.html": license_file,
              DATA + "config.ini": source / "docs/config.example.ini",
              "install-vita.sh": source / "tools/install-vita.sh"}
    for name in ("FL10_w8.vocr", "H15_w8.vocr"):
        path = models / name
        _, size, sha = VOCR_ASSETS[name]
        if path.stat().st_size != size or digest(path) != sha:
            raise ValueError("OCR model checksum failed: " + name)
        assets[DATA + "ocr/" + name] = path
    if meiki_model:
        pin = json.loads((source / "docs/meiki-dependencies.json").read_text())["dependencies"]["converted"]
        if meiki_model.stat().st_size != pin["bytes"] or digest(meiki_model) != pin["sha256"]:
            raise ValueError("Meiki model checksum failed")
        assets[DATA + "ocr/" + pin["filename"]] = meiki_model
    manifest = {"version": version, "files": {}, "dictionary": conversion,
                "defaults": {"ocr_backend": "vocr", "vocr_model": "FL10_w8.vocr",
                             "dictionary": "local", "local_dictionaries": "jmdict.vjdict",
                             "japanese_only_selection": True,
                             "ocr_mode": "auto", "anki": False}}
    if source_revision:
        manifest["source_revision"] = source_revision
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(base) as original, zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        if original.testzip() is not None:
            raise ValueError("Base archive is corrupt")
        required = {"ur0/tai/VitaJPOverlay_Kernel.skprx", "ur0/tai/VitaJPOverlay_Shell.suprx",
                    "ur0/data/VitaJPOverlay/vitajpoverlay.rco", "JPOverlaySettings.vpk",
                    "ur0/tai/VitaJPOverlay_OCR.suprx"}
        if meiki_model and "meiki/meiki-engine.suprx" not in original.namelist():
            raise ValueError("Meiki trial archive is missing its isolated engine")
        if meiki_model:
            notices = {"LICENSE", "third_party/licenses/Meiki.txt",
                       "third_party/licenses/Meiki-model-LGPL-3.0.txt",
                       "third_party/licenses/Meiki-resize.txt", "third_party/licenses/MNN.txt",
                       "third_party/licenses/MNN-Flatbuffers-Apache-2.0.txt",
                       "third_party/licenses/MNN-Half-MIT.txt"}
            if not notices.issubset(original.namelist()):
                raise ValueError("Meiki trial archive is missing its license notices")
        if not required.issubset(original.namelist()):
            raise ValueError("Base archive is missing plugins, RCO or settings VPK")
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
    parser.add_argument("--version", default="0.7.6")
    parser.add_argument("--meiki-model", type=Path, help="Pinned recognizer; requires an optional-engine base archive")
    parser.add_argument("--source-revision", help="Exact tested overlay commit for the package manifest")
    args = parser.parse_args()
    if args.verify:
        verify(args.verify)
    elif all((args.base, args.output, args.models, args.dictionary, args.license_file)):
        package(args.base, args.output, args.models, args.dictionary, args.license_file, args.version, args.meiki_model, args.source_revision)
    else:
        parser.error("Provide --verify DIRECTORY, or base, --output, --models, --dictionary and --license-file")


if __name__ == "__main__":
    main()
