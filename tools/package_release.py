#!/usr/bin/env python3
"""Combine tested plugins/settings VPK with pinned OCR and plain JMdict data."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import shutil
import zipfile

from prepare_ocr import VOCR_ASSETS, VOCR2_ASSETS, VOCR2_ENABLED, VOCR2_MODEL, VOCR2_TAG
import prepare_ppocrv6

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


def package(base, output, models, dictionary, license_file, version="0.7.12", source_revision=None):
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
    model_names = ["FL10_w8.vocr", "H15_w8.vocr"]
    if VOCR2_ENABLED:
        model_names.append(VOCR2_MODEL)
    model_names.append(prepare_ppocrv6.MODEL_NAME)
    for name in model_names:
        path = models / name
        if not path.is_file():
            raise ValueError("OCR model is missing: " + name)
        if name == prepare_ppocrv6.MODEL_NAME:
            _, size, sha = prepare_ppocrv6.WEIGHTS
        else:
            _, size, sha = (VOCR2_ASSETS if name == VOCR2_MODEL else VOCR_ASSETS)[name]
        if path.stat().st_size != size or digest(path) != sha:
            raise ValueError("OCR model checksum failed: " + name)
        assets[DATA + "ocr/" + name] = path
    manifest = {"version": version, "files": {}, "dictionary": conversion,
                "defaults": {"ocr_backend": "vocr", "vocr_model": "FL10_w8.vocr",
                             "dictionary": "local", "local_dictionaries": "jmdict.vjdict",
                             "japanese_only_selection": True,
                             "ocr_mode": "auto", "anki": False}}
    if source_revision:
        manifest["source_revision"] = source_revision
    manifest["ocr_backends"] = {}
    if VOCR2_ENABLED:
        manifest["ocr_backends"] = {
            "vocr2": {"model": VOCR2_MODEL, "finder_mode": "textbox",
                      "model_release": VOCR2_TAG,
                      "model_sha256": VOCR2_ASSETS[VOCR2_MODEL][2],
                      "model_bytes": VOCR2_ASSETS[VOCR2_MODEL][1],
                      "recognizer_arena_bytes": 124832,
                      "experimental": True,
                      "qualification": "rejected_for_promotion",
                      "default": False,
                      "protected_result_sha256": "3c2c873239e1bcbdf3858650d7b628853b0f4b1f4c324ecc9519a7348c8d09f7",
                      "runtime_commit": "49ff6f4d1c55804c600d098810574a49d382a009"}
        }
    manifest["ocr_backends"]["ppocrv6"] = {
        "model": prepare_ppocrv6.MODEL_NAME, "model_release": prepare_ppocrv6.RELEASE_TAG,
        "model_bytes": prepare_ppocrv6.WEIGHTS[1], "model_sha256": prepare_ppocrv6.WEIGHTS[2],
        "ncnn_heap_bytes": 18874368, "job_stack_bytes": 65536, "manual_only": True, "default": False,
        "source": prepare_ppocrv6.ONNX[0], "source_sha256": prepare_ppocrv6.ONNX[2],
        "ncnn_revision": prepare_ppocrv6.NCNN_REV, "license": "Apache-2.0 (PaddleOCR)"}
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(base) as original, zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        if original.testzip() is not None:
            raise ValueError("Base archive is corrupt")
        required = {"ur0/tai/VitaJPOverlay_Kernel.skprx", "ur0/tai/VitaJPOverlay_Shell.suprx",
                    "ur0/data/VitaJPOverlay/vitajpoverlay.rco", "JPOverlaySettings.vpk",
                    "ur0/tai/VitaJPOverlay_OCR.suprx"}
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
    parser.add_argument("--version", default="0.7.12")
    parser.add_argument("--source-revision", help="Exact tested overlay commit for the package manifest")
    args = parser.parse_args()
    if args.verify:
        verify(args.verify)
    elif all((args.base, args.output, args.models, args.dictionary, args.license_file)):
        package(args.base, args.output, args.models, args.dictionary, args.license_file, args.version, args.source_revision)
    else:
        parser.error("Provide --verify DIRECTORY, or base, --output, --models, --dictionary and --license-file")


if __name__ == "__main__":
    main()
