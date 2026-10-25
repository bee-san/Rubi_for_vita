#!/usr/bin/env python3
"""Regenerate third_party/vndb-names/games-common-words.txt: the per-game name keys that are among
the word dictionary's most frequent words, which the per-game names pack leaves out.

Each key of the vendored VNDB-Names-Games.zip is looked up alone in plaintext.vjdict with the host
vjo-cli; a key whose best match is that very word with a frequency rank of LIMIT or better is kept.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "third_party/vndb-names/games-common-words.txt"
HEADER = """# Per-game name keys that are among the word dictionary's 3,000 most frequent words (plaintext.vjdict
# frequency rank, after the tab). tools/prepare_vndb.py leaves them out of vndb-games.vjgames, so in a
# game whose character is called 一 or 光 the word comes first and the name stays an L/R page.
# Regenerate: python3 tools/vndb_games_guard.py --cli build/host/vjo-cli --dict-dir DIR_WITH_PLAINTEXT
"""


def keys(archive):
    with zipfile.ZipFile(archive) as z:
        return sorted({row[0] for game in json.loads(z.read("games.json")) for row in game["terms"]})


def rank(cli, dict_dir, key):
    out = subprocess.run([str(cli), "--text", key, "--dict", "local", "--local-dir", str(dict_dir),
                          "--local-dicts", "plaintext.vjdict"], capture_output=True, text=True).stdout.split("\n")
    head = out[2] if len(out) > 2 else ""
    word = head.split(" (")[0].split(" #")[0].strip()
    match = re.search(r"#(\d+)", head)
    return int(match.group(1)) if word == key and match else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--dict-dir", type=Path, required=True, help="the directory holding plaintext.vjdict")
    parser.add_argument("--archive", type=Path, default=ROOT / "third_party/vndb-names/VNDB-Names-Games.zip")
    parser.add_argument("--limit", type=int, default=3000)
    args = parser.parse_args()
    found = sorted((r, k) for k in keys(args.archive) if (r := rank(args.cli, args.dict_dir, k)) and r <= args.limit)
    OUTPUT.write_text(HEADER + "".join(f"{k}\t{r}\n" for r, k in found), encoding="utf-8")
    print(f"{len(found)} keys written to {OUTPUT}")


if __name__ == "__main__":
    main()
