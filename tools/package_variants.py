#!/usr/bin/env python3
"""The release's packages from the two builds' base zips (docs/release-variants.md, docs/one-vpk-install.md).

    package_variants.py --normal Rubi-V.zip --debug Rubi-V-debug.zip --models DIR \\
        --version V -o OUT_DIR [--source-revision SHA] [--set KEY=VALUE ...]

writes OUT_DIR/Rubi-V.vpk (the normal build's one-VPK install, the main download),
OUT_DIR/Rubi-V-debug.vpk, OUT_DIR/Rubi-V.zip and OUT_DIR/Rubi-V-debug.zip
(install-vita.sh, the advanced path) and OUT_DIR/SHA256SUMS, the normal VPK first. Both are tools/package_release.py's
packages of one tree with the same pinned models, frozen dictionary, config template and --set
settings, so a config.ini from either works in the other. Before it writes anything it checks the
bases: each is the build it is given as (tools/check_variant.py, from the modules themselves), the
Rubi app of each names version V (for a test build, V-test.N or a respin V-test.N.M, the V of its tree), and the two differ
only in the build's own files (the plugins, the Rubi app's VPK and its stamp). After, it verifies both
packages (package_release.py --verify on each, unpacked) and that they differ in exactly those files and
the manifests' variant.
"""
import argparse
import hashlib
import io
import json
import re
import sys
import tempfile
import zipfile
from pathlib import Path

import package_release as release
import package_vpk

# The files a build makes for itself: everything else of the two packages is the same bytes.
BUILD_FILES = {"ur0/tai/VitaJPOverlay_Kernel.skprx", "ur0/tai/VitaJPOverlay_Shell.suprx",
               "ur0/tai/VitaJPOverlay_OCR.suprx", "Rubi.vpk",
               "ux0/data/VitaJPOverlay/vjo_app.txt",  # the app's stamp: its eboot's hash (core/app_stamp.h)
               "payload-manifest.txt"}  # the one-VPK payload's: the build's plugins and app (tools/package_vpk.py)
MANIFEST = "package-manifest.json"


def release_of(app, version):
    """Whether `version` is a release of the Rubi app's own version `app`: that version, or a test build of it,
    `<app>-test.N`, or a respin of a test build, `<app>-test.N.M` (0.8.0-test.4.1: test.4 with fixes, #194). A test
    package's binaries are the tree's VJO_VERSION build: docs/release-variants.md, and the payload manifest's
    `version 0.8.0-test.4` beside `app_version 0.8.0`, docs/one-vpk-install.md. The app orders a respin after its
    test build and before the next (core/payload.c vjo_version_compare)."""
    test_build = re.escape(app or "") + r"-test\.[1-9][0-9]{0,3}(?:\.[1-9][0-9]{0,3})?"
    return app is not None and (version == app or re.fullmatch(test_build, version) is not None)


def zip_name(version, variant):
    return f"Rubi-{version}{'-debug' if variant == 'debug' else ''}.zip"


def vpk_name(version, variant):
    return f"Rubi-{version}{'-debug' if variant == 'debug' else ''}.vpk"


def contents(path):
    """name -> bytes of a zip's files."""
    with zipfile.ZipFile(path) as z:
        return {i.filename: z.read(i) for i in z.infolist() if not i.is_dir()}


def module_problems(base, variant, sources):
    """tools/check_variant.py's check of a base zip's four modules with the sources of `sources` (a checkout of
    the bases' commit): what the Vita build checks, the worker included, which no fixed marker names (audit-
    overlay-release-variants-d note 9: a debug base with the normal worker)."""
    import check_variant as variants
    names = {"kernel": "ur0/tai/VitaJPOverlay_Kernel.skprx", "shell": "ur0/tai/VitaJPOverlay_Shell.suprx",
             "worker": "ur0/tai/VitaJPOverlay_OCR.suprx"}
    blobs = {}
    with tempfile.TemporaryDirectory(prefix="vjo-variant-") as tmp:
        for part, name in names.items():
            (Path(tmp) / part).write_bytes(base.read(name))
            blobs[part] = variants.segments(Path(tmp) / part)
        with zipfile.ZipFile(io.BytesIO(base.read("Rubi.vpk"))) as vpk:
            (Path(tmp) / "eboot.bin").write_bytes(vpk.read("eboot.bin"))
            blobs["app"] = variants.segments(Path(tmp) / "eboot.bin")
    return variants.check(variant, blobs, sources=sources)


