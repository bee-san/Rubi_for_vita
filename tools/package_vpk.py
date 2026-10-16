#!/usr/bin/env python3
"""The one-VPK install's package (docs/one-vpk-install.md): VitaJPOverlay-<version>.vpk from a release zip.

    package_vpk.py RELEASE.zip -o VitaJPOverlay-V.vpk        the VPK of a package_release.py zip
    package_vpk.py --verify VitaJPOverlay-V.vpk              its checks again (exit 1 with what is wrong)
    package_vpk.py --manifest RELEASE.zip                    the payload manifest the VPK would carry

The VPK is the zip's JPOverlaySettings.vpk (the Overlay app, entry for entry, its order kept) followed by
payload/: the files the app installs (include/vjo_payload.h, the same bytes as the zip's ur0/ and ux0/)
and payload/manifest.txt (core/payload.h's format), then licenses/: the zip's licence and attribution
files at their zip paths (LICENSE, third_party/, docs/plaintext-attribution.txt, docs/JMdict-LICENSE.html),
which stay in the app's folder (ux0:app/VJOSET001/licenses/; the models' LM-ATTRIBUTION.md is a payload
file, installed beside them). The manifest's app_version, variant, api and build
come from the app itself (its eboot.bin's start line, check_variant.py; its stamp, core/app_stamp.h),
and its worker mode from the eboot's vjoworker= mark (settings/install_page.c VJO_WORKER_MARK), never
from the checkout that packages it; the app accepts the payload only when they are its own.

Checks, before anything is written and again by --verify:
  - every payload file has the zip manifest's size and SHA-256 (package_release.py wrote both), and so
    does every licence file when the VPK is written; --verify checks that the GPL, the dictionary's
    attribution and JMdict's licence are there and nothing else is under licenses/ (the release's
    SHA256SUMS is the outside reference for the VPK's bytes as a whole);
  - the manifest is exactly the one this tool makes of the VPK's own app and payload files: every
    line, kind and order, with only its version (named by the file, VitaJPOverlay-<version>.vpk) and
    its set lines (the app's keys, on or off) from the package;
  - the app's eboot.bin stays VitaShell's second entry, after sce_sys/param.sfo, so VitaShell's scan
    (archive.c archiveCheckFilesForUnsafeFself, which returns at the first unsafe SELF) stops there with
    its "extended permissions" question and reads no payload file;
  - no SELF in the VPK imports SceVshBridge's _vshIoMount or vshIoUmount (NIDs 0x3C522C35, 0x35BC26AC:
    VitaShell's "can potentially brick your device" warning, elf.c:126-134), and no plugin imports
    ScePromoterUtil or SceShellSvc (the app may: its install holds the PS button lock with SceShellSvc);
  - the VPK is a plain zip VitaShell's libarchive reads (stored or deflated entries, no zip64).
"""
import argparse
import hashlib
import io
import json
import re
import struct
import sys
import tempfile
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APP_VPK = "JPOverlaySettings.vpk"
MANIFEST = "payload/manifest.txt"
LICENSES = "licenses/"
# The zip's licence and attribution files the VPK carries (licenses/<zip path>); the first three must be there.
REQUIRED_LICENSES = ("LICENSE", "docs/plaintext-attribution.txt", "docs/JMdict-LICENSE.html")


def licence_name(name):
    """Whether a release zip's file is a licence or attribution file the VPK carries under licenses/."""
    return name in REQUIRED_LICENSES or name == "third_party/README.md" or (
        name.startswith("third_party/licenses/") and not name.endswith("/"))
MOUNT_NIDS = {0x3C522C35: "_vshIoMount", 0x35BC26AC: "vshIoUmount"}  # VitaShell 81af709 elf.c:126-134
UNSAFE_LIBRARIES = ("ScePromoterUtil", "SceShellSvc")                # elf.c:135-137
WORKER_MARK = re.compile(rb"vjoworker=(kernel|lines)")


