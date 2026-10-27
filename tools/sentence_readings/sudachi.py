#!/usr/bin/env python3
"""The morphological analyser's readings for the sentence-readings measurement (compare.py): SudachiPy's tokens
of every line, split mode C, with their readings.

  python3 tools/sentence_readings/sudachi.py lines.jsonl -o sudachi.jsonl

Each output line is {"id", "tokens": [[begin, end, reading], ...]}: code points of the line, the reading in
katakana as Sudachi gives it ("" for an unknown word, whose reading is no reading). Needs sudachipy and
sudachidict_core (tools/yomitan_check pins 0.7.0 and 20261015); their versions go to stderr.
"""
import argparse
import importlib.metadata
import json
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("lines", type=Path)
    parser.add_argument("-o", "--output", required=True, type=Path)
    args = parser.parse_args()
    from sudachipy import Dictionary, SplitMode
    tokenizer = Dictionary(dict="core").create()  # noqa: the 0.7 API that still works
    out = []
    for row in args.lines.read_text(encoding="utf-8").splitlines():
        if not row.strip():
            continue
        line = json.loads(row)
        tokens = [[m.begin(), m.end(), "" if m.is_oov() else m.reading_form()]
                  for m in tokenizer.tokenize(line["text"], SplitMode.C)]
        out.append(json.dumps({"id": line["id"], "tokens": tokens}, ensure_ascii=False))
    args.output.write_text("".join(x + "\n" for x in out), encoding="utf-8")
    print(f"sudachipy {importlib.metadata.version('sudachipy')}, sudachidict_core "
          f"{importlib.metadata.version('sudachidict_core')}, split mode C: {len(out)} lines", file=sys.stderr)


if __name__ == "__main__":
    main()
