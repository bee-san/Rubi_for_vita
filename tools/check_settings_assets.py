#!/usr/bin/env python3
"""Validate the actual settings VPK's Vita LiveArea PNGs, without dependencies.

The Vita promoter requires 8-bit indexed, non-interlaced PNGs. See
https://github.com/hammerill/livearea-specs for the native asset constraints.
Sony's content information specification also limits icon0.png to 32 KiB and
the start-up image pic0.png to 600 KiB, and asks both to hold only the data
needed for display: an image editor's other information "may cause an error"
when the package is made. The artwork may therefore carry no text chunk
(tEXt, iTXt, zTXt: a tool's name, a comment), in any of the four images.
Pass the assets directory to check committed artwork before a Vita build.

A VPK's sce_sys/param.sfo is checked too: its TITLE_ID must stay VJOSET001, the ID every earlier build's
app has, or the VPK installs as a second app beside the old one instead of updating it
(docs/rename-rubi.md), and its LiveArea title (TITLE, STITLE) is the app's name, Rubi.
"""
import argparse
import binascii
import json
from pathlib import Path
import struct
import zipfile


ASSETS = {
    "sce_sys/icon0.png": (128, 128),
    "sce_sys/pic0.png": (960, 544),  # the launch splash: bg.png, scaled
    "sce_sys/livearea/contents/bg.png": (840, 500),
    "sce_sys/livearea/contents/startup.png": (280, 158),
}
MAX_BYTES = {"sce_sys/icon0.png": 32 * 1024, "sce_sys/pic0.png": 600 * 1024}
TEXT_CHUNKS = (b"tEXt", b"iTXt", b"zTXt")
# The app's param.sfo (vita/CMakeLists.txt vita_create_vpk): its ID is every build's, its title the app's name.
PARAM_SFO = {"TITLE_ID": "VJOSET001", "TITLE": "Rubi", "STITLE": "Rubi"}


def check_png(data, name, dimensions, max_bytes=None):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{name}: invalid PNG signature")
    if max_bytes is not None and len(data) > max_bytes:
        raise ValueError(f"{name}: {len(data)} bytes; the Vita allows at most {max_bytes}")
    position, chunks = 8, []
    while position < len(data):
        if len(data) - position < 12:
            raise ValueError(f"{name}: truncated PNG chunk")
        size = struct.unpack_from(">I", data, position)[0]
        kind = data[position + 4:position + 8]
        end = position + 8 + size
        if end + 4 > len(data):
            raise ValueError(f"{name}: truncated PNG payload")
        payload = data[position + 8:end]
        crc = struct.unpack_from(">I", data, end)[0]
        if binascii.crc32(kind + payload) & 0xFFFFFFFF != crc:
            raise ValueError(f"{name}: PNG checksum mismatch")
        chunks.append((kind, payload))
        position = end + 4
        if kind == b"IEND":
            if size or position != len(data):
                raise ValueError(f"{name}: invalid PNG ending")
            break
    if not chunks or chunks[0][0] != b"IHDR" or len(chunks[0][1]) != 13:
        raise ValueError(f"{name}: missing or invalid IHDR")
    width, height, depth, color, compression, filtering, interlace = struct.unpack(
        ">IIBBBBB", chunks[0][1])
    if (width, height) != dimensions:
        raise ValueError(f"{name}: expected {dimensions[0]}x{dimensions[1]}, got {width}x{height}")
    if depth != 8 or color != 3 or compression or filtering or interlace:
        raise ValueError(f"{name}: expected 8-bit indexed, non-interlaced PNG; "
                         f"got depth={depth}, color_type={color}, interlace={interlace}")
    palettes = [payload for kind, payload in chunks if kind == b"PLTE"]
    if len(palettes) != 1 or not 3 <= len(palettes[0]) <= 768 or len(palettes[0]) % 3:
        raise ValueError(f"{name}: missing or invalid indexed palette")
    if any(value != 255 for kind, payload in chunks if kind == b"tRNS" for value in payload):
        raise ValueError(f"{name}: settings artwork must be opaque")
    texts = [kind.decode("ascii") for kind, _ in chunks if kind in TEXT_CHUNKS]
    if texts:
        raise ValueError(f"{name}: a {texts[0]} chunk; settings artwork holds display data only")
    if chunks[-1][0] != b"IEND" or not any(kind == b"IDAT" for kind, _ in chunks):
        raise ValueError(f"{name}: incomplete PNG")
    return {"width": width, "height": height, "bit_depth": depth,
            "color_type": color, "interlaced": False, "opaque": True, "bytes": len(data)}


def sfo_values(data):
    """The string and integer values of a PSF (param.sfo) file: {key: value}."""
    if len(data) < 20 or data[:4] != b"\0PSF":
        raise ValueError("sce_sys/param.sfo: not a PSF file")
    _, _, keys, values, count = struct.unpack_from("<4sIIII", data, 0)
    out = {}
    for i in range(count):
        key_at, kind, length, _, value_at = struct.unpack_from("<HHIII", data, 20 + 16 * i)
        key = data[keys + key_at:data.index(b"\0", keys + key_at)].decode("ascii")
        value = data[values + value_at:values + value_at + length]
        out[key] = (value.rstrip(b"\0").decode("utf-8") if kind in (0x0004, 0x0204)
                    else struct.unpack("<I", value[:4])[0])
    return out


def check_sfo(data):
    values = sfo_values(data)
    for key, want in PARAM_SFO.items():
        if values.get(key) != want:
            raise ValueError(f"sce_sys/param.sfo: {key} is {values.get(key)!r}, want {want!r}")
    return {key: values[key] for key in PARAM_SFO}


def check(source):
    if source.is_dir():
        return {name: check_png((source / Path(name).name).read_bytes(), name, size, MAX_BYTES.get(name))
                for name, size in ASSETS.items()}
    with zipfile.ZipFile(source) as archive:
        names = archive.namelist()
        for name in ASSETS:
            if names.count(name) != 1:
                raise ValueError(f"{name}: expected exactly one asset in settings VPK")
        if names.count("sce_sys/param.sfo") != 1:
            raise ValueError("sce_sys/param.sfo: expected exactly one in settings VPK")
        check_sfo(archive.read("sce_sys/param.sfo"))
        return {name: check_png(archive.read(name), name, size, MAX_BYTES.get(name))
                for name, size in ASSETS.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Settings VPK or committed assets directory")
    args = parser.parse_args()
    print(json.dumps({"source": str(args.source), "assets": check(args.source)}, indent=2))


if __name__ == "__main__":
    main()