def payload_files():
    """(destination, kind) of the payload, in include/vjo_payload.h's order: the compiled list itself."""
    header = (ROOT / "include/vjo_payload.h").read_text()
    sizes = (ROOT / "include/vjo_vocr_sizes.h").read_text()
    names = dict(re.findall(r'^#define (VJO_\w+_NAME) "([^"]+)"', sizes, re.M))
    for alias, target in re.findall(r"^#define (VJO_VOCR_TIER\d_NAME) (VJO_\w+_NAME)$", sizes, re.M):
        names[alias] = names[target]
    strings = {"VJO_PAYLOAD_DATA": "ux0:data/VitaJPOverlay/", "VJO_PAYLOAD_OCR": "ux0:data/VitaJPOverlay/ocr/"}
    block = header[header.index("#define VJO_PAYLOAD_FILE_LIST(X)"):header.index("#define VJO_PAYLOAD_COUNT_ONE")]
    out = []
    for args, kind in re.findall(r"X\((.+?), VJO_PAYLOAD_(COPY|MOVE|TEMPLATE)\)", block):
        dest = ""
        for part in re.findall(r'"[^"]*"|VJO_\w+', args):
            dest += part[1:-1] if part.startswith('"') else strings.get(part, names.get(part, "?" + part))
        if "?" in dest:
            raise ValueError("include/vjo_payload.h: cannot read " + args)
        out.append((dest, kind.lower()))
    return out


def zip_path(dest):
    """ur0:tai/X -> ur0/tai/X, the release zip's name of a device file."""
    return dest[:3] + "/" + dest[4:]


def sha256(data):
    return hashlib.sha256(data).hexdigest()


# ---------------- the SELFs (VitaShell's own reading, elf.c checkForUnsafeImports) ----------------

def self_imports(data):
    """(library name, [function NIDs]) of every import of a Vita SELF (fself, compressed or not)."""
    if data[:4] != b"SCE\0":
        raise ValueError("not a Vita SELF")
    elf_offset, ph_offset = struct.unpack_from("<QQ", data, 0x40)
    seginfo_offset, = struct.unpack_from("<Q", data, 0x58)
    if data[elf_offset:elf_offset + 4] != b"\x7fELF":
        raise ValueError("no ELF payload")
    entry, = struct.unpack_from("<I", data, elf_offset + 24)
    ph_size, ph_count = struct.unpack_from("<HH", data, elf_offset + 42)
    segments = []
    for i in range(ph_count):
        kind, _, vaddr, _, filesz, _, _, _ = struct.unpack_from("<8I", data, ph_offset + i * ph_size)
        offset, size, compression, _ = struct.unpack_from("<4Q", data, seginfo_offset + i * 32)
        blob = data[offset:offset + size]
        if compression == 2:
            blob = zlib.decompress(blob)
        segments.append((kind, vaddr, blob))
    index, info_offset = entry >> 30, entry & 0x3FFFFFFF
    _, base, code = segments[index]
    import_top, import_end = struct.unpack_from("<II", code, info_offset + 36 + 8)

    def at(pointer, size):
        for kind, vaddr, blob in segments:
            if kind == 1 and vaddr <= pointer and pointer + size <= vaddr + len(blob):
                return blob, pointer - vaddr
        raise ValueError(f"unmapped pointer 0x{pointer:x}")

    out, offset = [], import_top
    while offset < import_end:
        size, = struct.unpack_from("<H", code, offset)
        functions, = struct.unpack_from("<H", code, offset + 6)
        if size == 52:   # SceImportsTable3xx
            name_ptr, = struct.unpack_from("<I", code, offset + 20)
            nids_ptr, = struct.unpack_from("<I", code, offset + 28)
        elif size == 36:  # SceImportsTable2xx
            name_ptr, = struct.unpack_from("<I", code, offset + 16)
            nids_ptr, = struct.unpack_from("<I", code, offset + 20)
        else:
            raise ValueError(f"unknown import table entry size {size}")
        blob, o = at(name_ptr, 1)
        name = blob[o:blob.index(b"\0", o)].decode("ascii", "replace")
        blob, o = at(nids_ptr, functions * 4)
        nids = list(struct.unpack_from(f"<{functions}I", blob, o)) if functions else []
        out.append((name, nids))
        offset += size
    return out


