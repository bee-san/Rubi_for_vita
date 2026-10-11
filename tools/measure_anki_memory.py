#!/usr/bin/env python3
"""Host-only Anki memory probe using an isolated, disposable collection.

Never opens an existing profile, contacts AnkiWeb or uses account credentials.
Peak RSS includes Python and native Anki; it is not a Vita memory measurement.
Run on Linux with the official `anki` package installed.
"""
import argparse
import json
from pathlib import Path
import resource
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--notes", type=int, default=10000)
    args = parser.parse_args()
    if not 0 <= args.notes <= 100000:
        parser.error("--notes must be between 0 and 100000")
    def sample(stage):
        print(json.dumps({"stage": stage,
                          "peak_rss_kib": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss}), flush=True)
    sample("python")
    from anki.collection import Collection
    sample("import")
    with tempfile.TemporaryDirectory(prefix="vjo-anki-memory-") as directory:
        path = Path(directory) / "collection.anki2"
        collection = Collection(str(path))
        try:
            sample("open")
            model = collection.models.by_name("Basic")
            for index in range(args.notes):
                note = collection.new_note(model)
                note["Front"] = f"猫がいる。 {index}"
                note["Back"] = "synthetic Japanese definition 日本語 " * 12
                collection.add_note(note, 1)
            sample("notes")
        finally:
            collection.close()
        print(json.dumps({"notes": args.notes, "collection_bytes": path.stat().st_size}))


if __name__ == "__main__":
    main()
