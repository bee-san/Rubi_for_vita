#!/usr/bin/env python3
"""The Overlay app's version stamp (core/app_stamp.h): which app the plugins came with, and which is installed.

    vjoapp1 api=21 build=0123456789abcdef

api is VJO_API_VERSION (include/vjo_api.h) of the tree the app was built from, build the first 16 hex digits
of the SHA-256 of its eboot.bin. The Vita build writes it (vita/CMakeLists.txt) into the app's VPK as
vjo_app.txt (installed as ux0:app/VJOSET001/vjo_app.txt) and, the same bytes, into the release as
ux0:data/VitaJPOverlay/vjo_app.txt: the record of the app the plugins came with, which the installer uploads
with them. The Shell plugin compares the two at each game start, the app compares itself with the record and
the running plugin, and install_ftp.sh (at the end of an install, and with --check-app) compares the Vita's
app with the package's.

    app_stamp.py make --eboot build/vita/eboot.bin --api-header include/vjo_api.h -o build/vita/vjo_app.txt
    app_stamp.py check --package STAMP [--vpk JPOverlaySettings.vpk] --installed FILE [--installed-eboot FILE]
                       [--host VITA_IP] [--after-install]

check prints a box with the verdict and what to do, and exits 0 when the Vita's app is the package's, 1 when it
is not, 2 when it cannot tell (no package stamp). A missing --installed or --installed-eboot file means the Vita
has none.
"""
import argparse
import hashlib
import re
import sys
import textwrap
import zipfile
from pathlib import Path

MAGIC = "vjoapp1"
STAMP = re.compile(r"^vjoapp1 api=(\d{1,6}) build=([0-9a-f]{16})$")
API = re.compile(r"^#define\s+VJO_API_VERSION\s+(\d+)\s*$", re.MULTILINE)
TITLE_ID = "VJOSET001"
VPK = "JPOverlaySettings.vpk"


def api_version(header):
    """VJO_API_VERSION of include/vjo_api.h."""
    m = API.search(Path(header).read_text(encoding="utf-8"))
    if not m:
        raise ValueError(f"{header}: no #define VJO_API_VERSION")
    return int(m.group(1))


def make(eboot_bytes, api):
    """The stamp line (with its newline) of an eboot.bin built at API api."""
    return f"{MAGIC} api={int(api)} build={hashlib.sha256(eboot_bytes).hexdigest()[:16]}\n"


def parse(text):
    """{'api', 'build', 'line'} of a stamp's first line, or None when it is not one."""
    if text is None:
        return None
    line = text.split("\n", 1)[0].strip("\r")
    m = STAMP.match(line)
    return {"api": int(m.group(1)), "build": m.group(2), "line": line} if m else None


def vpk_info(path):
    """The stamp and eboot.bin of an app VPK: {'stamp': parsed or None, 'eboot_sha256', 'eboot_bytes'}; None
    when the file is not a VPK with an eboot.bin."""
    try:
        with zipfile.ZipFile(path) as z:
            eboot = z.read("eboot.bin")
            try:
                stamp = parse(z.read("vjo_app.txt").decode("utf-8", "replace"))
            except KeyError:
                stamp = None
    except (OSError, KeyError, zipfile.BadZipFile):
        return None
    return {"stamp": stamp, "eboot_sha256": hashlib.sha256(eboot).hexdigest(), "eboot_bytes": len(eboot)}


def verdict(package, installed_text, installed_eboot=None, package_eboot_sha256=None):
    """(state, lines): state "ok" (the Vita's app is the package's), "mismatch" or "unknown" (no package
    stamp); installed_text the Vita's vjo_app.txt (None: not there), installed_eboot its eboot.bin's bytes
    (None: not there), package_eboot_sha256 the package VPK's eboot.bin, when known."""
    pkg = parse(package)
    if not pkg:
        return "unknown", ["This package has no Overlay app stamp to compare with (built before the stamps)."]
    inst = parse(installed_text)
    eboot_sha = hashlib.sha256(installed_eboot).hexdigest() if installed_eboot is not None else None
    eboot_note = []
    if eboot_sha and package_eboot_sha256:
        eboot_note = ["Its eboot.bin is the package VPK's." if eboot_sha == package_eboot_sha256 else
                      "Its eboot.bin differs from the package VPK's."]
    if inst and inst["line"] == pkg["line"]:
        # The stamp decides. Whether VitaShell installs eboot.bin byte for byte is not measured on a console yet.
        if eboot_note and eboot_sha != package_eboot_sha256:
            eboot_note = ["Its eboot.bin is not byte-identical to the VPK's (the stamp decides; send --collect's zip)."]
        return "ok", [f"The Overlay app on the Vita is this package's ({pkg['line']})."] + eboot_note
    if installed_text is None and installed_eboot is None:
        why = [f"No Overlay app is installed on the Vita (ux0:app/{TITLE_ID})."]
    elif installed_text is None:
        why = [("The Overlay app on the Vita has no version stamp: it is from an older build "
                "(v0.8.0-test.2 or earlier).")]
    elif not inst:
        why = [f"The Overlay app's stamp on the Vita is not readable: {installed_text.strip()[:60]!r}."]
    else:
        older = "an older" if inst["api"] < pkg["api"] else "a newer" if inst["api"] > pkg["api"] else "another"
        why = [f"The Overlay app on the Vita is {older} build ({inst['line']}); this package's is {pkg['line']}."]
    return "mismatch", why + eboot_note