def self_problems(name, data, plugin):
    """What VitaShell's scan would warn of in this SELF: [] when nothing beyond "extended permissions"."""
    problems = []
    for library, nids in self_imports(data):
        mounts = [MOUNT_NIDS[n] for n in nids if n in MOUNT_NIDS]
        if library == "SceVshBridge" and mounts:
            problems.append(f"{name} imports {', '.join(mounts)}: VitaShell would say it can brick the console")
        if plugin and library in UNSAFE_LIBRARIES:
            problems.append(f"{name} (a plugin) imports {library}")
    return problems


def eboot_facts(eboot):
    """The app's (app_version, variant, worker) from its eboot.bin's strings."""
    sys.path.insert(0, str(ROOT / "tools"))
    from check_variant import app_version, segments
    with tempfile.TemporaryDirectory(prefix="vjo-vpk-") as tmp:
        path = Path(tmp) / "eboot.bin"
        path.write_bytes(eboot)
        blob = segments(path)
    version, variant = app_version(blob)
    marks = set(WORKER_MARK.findall(blob))
    if version is None:
        raise ValueError("the Overlay app's eboot.bin names no version (its start line)")
    if len(marks) != 1:
        raise ValueError("the Overlay app's eboot.bin has no single vjoworker= mark (settings/install_page.c)")
    return version, variant, marks.pop().decode()


# ---------------- the manifest ----------------

def app_facts(app_vpk):
    """The Overlay app's (app_version, variant, worker, api, build) from its VPK's bytes: its eboot.bin's
    start line and vjoworker= mark, and its stamp (which must be its eboot's). ValueError for a VPK that
    is not a stamped app of this tree (an older base, a test's placeholder)."""
    try:
        with zipfile.ZipFile(io.BytesIO(app_vpk)) as vpk:
            eboot = vpk.read("eboot.bin")
            stamp = vpk.read("vjo_app.txt").decode("ascii").strip()
    except (zipfile.BadZipFile, KeyError) as e:
        raise ValueError(f"{APP_VPK} is not a stamped Overlay app ({e})")
    try:
        app_version, variant, worker = eboot_facts(eboot)
    except (struct.error, zlib.error, IndexError) as e:
        raise ValueError(f"the Overlay app's eboot.bin is not a Vita SELF ({e})")
    m = re.fullmatch(r"vjoapp1 api=(\d+) build=([0-9a-f]{16})", stamp)
    if not m or m.group(2) != sha256(eboot)[:16]:
        raise ValueError("the Overlay app's stamp is not its eboot.bin's")
    return app_version, variant, worker, int(m.group(1)), m.group(2)


def build_manifest(version, app_vpk, entries, install_settings=None):
    """The payload manifest's text: entries maps each payload file's zip name (ur0/tai/...) to its
    (bytes, sha256), as the zip manifest records them; the app's facts come from app_vpk itself."""
    app_version, variant, worker, api, build = app_facts(app_vpk)
    version = version or app_version
    if not re.fullmatch(r"\d[0-9A-Za-z.+~-]{0,38}", version):
        raise ValueError(f"the package's version {version!r}")
    lines = ["vjopayload 1", f"version {version}", f"app_version {app_version}", f"variant {variant}",
             f"api {api}", f"worker {worker}", f"build {build}"]
    for dest, kind in payload_files():
        name = zip_path(dest)
        if name not in entries:
            raise ValueError(f"the package has no {name}")
        size, digest = entries[name]
        lines.append(f"template {dest} {size} {digest}" if kind == "template" else f"file {dest} {size} {digest} {kind}")
    sets = dict(install_settings or {})
    if variant == "debug":
        sets.setdefault("log_file", "on")  # the debug build is the one asked for when a full log is needed
    for key, value in sorted(sets.items()):
        if key in ("anki_mining", "log_file") and value in ("on", "off"):
            lines.append(f"set {key} {value}")
    text = "\n".join(lines) + "\n"
    if len(text.encode()) > 8192 or len(lines) > 64 or max(len(l) for l in lines) > 255:
        raise ValueError("the payload manifest is over its bounds (core/payload.h)")
    return text


