#!/usr/bin/env python3
"""Refuse an Adrenaline Shell bridge that exceeds its 8 KiB resident budget.

Reads the built SELF's PT_LOAD memory sizes, including BSS. This is a build
measurement, not a console free-memory measurement.
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vita_self  # noqa: E402  (tools/vita_self.py, beside this file in the package's tools/ too)

LIMIT = 8192


def check(path):
    sce = vita_self.read(Path(path).read_bytes(), headers_only=True)  # vita_self.Invalid is a ValueError
    segments = [vita_self.page_round(s.memsz) for s in sce.segments if s.kind == vita_self.PT_LOAD]
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
    except ValueError as error:
        parser.exit(1, f"{error}\n")
