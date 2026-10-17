#!/usr/bin/env python3
"""Refuse an Adrenaline Shell bridge that exceeds its 8 KiB resident budget.

Reads the built SELF's PT_LOAD memory sizes, including BSS. This is a build
measurement, not a console free-memory measurement.
"""
import argparse
import json
import struct
from pathlib import Path

LIMIT = 8192


def check(path):
    data = Path(path).read_bytes()
    if data[:4] != b"SCE\0":
        raise ValueError("not a Vita SELF")
    elf, phdr = struct.unpack_from("<QQ", data, 0x40)
    if data[elf:elf + 6] != b"\x7fELF\x01\x01":
        raise ValueError("not a little-endian ELF32")
    entry_size, count = struct.unpack_from("<HH", data, elf + 42)
    if entry_size != 32 or not 1 <= count <= 16:
        raise ValueError("invalid program headers")
    segments = []
    for i in range(count):
        kind, _, _, _, _, memsz, _, _ = struct.unpack_from("<8I", data, phdr + i * 32)
        if kind == 1:
            segments.append((memsz + 4095) & ~4095)
    total = sum(segments)
    if not total or total > LIMIT:
        raise ValueError(f"Adrenaline Shell bridge uses {total} B of pages; limit {LIMIT} B")
    return {"pages": total, "limit": LIMIT, "segments": segments}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("self", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(check(args.self)))
    except (ValueError, struct.error) as error:
        parser.exit(1, f"{error}\n")