def licence_files(release):
    """{zip name: bytes} of a package_release.py zip's licence and attribution files, each checked against
    the zip manifest; ValueError when one of REQUIRED_LICENSES is missing or a file is not its bytes."""
    with zipfile.ZipFile(release) as z:
        manifest = json.loads(z.read("package-manifest.json"))
        out = {}
        for name in sorted(n for n in z.namelist() if licence_name(n)):
            data = z.read(name)
            want = manifest["files"].get(name)
            if not want or want["bytes"] != len(data) or want["sha256"] != sha256(data):
                raise ValueError(f"{name}: not the zip manifest's bytes")
            out[name] = data
    missing = [n for n in REQUIRED_LICENSES if n not in out]
    if missing:
        raise ValueError(f"the release zip has no {', '.join(missing)}: the VPK must carry its licences")
    return out


def payload_manifest(release):
    """(the payload manifest's text, {zip name: bytes} of the payload, the app's VPK bytes) for a
    package_release.py zip; its own payload-manifest.txt, when it has one, must be the same text."""
    with zipfile.ZipFile(release) as z:
        names = set(z.namelist())
        manifest = json.loads(z.read("package-manifest.json"))
        app = z.read(APP_VPK)
        files, entries = {}, {}
        for dest, _ in payload_files():
            name = zip_path(dest)
            if name not in names:
                raise ValueError(f"the release zip has no {name}")
            data = z.read(name)
            want = manifest["files"].get(name)
            if not want or want["bytes"] != len(data) or want["sha256"] != sha256(data):
                raise ValueError(f"{name}: not the zip manifest's bytes")
            files[name] = data
            entries[name] = (len(data), want["sha256"])
        recorded = z.read("payload-manifest.txt").decode("ascii") if "payload-manifest.txt" in names else None
    if manifest.get("variant") not in (None, app_facts(app)[1]):
        raise ValueError(f"the zip manifest's variant {manifest.get('variant')} is not the app's")
    text = build_manifest(manifest.get("version"), app, entries, manifest.get("install_settings"))
    if recorded is not None and recorded != text:
        raise ValueError("the zip's payload-manifest.txt is not the payload manifest of its own files")
    return text, files, app


def installed_record(text):
    """installed.txt's line for a payload manifest (core/payload.c vjo_installed_format)."""
    fields = dict(line.split(" ", 1) for line in text.splitlines()[1:] if line.split(" ", 1)[0] in
                  ("version", "variant", "api", "build"))
    return (f"vjoinst1 version={fields['version']} variant={fields['variant']} api={fields['api']} "
            f"build={fields['build']} manifest={sha256(text.encode())}\n")


# ---------------- the VPK ----------------

