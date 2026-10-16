#!/usr/bin/env python3
"""The release's two zips from the two builds' base zips (docs/release-variants.md).

    package_variants.py --normal VitaJPOverlay-V.zip --debug VitaJPOverlay-V-debug.zip --models DIR \\
        --version V -o OUT_DIR [--source-revision SHA] [--set KEY=VALUE ...]

writes OUT_DIR/VitaJPOverlay-V.zip (the normal build, the main download), OUT_DIR/VitaJPOverlay-V-debug.zip
(the debug build, the test builds' own) and OUT_DIR/SHA256SUMS. Both are tools/package_release.py's
packages of one tree with the same pinned models, frozen dictionary, config template and --set
settings, so a config.ini from either works in the other. Before it writes anything it checks the
bases: each is the build it is given as (tools/check_variant.py, from the modules themselves), the
Overlay app of each names version V, and the two differ only in the build's own files (the plugins,
the Overlay app's VPK and its stamp). After, it verifies both packages (package_release.py --verify on
each, unpacked) and that they differ in exactly those files and the manifests' variant.
"""
import argparse
import hashlib
import json
import sys
import tempfile
import zipfile
from pathlib import Path

import package_release as release

# The files a build makes for itself: everything else of the two packages is the same bytes.
BUILD_FILES = {"ur0/tai/VitaJPOverlay_Kernel.skprx", "ur0/tai/VitaJPOverlay_Shell.suprx",
               "ur0/tai/VitaJPOverlay_OCR.suprx", "JPOverlaySettings.vpk",
               "ux0/data/VitaJPOverlay/vjo_app.txt"}  # the app's stamp: its eboot's hash (core/app_stamp.h)
MANIFEST = "package-manifest.json"


def zip_name(version, variant):
    return f"VitaJPOverlay-{version}{'-debug' if variant == 'debug' else ''}.zip"


def contents(path):
    """name -> bytes of a zip's files."""
    with zipfile.ZipFile(path) as z:
        return {i.filename: z.read(i) for i in z.infolist() if not i.is_dir()}


def check_bases(normal, debug, version):
    """Every problem with the two base zips as this release's (empty: none)."""
    problems = []
    for path, want in ((normal, "normal"), (debug, "debug")):
        try:
            with zipfile.ZipFile(path) as z:
                got, app = release.base_variant(z)
        except (ValueError, KeyError, zipfile.BadZipFile) as e:  # modules of both builds, a missing plugin, not a zip
            problems.append(f"{path.name}: {e}")
            continue
        if got != want:
            problems.append(f"{path.name}: the {got} build, given as the {want} one")
        if app != version:
            problems.append(f"{path.name}: the Overlay app is v{app}, the release v{version} "
                            "(set VJO_VERSION in vita/CMakeLists.txt and build again)")
    a, b = contents(normal), contents(debug)
    if set(a) != set(b):
        problems.append(f"the bases' files differ: only normal {sorted(set(a) - set(b))}, "
                        f"only debug {sorted(set(b) - set(a))}")
    for name in sorted(set(a) & set(b) - BUILD_FILES):
        if a[name] != b[name]:
            problems.append(f"{name} differs between the bases (one tree builds both)")
    return problems


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def check_packages(normal_zip, debug_zip):
    """Every problem with the two packages (empty: none): each verifies, and they differ in the
    build's own files and the manifests' variant, file entries and app version only."""
    problems = []
    with tempfile.TemporaryDirectory(prefix="vjo-variants-") as tmp:
        for path in (normal_zip, debug_zip):
            root = Path(tmp) / path.stem
            with zipfile.ZipFile(path) as z:
                z.extractall(root)
            try:
                release.verify(root)
            except ValueError as e:
                problems.append(f"{path.name}: {e}")
    a, b = contents(normal_zip), contents(debug_zip)
    if set(a) != set(b):
        problems.append(f"the packages' files differ: {sorted(set(a) ^ set(b))}")
    for name in sorted(set(a) & set(b) - BUILD_FILES - {MANIFEST}):
        if a[name] != b[name]:
            problems.append(f"{name} differs between the packages")
    ma, mb = json.loads(a[MANIFEST]), json.loads(b[MANIFEST])
    if (ma.get("variant"), mb.get("variant")) != ("normal", "debug"):
        problems.append(f"manifest variants {ma.get('variant')} / {mb.get('variant')}")
    for key in sorted(set(ma) | set(mb)):
        if key in ("variant", "files", "app"):  # app: the build's own Overlay app (its stamp and eboot)
            continue
        if ma.get(key) != mb.get(key):
            problems.append(f"manifest {key} differs between the packages")
    fa = {k: v for k, v in ma["files"].items() if k not in BUILD_FILES}
    fb = {k: v for k, v in mb["files"].items() if k not in BUILD_FILES}
    if fa != fb:
        problems.append("the manifests' shared files differ")
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--normal", required=True, type=Path, help="The normal build's base zip (VJO_DEBUG=OFF)")
    parser.add_argument("--debug", required=True, type=Path, help="The debug build's base zip (VJO_DEBUG=ON)")
    parser.add_argument("--models", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("-o", "--output", required=True, type=Path, help="Directory for the two zips and SHA256SUMS")
    parser.add_argument("--source-revision", help="Exact tested overlay commit for both manifests")
    parser.add_argument("--set", dest="settings", action="append", type=release.parse_setting, default=[],
                        metavar="KEY=VALUE", help="A config.ini setting of both packages (package_release.py --set)")
    args = parser.parse_args()
    problems = check_bases(args.normal, args.debug, args.version)
    if problems:
        sys.exit("package_variants: the bases are not this release's two builds:\n  " + "\n  ".join(problems))
    args.output.mkdir(parents=True, exist_ok=True)
    zips = {}
    for variant, base in (("normal", args.normal), ("debug", args.debug)):
        zips[variant] = args.output / zip_name(args.version, variant)
        release.package(base, zips[variant], args.models, version=args.version, source_revision=args.source_revision,
                        settings=dict(args.settings), variant=variant)
    problems = check_packages(zips["normal"], zips["debug"])
    if problems:
        sys.exit("package_variants: the two packages are not one release:\n  " + "\n  ".join(problems))
    sums = "".join(f"{sha256(zips[v])}  {zips[v].name}\n" for v in ("normal", "debug"))
    (args.output / "SHA256SUMS").write_text(sums)
    print(sums, end="")
    print(json.dumps({v: {"file": str(p), "bytes": p.stat().st_size, "sha256": sha256(p)} for v, p in zips.items()},
                     indent=2))


if __name__ == "__main__":
    main()
