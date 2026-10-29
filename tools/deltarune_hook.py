#!/usr/bin/env python3
"""Prepare an experimental Deltarune v0.73 profile without replacing plugins.

This is an offline prototype, not a console-verified hook. It observes the
runner's _strdup_r input to find complete starred dialogue strings; renderer
hooks alone can see individual typewriter glyphs. Never install automatically.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from vita_self import read as read_self
from hook_profiles import revision as worker_revision

TITLE = "DELT00001"
EBOOT_SHA256 = "9d9c7a6d464850b5b8140ccfefc8d38c603c5f81a06ed64408e93753be4710b2"
MODULE_NID = 0x8D433617
SITE = 0x1B11F0
# push {r4,r5,r6,lr}; mov r4,r0; mov r0,r1; mov r5,r1.
# The following BL has a SELF relocation and is deliberately outside the signature.
SIGNATURE = bytes.fromhex("70b5044608460d46")
FILTER = 1  # Local to this title's profile, not a built-in filter identifier.
BASE_REVISION = "a683b691151ca70883cd9a5a694c84545753902c0e9461f30d0e7e13a2ec3ced"


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def catalogue(data):
    data = bytes(data)
    if len(data) < 16 or data[:8] != b"vjohkdb1" or any(data[12:16]):
        raise ValueError("invalid hook archive header")
    count, = struct.unpack_from("<I", data, 8)
    if not 1 <= count <= 4096 or 16 + 20 * count > len(data):
        raise ValueError("invalid hook archive index")
    entries, revisions = {}, set()
    previous, expected = "", 16 + 20 * count
    for index in range(count):
        raw, offset, size = struct.unpack_from("<12sII", data, 16 + 20 * index)
        name = raw[:9].decode("ascii")
        if (not name.isalnum() or not name.isupper() or any(raw[9:]) or name <= previous
                or offset != expected or not 73 <= size <= 1024 or offset + size > len(data)):
            raise ValueError("invalid hook archive row")
        blob = data[offset:offset + size]
        if (blob[:8] != b"vjohook\1" or blob[8:17] != raw[:9] or any(blob[17:20])
                or struct.unpack_from("<I", blob, 52)[0] != size
                or struct.unpack_from("<I", blob, 68)[0] != checksum(blob[72:])):
            raise ValueError("invalid title profile")
        entries[name] = blob
        revisions.add(blob[20:52])
        previous, expected = name, offset + size
    if len(revisions) != 1 or expected != len(data):
        raise ValueError("mixed revisions or trailing archive bytes")
    return entries, revisions.pop()


def profile(revision):
    if len(revision) != 32:
        raise ValueError("profile revision must have 32 bytes")
    # Existing Rubi filter VM operations. Only strings beginning with a star
    # after leading formatting commands qualify. No changes to the worker ABI.
    operations = [
        (4, r"\\[A-Za-z][0-9]*", ""),  # OP_SUBW: expression/colour codes
        (4, r"\^[0-9]+", ""),          # OP_SUBW: pauses
        (2, "＊", "*"),                 # OP_REP: Japanese dialogue bullet
        (15, "", ""),                  # OP_TRIM: leading layout spaces
        (7, "*", ""),                   # OP_PREFIX: reject ordinary strings
        (9, "*", ""),                   # OP_AFTER: remove the dialogue bullet
        (4, r"[/%]+$", ""),             # OP_SUBW: trailing message terminators
        (2, "&", "\n"),                # OP_REP: line break
        (15, "", ""),                  # OP_TRIM
        (17, "", ""),                  # OP_ASCII: Japanese mode only
        (0, "", ""),                   # OP_END
    ]
    strings, offsets = bytearray(b"\0"), {"": 0}
    def string(value):
        if value not in offsets:
            offsets[value] = len(strings)
            strings.extend(value.encode("utf-8") + b"\0")
        return offsets[value]
    ops = bytearray()
    for op, a, b in operations:
        ops.extend(struct.pack("<BBBBHH", op, 0, 0, 0, string(a), string(b)))
    row = bytearray(36)
    struct.pack_into("<III4B", row, 0, MODULE_NID, SITE, 0, 1, 1, 1, len(SIGNATURE))
    row[16:24] = SIGNATURE
    row[32] = FILTER
    row[33] = 5  # VJO_PLACE_READ_DELTARUNE: reject unrelated copies at capture time.
    struct.pack_into("<H", row, 34, 0x0073)
    body = row + struct.pack("<HH", FILTER, 0) + ops + strings
    header = bytearray(72)
    header[:8] = b"vjohook\1"
    header[8:17] = TITLE.encode("ascii")
    header[20:52] = revision
    struct.pack_into("<I5H", header, 52, 72 + len(body), 0, 1, 1, len(operations), len(strings))
    struct.pack_into("<I", header, 68, checksum(body))
    return bytes(header + body)


def prepare(base, eboot):
    if hashlib.sha256(eboot).hexdigest() != EBOOT_SHA256:
        raise ValueError("eboot is not the pinned Deltarune v0.73 release")
    sce = read_self(eboot)
    code = sce.segments[0].data
    info = sce.entry & 0x3FFFFFFF
    if (sce.entry >> 30 or struct.unpack_from("<I", code, info + 0x34)[0] != MODULE_NID
            or code[SITE:SITE + len(SIGNATURE)] != SIGNATURE):
        raise ValueError("module identity or hook signature differs")
    # Standard vita-elf-create relocations: only the first eight bytes are
    # moved by taiHEN here. The BL at +8 must remain in the game.
    for segment in sce.segments:
        if segment.kind != 0x60000000:
            continue
        if len(segment.data) % 12:
            raise ValueError("unsupported relocation format")
        for word, addend, place in struct.iter_unpack("<III", segment.data):
            if word & 15 or word >> 20:
                raise ValueError("unsupported optimized relocation")
            if (word >> 16) & 15 == 0 and place < SITE + len(SIGNATURE) and place + 4 > SITE:
                raise ValueError("relocation overlaps the hook patch")
    entries, old_revision = catalogue(base)
    revision = bytes.fromhex(worker_revision())
    if old_revision.hex() not in (BASE_REVISION, revision.hex()):
        raise ValueError("unsupported source catalogue revision; cannot migrate its definitions")
    if TITLE in entries:
        raise ValueError("archive already has Deltarune; refusing to replace it")
    preserved = {name: hashlib.sha256(blob[72:]).hexdigest() for name, blob in entries.items()}
    # This change adds one read mode without changing existing programs or
    # record layouts. Migrate only the known previous revision's header; all
    # existing title-specific definitions remain byte-identical.
    for name, blob in entries.items():
        migrated = bytearray(blob)
        migrated[20:52] = revision
        entries[name] = bytes(migrated)
    entries[TITLE] = profile(revision)
    output = bytearray(b"vjohkdb1" + struct.pack("<II", len(entries), 0))
    offset = 16 + 20 * len(entries)
    for name, blob in sorted(entries.items()):
        output.extend(struct.pack("<12sII", name.encode("ascii"), offset, len(blob)))
        offset += len(blob)
    for _, blob in sorted(entries.items()):
        output.extend(blob)
    checked, checked_revision = catalogue(output)
    assert checked_revision == revision
    assert all(hashlib.sha256(checked[name][72:]).hexdigest() == digest for name, digest in preserved.items())
    return bytes(output), {
        "status": "experimental; no console text verification",
        "title_id": TITLE, "app_ver": "00.73", "module_nid": f"{MODULE_NID:08X}",
        "site_offset": f"{SITE:08X}", "register": 1, "encoding": "UTF8",
        "signature": SIGNATURE.hex(), "relocations_overlapping_patch": 0,
        "profile_revision": revision.hex(), "source_profile_revision": old_revision.hex(),
        "preserved_title_definition_bodies": preserved,
        "existing_profile_changes": "only revision bytes 20..51; definitions and checksums preserved",
        "eboot_sha256": EBOOT_SHA256, "archive_sha256": hashlib.sha256(output).hexdigest(),
        "limitations": ["only starred Japanese strings", "may capture text before it is visible",
                        "may capture preloaded dialogue; gameplay validation required",
                        "menus and non-starred dialogue are not covered"],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", type=Path, required=True, help="backed-up installed profiles.vjhp")
    parser.add_argument("--eboot", type=Path, required=True, help="installed DELT00001/eboot.bin")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    # Never overwrite a prior catalogue or receipt, including the input.
    receipt = args.output.with_suffix(args.output.suffix + ".json")
    if args.output.exists() or receipt.exists():
        parser.error("output archive or receipt already exists")
    result, manifest = prepare(args.base.read_bytes(), args.eboot.read_bytes())
    with args.output.open("xb") as stream:
        stream.write(result)
    with receipt.open("x") as stream:
        json.dump(manifest, stream, indent=2)
        stream.write("\n")
    print(f"Prepared experimental {TITLE} profile; preserved {len(manifest['preserved_title_definition_bodies'])} title definitions")
    print("Requires the matching worker built from this checkout. Neither worker nor catalogue is installed.")


if __name__ == "__main__":
    main()