def write_vpk(release, output):
    text, files, app = payload_manifest(release)
    licences = licence_files(release)
    output.parent.mkdir(parents=True, exist_ok=True)
    tmp = output.with_name(output.name + ".tmp")
    with zipfile.ZipFile(io.BytesIO(app)) as original, zipfile.ZipFile(tmp, "w") as vpk:
        for info in original.infolist():
            vpk.writestr(info, original.read(info))  # the app as built: names, order, compression
        for name, data in files.items():
            info = zipfile.ZipInfo("payload/" + name, (1980, 1, 1, 0, 0, 0))
            # deflated, as in the release zip: the dictionary's 143 MB is about 43 MB so, which is what
            # the user downloads and copies to the Vita (VitaShell inflates it as it extracts)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            vpk.writestr(info, data)
        info = zipfile.ZipInfo(MANIFEST, (1980, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o100644 << 16
        vpk.writestr(info, text)
        for name, data in licences.items():  # after the payload: the app reads none of them
            info = zipfile.ZipInfo(LICENSES + name, (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            vpk.writestr(info, data)
    problems = verify(tmp)
    if problems:
        tmp.unlink()
        raise ValueError("the VPK failed its checks:\n  " + "\n  ".join(problems))
    tmp.replace(output)
    return text


def verify(path):
    """Every problem with a one-VPK package (empty: none)."""
    problems = []
    with zipfile.ZipFile(path) as vpk:
        infos = vpk.infolist()
        names = [i.filename for i in infos]
        if names[:2] != ["sce_sys/param.sfo", "eboot.bin"] and names[:2] != ["eboot.bin", "sce_sys/param.sfo"]:
            problems.append(f"the first entries are {names[:2]}: VitaShell's scan must stop at the app's eboot.bin")
        if any(i.file_size >= 0xFFFFFFFF or i.compress_size >= 0xFFFFFFFF for i in infos) or len(infos) >= 0xFFFF:
            problems.append("a zip64 entry")
        if any(i.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED) for i in infos):
            problems.append("an entry neither stored nor deflated")
        if MANIFEST not in names:
            return problems + ["no payload/manifest.txt: an app without a payload"]
        missing = [LICENSES + n for n in REQUIRED_LICENSES if LICENSES + n not in names]
        if missing:
            problems.append(f"no {', '.join(missing)}: the VPK must carry its licences")
        problems += [f"{n}: not a licence or attribution file" for n in names
                     if n.startswith(LICENSES) and not licence_name(n[len(LICENSES):])]
        text = vpk.read(MANIFEST).decode("ascii")
        listed = {}
        for line in text.splitlines()[1:]:
            parts = line.split(" ")
            if parts[0] in ("file", "template"):
                listed["payload/" + zip_path(parts[1])] = (int(parts[2]), parts[3])
        want = {"payload/" + zip_path(dest) for dest, _ in payload_files()}
        if set(listed) != want:
            problems.append(f"the manifest's files are not the app's list: {sorted(set(listed) ^ want)}")
        payload = [n for n in names if n.startswith("payload/") and n != MANIFEST]
        if set(payload) != want:
            problems.append(f"the payload's files are not the app's list: {sorted(set(payload) ^ want)}")
        for name, (size, digest) in listed.items():
            if name in names:
                data = vpk.read(name)
                if len(data) != size or sha256(data) != digest:
                    problems.append(f"{name}: not the manifest's bytes")
        eboot = vpk.read("eboot.bin")
        try:
            version, variant, worker = eboot_facts(eboot)
            stamp = vpk.read("vjo_app.txt").decode("ascii").strip()
            fields = dict(l.split(" ", 1) for l in text.splitlines()[1:] if " " in l and l.split(" ", 1)[0] in
                          ("app_version", "variant", "worker", "build", "api"))
            if (fields.get("app_version"), fields.get("variant"), fields.get("worker")) != (version, variant, worker):
                problems.append(f"the manifest's app_version/variant/worker {fields.get('app_version')}/"
                                f"{fields.get('variant')}/{fields.get('worker')} are not the app's {version}/{variant}/{worker}")
            if f"vjoapp1 api={fields.get('api')} build={fields.get('build')}" != stamp:
                problems.append("the manifest's api/build are not the app's stamp")
        except (ValueError, KeyError) as e:
            problems.append(str(e))
        # the manifest is exactly the one this tool makes of the VPK's own app and payload files (the kinds,
        # the order, every line; core/payload.c refuses anything else): only its version and set lines come
        # from the package, and they must have the shapes the app accepts
        problems += manifest_problems(vpk, text, Path(path).name)
        for name in names:
            if name.endswith((".suprx", ".skprx")) or name == "eboot.bin":
                try:
                    problems += self_problems(name, vpk.read(name), plugin=name != "eboot.bin")
                except (ValueError, struct.error) as e:
                    if not name.startswith("payload/") or vpk.read(name)[:4] == b"SCE\0":
                        problems.append(f"{name}: {e}")
    return problems


def manifest_problems(vpk, text, file_name):
    """The payload manifest against the one build_manifest makes of this VPK's own app and payload files."""
    lines = text.splitlines()
    version = next((l.split(" ", 1)[1] for l in lines if l.startswith("version ")), None)
    sets, problems = {}, []
    for l in lines:
        if l.startswith("set "):
            parts = l.split(" ")
            if len(parts) != 3 or parts[1] not in ("anki_mining", "log_file") or parts[2] not in ("on", "off") \
                    or parts[1] in sets:
                problems.append(f"the manifest's line {l!r} is not a setting the app accepts")
            sets[parts[1]] = parts[2] if len(parts) == 3 else ""
    try:
        app = io.BytesIO()
        with zipfile.ZipFile(app, "w") as z:
            z.writestr("eboot.bin", vpk.read("eboot.bin"))
            z.writestr("vjo_app.txt", vpk.read("vjo_app.txt"))
        entries = {}
        for dest, _ in payload_files():
            data = vpk.read("payload/" + zip_path(dest))
            entries[zip_path(dest)] = (len(data), sha256(data))
        rebuilt = build_manifest(version, app.getvalue(), entries, sets)
    except (ValueError, KeyError) as e:
        return problems + [f"the manifest cannot be made again from the VPK's own files: {e}"]
    if rebuilt != text:
        diff = [l for l in text.splitlines() if l not in rebuilt.splitlines()] + \
               [f"(want) {l}" for l in rebuilt.splitlines() if l not in text.splitlines()]
        problems.append("the manifest is not the one this VPK's own app and files make: " + "; ".join(diff[:4]))
    # the release names its VPKs VitaJPOverlay-<version>.vpk and -<version>-debug.vpk (package_variants.vpk_name)
    m = re.fullmatch(r"VitaJPOverlay-(.+?)(-debug)?\.vpk", file_name)
    if m and version is not None and m.group(1) != version:
        problems.append(f"the file is named for version {m.group(1)}, the manifest says {version}")
    return problems


def check_build(eboot, plugins, worker):
    """The Vita build's check (vita/CMakeLists.txt vjo_one_vpk): the app names one worker mode, the one
    its include/vjo_api.h compiled (worker), and no SELF has what VitaShell's scan warns of (the app may
    import SceShellSvc: its install's PS button lock)."""
    problems = []
    try:
        _, _, mark = eboot_facts(Path(eboot).read_bytes())
        if mark != worker:
            problems.append(f"{eboot}: vjoworker={mark}, but the tree's VJO_KERNEL_LOADS_WORKER says {worker}")
    except (ValueError, struct.error) as e:
        problems.append(f"{eboot}: {e}")
    for path in [eboot, *plugins]:
        try:
            problems += self_problems(Path(path).name, Path(path).read_bytes(), plugin=path != eboot)
        except (ValueError, struct.error) as e:
            problems.append(f"{path}: {e}")
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("release", type=Path, nargs="?", help="A package_release.py zip")
    parser.add_argument("-o", "--output", type=Path)
    parser.add_argument("--verify", type=Path, metavar="VPK")
    parser.add_argument("--manifest", type=Path, metavar="ZIP", help="Print the payload manifest of a release zip")
    parser.add_argument("--installed-record", type=Path, metavar="ZIP_OR_DIR",
                        help="Print installed.txt's line for a release zip or its extracted folder")
    parser.add_argument("--check-build", nargs="+", type=Path, metavar="SELF",
                        help="The Vita build's check: EBOOT.BIN then the plugins (with --worker)")
    parser.add_argument("--worker", choices=("kernel", "lines"), help="The worker mode the tree compiled")
    args = parser.parse_args()
    try:
        if args.check_build:
            if not args.worker:
                parser.error("--check-build needs --worker")
            problems = check_build(args.check_build[0], args.check_build[1:], args.worker)
            for p in problems:
                print(p, file=sys.stderr)
            if problems:
                sys.exit(1)
            print(f"The one-VPK install's modules: vjoworker={args.worker}; no SELF has what VitaShell warns of")
        elif args.verify:
            problems = verify(args.verify)
            for p in problems:
                print(p, file=sys.stderr)
            if problems:
                sys.exit(1)
            print(f"Verified {args.verify}")
        elif args.manifest:
            print(payload_manifest(args.manifest)[0], end="")
        elif args.installed_record:
            path = args.installed_record
            text = (path / "payload-manifest.txt").read_text() if path.is_dir() else payload_manifest(path)[0]
            print(installed_record(text), end="")
        elif args.release and args.output:
            text = write_vpk(args.release, args.output)
            print(json.dumps({"file": str(args.output), "bytes": args.output.stat().st_size,
                              "sha256": sha256(args.output.read_bytes()), "payload_manifest_sha256": sha256(text.encode())}))
        else:
            parser.error("give RELEASE.zip -o OUT.vpk, --verify VPK, --manifest ZIP or --installed-record ZIP_OR_DIR")
    except (ValueError, KeyError, zipfile.BadZipFile) as error:
        sys.exit(f"package_vpk: {error}")


if __name__ == "__main__":
    main()