def check_bases(normal, debug, version, sources=None):
    """Every problem with the two base zips as this release's (empty: none); with sources, each base's modules
    are also checked against them (module_problems)."""
    problems = []
    for path, want in ((normal, "normal"), (debug, "debug")):
        try:
            with zipfile.ZipFile(path) as z:
                got, app = release.base_variant(z)
                checked = module_problems(z, want, sources) if sources is not None and got == want else []
        except (ValueError, KeyError, zipfile.BadZipFile, OSError) as e:  # both builds' modules, a missing plugin,
            # not a zip, no file (a directory, a missing path)
            problems.append(f"{path.name}: {e}")
            continue
        if got != want:
            problems.append(f"{path.name}: the {got} build, given as the {want} one")
        problems += [f"{path.name}: {p}" for p in checked]
        if app is None:  # a base from before the two builds (package_release.base_variant)
            problems.append(f"{path.name}: the Rubi app names no version (a base from before the two builds)")
        elif not release_of(app, version):
            problems.append(f"{path.name}: the Rubi app is v{app}, the release v{version}: give v{app} or a "
                            f"test build of it, v{app}-test.N or v{app}-test.N.M (or set VJO_VERSION in vita/CMakeLists.txt and "
                            "build again)")
    if problems:
        return problems  # a base that is not a zip, or not a build: nothing more to compare
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
        if key in ("variant", "files", "app"):  # app: the build's own Rubi app (its stamp and eboot)
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
    parser.add_argument("--sources", type=Path, help="A checkout of the bases' own commit: their modules are checked "
                        "as its Vita build checks them (tools/check_variant.py --sources), the worker included; a path "
                        "that is not one (no shell/, kernel/, game_ocr/, settings/) is refused")
    args = parser.parse_args()
    if args.sources is not None:
        import check_variant as variants
        problem = variants.sources_problem(args.sources)
        if problem:  # its modules would be checked against nothing (audit-overlay-release-variants-e note 5)
            sys.exit(f"package_variants: {problem}: give a checkout of the bases' own commit (CI: --sources .)")
    problems = check_bases(args.normal, args.debug, args.version, args.sources)
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
    # the worker mode first: no VPK is written of a build whose payload would install no game's OCR
    with zipfile.ZipFile(zips["normal"]) as z:
        worker = json.loads(z.read(MANIFEST)).get("worker")
    if worker != "kernel":
        # the per-title rules need install-vita.sh --title: no main VPK in that mode without the owner
        sys.exit("package_variants: the build's worker mode is " + str(worker) + ", not kernel: a VPK of it would "
                 "install no game's OCR (docs/one-vpk-install.md, lines mode)")
    # the one-VPK install of each build (tools/package_vpk.py): the zip's own files, the app's own facts
    vpks = {}
    for variant in ("normal", "debug"):
        vpks[variant] = args.output / vpk_name(args.version, variant)
        try:
            package_vpk.write_vpk(zips[variant], vpks[variant])
        except ValueError as e:
            sys.exit(f"package_variants: {vpks[variant].name}: {e}")
    assets = [vpks["normal"], vpks["debug"], zips["normal"], zips["debug"]]
    sums = "".join(f"{sha256(p)}  {p.name}\n" for p in assets)
    (args.output / "SHA256SUMS").write_text(sums)
    print(sums, end="")
    print(json.dumps({p.name: {"file": str(p), "bytes": p.stat().st_size, "sha256": sha256(p)} for p in assets},
                     indent=2))


if __name__ == "__main__":
    main()