def steps(command, after_install=False):
    first = "Do it now, before you reboot: in VitaShell" if after_install else "In VitaShell"
    return [(f"{first}, press OK on the FTP dialog to keep FTP running, open ux0:data/, select "
             "JPOverlaySettings.vpk, press the confirm button (x on most consoles, o on Japanese ones) and confirm "
             "the install."),
            f"Then check it from here, with VitaShell's FTP still running: {command}"
            + (", then reboot." if after_install else ""),
            ("An older app shows engines and pages that are gone and saves settings the plugins no longer use "
             "(test.2's run had test.1's app writing ocr_backend = vocr2, #101).")]


def box(title, paragraphs, width=78):
    inner = width - 4
    blank = "|" + " " * (width - 2) + "|"
    rows = ["+" + "-" * (width - 2) + "+", "| " + title.ljust(inner) + " |"]
    for p in paragraphs:
        rows.append(blank)
        for line in textwrap.wrap(p, inner, break_on_hyphens=False, break_long_words=True) or [""]:
            rows.append("| " + line.ljust(inner) + " |")
    rows.append("+" + "-" * (width - 2) + "+")
    return "\n".join(rows)


def report(state, lines, command, after_install):
    if state == "ok":
        return box("Overlay app: up to date", lines)
    if state == "unknown":
        return box("Overlay app: not checked", [*lines, ("Install ux0:data/JPOverlaySettings.vpk in VitaShell to "
                                                         "be sure it is this package's.")])
    title = "ONE MORE STEP: install the Overlay app (JPOverlaySettings.vpk)" if after_install else \
        "Overlay app: NOT this package's"
    return box(title, lines + steps(command, after_install))


def read(path, binary=False):
    p = Path(path) if path else None
    if not p or not p.is_file():
        return None
    return p.read_bytes() if binary else p.read_text(encoding="utf-8", errors="replace")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("make", help="write the stamp of an eboot.bin")
    m.add_argument("--eboot", required=True, type=Path)
    m.add_argument("--api-header", required=True, type=Path)
    m.add_argument("-o", "--output", required=True, type=Path)
    c = sub.add_parser("check", help="the Vita's app against the package's")
    c.add_argument("--package", required=True, type=Path, help="the package's vjo_app.txt")
    c.add_argument("--vpk", type=Path, help="the package's JPOverlaySettings.vpk (its eboot.bin is compared too)")
    c.add_argument("--installed", type=Path, help="the Vita's ux0:app/VJOSET001/vjo_app.txt, fetched (missing: none)")
    c.add_argument("--installed-eboot", type=Path, help="the Vita's ux0:app/VJOSET001/eboot.bin, fetched")
    c.add_argument("--command", default="bash tools/install_ftp.sh --check-app VITA_IP",
                   help="the check command the box shows")
    c.add_argument("--after-install", action="store_true", help="worded as the installer's last step")
    args = ap.parse_args()
    if args.cmd == "make":
        line = make(args.eboot.read_bytes(), api_version(args.api_header))
        args.output.write_text(line, encoding="utf-8")
        print(line.strip())
        return 0
    package = read(args.package)
    vpk = vpk_info(args.vpk) if args.vpk else None
    if vpk and vpk["stamp"] and package and parse(package) and vpk["stamp"]["line"] != parse(package)["line"]:
        print(f"error: the package's {args.package} and its VPK's vjo_app.txt differ", file=sys.stderr)
        return 2
    state, lines = verdict(package, read(args.installed), read(args.installed_eboot, binary=True),
                           vpk["eboot_sha256"] if vpk else None)
    print(report(state, lines, args.command, args.after_install))
    return {"ok": 0, "mismatch": 1}.get(state, 2)


if __name__ == "__main__":
    sys.exit(main())
