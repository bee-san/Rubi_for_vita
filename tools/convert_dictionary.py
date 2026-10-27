#!/usr/bin/env python3
"""Convert Yomitan v3 term ZIPs to a bounded-memory Vita dictionary.

Python 3.9+ standard library only. ZIP banks are streamed one row at a time;
SQLite sorts the index on disk, and definitions are never retained wholesale.

With --frequency, a rank-based Yomitan frequency dictionary (term_meta_bank
"freq" rows, such as Jiten's) gives every entry a rank, written as format 3:
each payload starts with one 32-bit frequency word (0 = unranked), so the
rank is read with the entry and nothing about it stays in memory.
"""
import argparse
import io
import json
import math
import os
from pathlib import Path
import re
import shutil
import sqlite3
import struct
import sys
import tempfile
import zipfile
from html.parser import HTMLParser

MAGIC = b"VJDICT1\0"
HEADER = struct.Struct("<8sIIIIQQQQQ")
RECORD = struct.Struct("<QQHHI")  # Version 1 lexical index.
MAGIC_V2 = b"VJDICT2\0"
MAGIC_V3 = b"VJDICT3\0"  # Version 2's layout; each payload starts with a frequency word.
PAGE_SIZE = 4096
# Version 3: the frequency list's title, NUL-padded, in the header page's spare bytes.
FREQ_TITLE = struct.Struct("<64s")
FREQ_TITLE_OFFSET = HEADER.size
FREQ_WORD = struct.Struct("<I")  # bits 0-23 rank (0 = unranked), 24-27 how it matched, 28-31 zero
FREQ_RANK_MAX = (1 << 24) - 1
# The rank is borrowed: a kanji spelling with no rank of its own for its
# reading takes the word's rank written in kana (Jiten shows it as N㋕). The
# overlay ranks borrowed ranks below every entry's own.
FREQ_BORROWED = 1 << 24
# Bits 25-26, the rows that decided: 1 reading-specific, 2 spelling-only, 3 the
# spelling's reading-specific rows for an entry that has no reading.
FREQ_READING = 1 << 25
FREQ_SPELLING = 1 << 26
FREQ_NORMALIZED = 1 << 27  # matched only after kana normalisation
MATCHES = {FREQ_READING: "reading", FREQ_SPELLING: "spelling", FREQ_READING | FREQ_SPELLING: "spelling_readings"}
MAX_FREQ_TERM = 256
HASH_RECORD = struct.Struct("<QQIHH")
POSTING = struct.Struct("<QIHH")
PAGE_HEADER = struct.Struct("<4sIQ")
HASH_PAGE_RECORDS = (PAGE_SIZE - PAGE_HEADER.size) // HASH_RECORD.size
MAX_HASH_CHAIN = 64
MAX_KEY = 192
MAX_SCAN_CHARS = 32
MAX_ENTRY = 8192
MAX_ROW_CHARS = 8 * 1024 * 1024
MAX_INDEX_BYTES = 1024 * 1024
# A file is under 4 GiB: the Shell's word locators keep 32-bit payload offsets (core/local_dict.c
# Located, #360), and the overlay refuses a larger file as an invalid dictionary.
MAX_FILE_SIZE = (1 << 32) - 1
TOO_LARGE = "the dictionary would be 4 GiB or larger; Rubi reads dictionary files under 4 GiB"
RULES = {"v1": 1, "v5": 2, "vs": 4, "vk": 8, "vz": 16, "adj-i": 32, "iru": 64}
# Key filter (docs/local-dictionaries.md): page-sized Bloom blocks after all other data,
# announced in page 0 after the header. Readers that predate it read only the header.
FILTER_MAGIC = b"VJKF"
FILTER_DESCRIPTOR = struct.Struct("<4sIIIQII")  # magic, version 1, blocks, hashes, offset, keys, 0
FILTER_DESCRIPTOR_OFFSET = 128
# Page-0 bytes 192..255 belong to the dictionary's title (bee-san/plaintext-dicts), not the filter.
FILTER_TITLE_GAP = (192, 256)
FILTER_DIRECTORY_OFFSET = 256  # one u32 first prefix per block
FILTER_MAX_BLOCKS = (PAGE_SIZE - FILTER_DIRECTORY_OFFSET) // 4
FILTER_BLOCK_BITS = PAGE_SIZE * 8
FILTER_HASHES = 7
FILTER_BITS_PER_KEY = 10
# Below this many hash buckets the few index pages mostly stay in the lookup's page cache and
# the filter's own block read costs more than it saves (JMdict subsets: 2 buckets 2.82 -> 3.08
# reads a lookup, 4 buckets 4.07 -> 3.16; reports/dict-lookup-speed.md, 6.1).
FILTER_MIN_BUCKETS = 4
# Posting hints (version 3): each posting's last u16 ranks it without reading its payload's
# head (core/local_dict.c rank_posting), announced by HINTS_MAGIC at page-0 byte 160.
HINTS_MAGIC = b"VJPH"
HINTS_DESCRIPTOR = struct.Struct("<4sI")  # magic, version 1
HINTS_DESCRIPTOR_OFFSET = 160
HINT_SPELLING, HINT_NORMALIZED, HINT_READING, HINT_FREQ_SHIFT = 1, 2, 4, 3
HEAD_BYTES = 160  # the reader's head window after the frequency word (core/local_dict.c HEAD_BYTES)


def normalize(text):
    """Must match core/local_dict.c, without lossy/cross-platform collation."""
    chars = []
    for char in text:
        cp = ord(char)
        if 0x30A1 <= cp <= 0x30F6:
            cp -= 0x60
        if 0xFF01 <= cp <= 0xFF5E:
            cp -= 0xFEE0
        chars.append(chr(cp))
    return "".join(chars).encode("utf-8")


def clean(text):
    return "".join(c for c in text if c in "\n\t" or ord(c) >= 0x20).replace("\x7f", "")


def utf8_clip(text, size):
    return text.encode("utf-8")[:size].decode("utf-8", "ignore")


class PlainHTML(HTMLParser):
    """HTML glossary strings become plain text: markup, CSS, scripts and ruby
    readings are dropped at conversion time, so the Vita never parses them."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.parts = []
        self.hidden = 0
        self.annotation = False  # inside <rt>/<rp>; HTML may omit their end tags

    def handle_starttag(self, tag, attrs):
        if tag in ("script", "style"):
            self.hidden += 1
        if tag in ("rt", "rp"):
            self.annotation = True
        if not self.hidden and tag in ("br", "div", "p", "li", "tr"):
            self.parts.append("\n")
        if tag == "img" and not self.hidden and not self.annotation:
            attributes = dict(attrs)
            self.parts.append(attributes.get("alt") or attributes.get("title") or "[image omitted]")

    def handle_endtag(self, tag):
        if tag in ("script", "style"):
            self.hidden = max(0, self.hidden - 1)
        if tag in ("rt", "rp", "ruby"):
            self.annotation = False
        if not self.hidden and tag in ("div", "p", "li", "tr"):
            self.parts.append("\n")

    def handle_data(self, data):
        if not self.hidden and not self.annotation:
            self.parts.append(data)


def joins_words(left, right):
    """Adjacent inline elements whose text would otherwise run together, such
    as the labels "noun" + "kana" or "See also" + "ぽとり", get one space.
    Japanese next to Japanese (ruby bases, split sentences) stays unspaced."""
    if not left or not right:
        return False
    a, b = left[-1], right[0]
    return a.isalnum() and b.isalnum() and (a.isascii() or b.isascii())


def list_marker(style):
    """A quoted CSS list marker (Jitendex senses use '"①"') as plain text."""
    kind = style.get("listStyleType") if isinstance(style, dict) else None
    if isinstance(kind, str):
        kind = kind.strip()
        if len(kind) >= 3 and kind[0] == kind[-1] and kind[0] in "\"'":
            return clean(kind[1:-1]).strip()
    return ""


def glossary_text(value, depth=0):
    if depth > 64:
        raise ValueError("structured glossary nesting exceeds 64 levels")
    if isinstance(value, str):
        if re.search(r"</?[A-Za-z][^>]*>", value):
            parser = PlainHTML()
            parser.feed(value)
            parser.close()
            return "".join(parser.parts)
        return value
    if isinstance(value, list):
        text, after_element = "", False
        for item in value:
            part = glossary_text(item, depth + 1)
            if after_element and isinstance(item, dict) and joins_words(text, part):
                text += " "
            text += part
            after_element = isinstance(item, dict)
        return text
    if isinstance(value, dict):
        kind, tag = value.get("type"), value.get("tag")
        if kind == "image" or tag == "img":
            return str(value.get("alt") or value.get("title") or "[image omitted]")
        if kind == "text":
            return glossary_text(value.get("text", ""), depth + 1)
        if tag in ("script", "style", "rt", "rp"):
            return ""
        if tag == "br":
            return "\n"
        result = glossary_text(value.get("content", ""), depth + 1)
        if tag == "li":
            marker = list_marker(value.get("style"))
            if marker and result.strip():
                result = marker + " " + result.lstrip()
        if tag in ("div", "p", "li", "ul", "ol", "tr", "table"):
            return "\n" + result + "\n"
        if tag in ("td", "th"):
            return result + " "
        return result
    raise ValueError("glossary must contain strings or structured content")


def stream_array(stream, chunk_size=65536):
    """Incrementally parse a JSON array, including strict separator/EOF checks."""
    decoder = json.JSONDecoder()
    buf, pos, eof = "", 0, False

    def more():
        nonlocal buf, pos, eof
        buf = buf[pos:]
        pos = 0
        part = stream.read(chunk_size)
        eof = not part
        buf += part
        if len(buf) > MAX_ROW_CHARS:
            raise ValueError("one JSON row exceeds the 8 MiB conversion limit")

    def skip():
        nonlocal pos
        while True:
            while pos < len(buf) and buf[pos].isspace():
                pos += 1
            if pos < len(buf) or eof:
                return
            more()

    skip()
    if pos >= len(buf) or buf[pos] != "[":
        raise ValueError("term bank must be a JSON array")
    pos += 1
    skip()
    if pos < len(buf) and buf[pos] == "]":
        pos += 1
    else:
        while True:
            skip()
            while True:
                try:
                    value, end = decoder.raw_decode(buf, pos)
                    break
                except json.JSONDecodeError as exc:
                    if eof:
                        raise ValueError("invalid or truncated term bank JSON") from exc
                    more()
            pos = end
            yield value
            skip()
            if pos >= len(buf):
                raise ValueError("unterminated term bank array")
            char = buf[pos]
            pos += 1
            if char == "]":
                break
            if char != ",":
                raise ValueError("expected comma between term bank rows")
    skip()
    if pos < len(buf):
        raise ValueError("unexpected content after term bank array")


def rule_flags(value):
    flags = 0
    for token in value.split():
        if token.startswith("v5"):
            token = "v5"
        elif token.startswith("vs"):
            token = "vs"
        flags |= RULES.get(token, 0)
    return flags


def read_index(archive):
    info = archive.getinfo("index.json")
    if info.file_size > MAX_INDEX_BYTES:
        raise ValueError("index.json exceeds 1 MiB")
    data = json.loads(archive.read(info).decode("utf-8-sig"))
    if not isinstance(data, dict) or data.get("format") != 3:
        raise ValueError("expected a Yomitan format 3 dictionary")
    title = data.get("title")
    if not isinstance(title, str) or not title.strip():
        raise ValueError("index.json has no dictionary title")
    return utf8_clip(clean(title.strip()).replace("\n", " "), 128)


def frequency_value(data):
    """(rank, kana form) of one Yomitan frequency value: a number, a numeric
    string or {"value", "displayValue"}. Jiten marks a kana form's rank with
    a trailing ㋕ in displayValue. None when there is no whole rank >= 1."""
    display = None
    if isinstance(data, dict):
        display = data.get("displayValue")
        data = data.get("value")
    if isinstance(data, str) and re.fullmatch(r"[0-9]{1,9}", data.strip()):
        data = int(data.strip())
    if isinstance(data, bool) or not isinstance(data, (int, float)) or not math.isfinite(data) or data != int(data):
        return None
    rank = int(data)
    if not 1 <= rank <= FREQ_RANK_MAX:
        return None
    return rank, isinstance(display, str) and display.endswith("㋕")


def load_frequency(path, db):
    """Streams a rank-based Yomitan frequency ZIP into the freq table; returns its stats."""
    try:
        with zipfile.ZipFile(path) as archive:
            names = archive.namelist()
            if len(names) != len(set(names)):
                raise ValueError("ZIP contains duplicate filenames")
            title = read_index(archive)
            index = json.loads(archive.read("index.json").decode("utf-8-sig"))
            if index.get("frequencyMode") != "rank-based":
                raise ValueError("the frequency dictionary must declare \"frequencyMode\": \"rank-based\"")
            banks = sorted((n for n in names if re.fullmatch(r"term_meta_bank_[0-9]+\.json", n)),
                           key=lambda n: int(n[15:-5]))
            if not banks:
                raise ValueError("no term_meta_bank_*.json files")
            stats = {"title": utf8_clip(title, FREQ_TITLE.size - 1), "revision": index.get("revision"),
                     "rows": 0, "freq_rows": 0, "reading_rows": 0, "spelling_rows": 0, "kana_form_rows": 0,
                     "other_mode_rows": 0, "unusable_rows": 0}
            order = 0
            for name in banks:
                with archive.open(name) as raw, io.TextIOWrapper(raw, encoding="utf-8-sig") as bank:
                    for rownum, row in enumerate(stream_array(bank), 1):
                        stats["rows"] += 1
                        if not isinstance(row, list) or len(row) != 3 or not isinstance(row[0], str) \
                                or not isinstance(row[1], str):
                            raise ValueError(f"{name}, row {rownum}: expected [term, mode, data]")
                        term, mode, data = row
                        if mode != "freq":
                            stats["other_mode_rows"] += 1
                            continue
                        reading = None
                        if isinstance(data, dict) and "reading" in data:
                            reading = data.get("reading")
                            data = data.get("frequency")
                            if not isinstance(reading, str) or not reading:
                                stats["unusable_rows"] += 1
                                continue
                        value = frequency_value(data)
                        if (value is None or not term or len(term) > MAX_FREQ_TERM or clean(term) != term
                                or (reading is not None and (len(reading) > MAX_FREQ_TERM or clean(reading) != reading))):
                            stats["unusable_rows"] += 1
                            continue
                        term.encode("utf-8")  # Reject lone surrogates.
                        db.execute("INSERT INTO freq VALUES (?,?,?,?,?,?,?)",
                                   (order, term, reading, normalize(term),
                                    None if reading is None else normalize(reading), value[0], int(value[1])))
                        order += 1
                        stats["freq_rows"] += 1
                        stats["reading_rows" if reading is not None else "spelling_rows"] += 1
                        stats["kana_form_rows"] += int(value[1])
            if not stats["freq_rows"]:
                raise ValueError("no usable frequency rows")
            db.execute("CREATE INDEX freq_term ON freq(nterm, ord)")
            db.commit()
            return stats
    except (ValueError, KeyError, UnicodeError, zipfile.BadZipFile, RecursionError, OverflowError) as exc:
        raise ValueError(f"{path}: {exc}") from exc


def frequency_word(db, spelling, reading):
    """The frequency word of one entry.

    Yomitan (yomidevs/yomitan@67db60d, ext/js/language/translator.js) attaches
    a frequency row to a headword when the row's term is the headword's
    spelling and, if the row has a reading, that reading is the headword's
    (_addTermMeta, 1270-1276); a rank-based sort takes the lowest value
    (_updateSortFrequencies, 2340-2358). Here the reading-specific rows decide
    first and the spelling-only ones only when none matches; each exactly,
    then after the overlay's kana normalisation (katakana as hiragana,
    full-width ASCII as ASCII; Yomitan compares exactly). An entry with no
    reading (a variant spelling, or a character article) matches no reading,
    so last it takes its spelling's reading-specific rows. Within the
    deciding level, a kanji spelling takes its own form's rows (Jiten's
    kanji-form ranks, without ㋕): Jiten's kana-form rank of a reading (㋕)
    counts the word written in kana, so for 先 read さっき it is 221 while 先
    itself is read so at 213,803 (and さき at 416). Only without an own row
    does it borrow the kana-form rank, marked FREQ_BORROWED. The lowest rank
    of the chosen rows wins (Yomitan's sort). No row: 0, the explicit
    unranked value."""
    has_reading = bool(reading)
    reading = reading or spelling
    rows = db.execute("SELECT term, reading, nreading, rank, kana, ord FROM freq WHERE nterm=? ORDER BY ord",
                      (normalize(spelling),)).fetchall()
    if not rows:
        return 0
    nreading = normalize(reading)
    levels = ([r for r in rows if r[1] is not None and r[0] == spelling and r[1] == reading],
              [r for r in rows if r[1] is not None and r[2] == nreading],
              [r for r in rows if r[1] is None and r[0] == spelling],
              [r for r in rows if r[1] is None],
              [r for r in rows if r[1] is not None and r[0] == spelling and not has_reading],
              [r for r in rows if r[1] is not None and not has_reading])
    for level, found in enumerate(levels):
        if found:
            # The entry's own written form first: a kanji spelling's rows
            # without ㋕ (Jiten's kanji-form ranks); its kana-form rows (the
            # word's rank written in kana) only when it has none, marked
            # borrowed. A kana spelling's rows are all its own.
            kana = is_kana(spelling)
            own = [r for r in found if kana or not r[4]]
            best = min(own or found, key=lambda r: (r[3], r[4], r[5]))
            word = best[3] | (FREQ_READING, FREQ_SPELLING, FREQ_READING | FREQ_SPELLING)[level // 2]
            return word | (FREQ_NORMALIZED if level % 2 else 0) | (0 if own else FREQ_BORROWED)
    return 0


def is_kana(text):
    """Hiragana, katakana (with ー), half-width katakana only."""
    return all(0x3040 <= ord(c) <= 0x30FF or 0x31F0 <= ord(c) <= 0x31FF or 0xFF66 <= ord(c) <= 0xFF9F
               for c in text)


def percent(part, whole):
    return round(100.0 * part / whole, 2) if whole else 0.0


def frequency_coverage(db, freq, entries):
    """How much of the dictionary got a rank, and how much of the list has a
    ranked entry where the overlay looks its written form up: one spelled so
    (normalised, as lookups are), or for a kana row also one read so."""
    freq["entries_unranked"] = entries - freq["entries_ranked"]
    freq["entries_ranked_percent"] = percent(freq["entries_ranked"], entries)
    for item in freq["by_dictionary"]:
        item["ranked_percent"] = percent(item["ranked"], item["entries"])
    db.executescript("CREATE INDEX ranked_spelling ON ranked_keys(nspelling);"
                     "CREATE INDEX ranked_reading ON ranked_keys(nreading);")
    limits = (1000, 10000, 100000)
    tops = {limit: [0, 0] for limit in limits}
    covered = 0
    for rank, hit in db.execute("""
            SELECT rank, EXISTS(SELECT 1 FROM ranked_keys k WHERE k.nspelling = f.nterm)
                OR (f.nreading IS NULL AND EXISTS(SELECT 1 FROM ranked_keys k WHERE k.nreading = f.nterm))
            FROM freq f ORDER BY ord"""):
        covered += hit
        for limit in limits:
            if rank <= limit:
                tops[limit][0] += hit
                tops[limit][1] += 1
    freq["list_rows_with_ranked_entry"] = covered
    freq["list_rows_with_ranked_entry_percent"] = percent(covered, freq["freq_rows"])
    freq["list_rows_with_ranked_entry_percent_by_rank"] = {
        f"rank<={limit}": percent(hit, n) for limit, (hit, n) in tops.items()}
    freq["unranked_value"] = 0
    return freq


def key_hash(key):
    """FNV-1a-64 plus avalanche; identical to core/local_dict.c."""
    mask = (1 << 64) - 1
    h = 14695981039346656037
    for byte in key:
        h = ((h ^ byte) * 1099511628211) & mask
    h = ((h ^ (h >> 33)) * 0xff51afd7ed558ccd) & mask
    h = ((h ^ (h >> 33)) * 0xc4ceb9fe1a85ec53) & mask
    return h ^ (h >> 33)


def freq_key(word):
    """core/local_dict.c freq_key: a frequency word's rank as 13 bits, higher for a more
    frequent entry, 0 unranked; an own rank (no FREQ_BORROWED) above any borrowed one."""
    rank = word & FREQ_RANK_MAX
    if not rank:
        return 0
    e = rank.bit_length() - 1
    m = (rank >> (e - 7)) & 127 if e >= 7 else (rank << (7 - e)) & 127
    return (0 if word & FREQ_BORROWED else 4096) + 4095 - (e * 128 + m)


def posting_hint(key, payload_head, entry_len):
    """A version 3 posting's hint: what rank_posting reads from the payload's head, which
    is payload_head (at least the first min(entry_len, 4 + HEAD_BYTES) payload bytes).
    Spelling and reading count only inside the reader's window, as there."""
    word = struct.unpack_from("<I", payload_head)[0]
    hint = freq_key(word) << HINT_FREQ_SHIFT
    n = min(entry_len, 4 + HEAD_BYTES) - 4
    head = payload_head[4:4 + n]
    end = head.find(b"\0")
    if end < 0:
        return hint  # the spelling fills the window: no flags, as the reader decides
    spelling = head[:end]
    if spelling == key:
        hint |= HINT_SPELLING
    if normalize(spelling.decode("utf-8")) == key:
        hint |= HINT_NORMALIZED
    if end + 1 < n and head[end + 1] != 0:
        hint |= HINT_READING
    return hint


def add_index_hints(path, output=None):
    """Writes the posting hints into an existing version 3 file without them (deterministic;
    entries, index and payloads unchanged, readers that predate them ignore the u16)."""
    path, output = Path(path), Path(output or path)
    with path.open("rb") as stream:
        page0 = stream.read(PAGE_SIZE)
        if page0[:8] != MAGIC_V3:
            raise ValueError(f"{path}: posting hints need a version 3 dictionary")
        if any(page0[HINTS_DESCRIPTOR_OFFSET:HINTS_DESCRIPTOR_OFFSET + HINTS_DESCRIPTOR.size]):
            raise ValueError(f"{path}: page 0's hint descriptor bytes are not empty")
        pages = HEADER.unpack_from(page0)[9]
        with tempfile.NamedTemporaryFile(prefix=output.name + ".", suffix=".tmp", dir=output.parent,
                                         delete=False) as final:
            try:
                stream.seek(0)
                shutil.copyfileobj(stream, final, length=1024 * 1024)
                postings = 0
                for page in range(pages):
                    stream.seek(PAGE_SIZE * (1 + page))
                    block = stream.read(PAGE_SIZE)
                    count = PAGE_HEADER.unpack_from(block)[1]
                    for slot in range(count):
                        _, group, n, length, _ = HASH_RECORD.unpack_from(block, PAGE_HEADER.size + slot * HASH_RECORD.size)
                        stream.seek(group)
                        key = stream.read(length)
                        raw = stream.read(n * POSTING.size)
                        for j in range(n):
                            entry_off, entry_len, _, old = POSTING.unpack_from(raw, j * POSTING.size)
                            if old:
                                raise ValueError(f"{path}: a posting already has a hint")
                            stream.seek(entry_off)
                            hint = posting_hint(key, stream.read(min(entry_len, 4 + HEAD_BYTES)), entry_len)
                            final.seek(group + length + j * POSTING.size + 14)
                            final.write(struct.pack("<H", hint))
                            postings += 1
                final.seek(HINTS_DESCRIPTOR_OFFSET)
                final.write(HINTS_DESCRIPTOR.pack(HINTS_MAGIC, 1))
                final.flush()
                os.fsync(final.fileno())
                os.chmod(final.name, path.stat().st_mode & 0o777)
            except BaseException:
                Path(final.name).unlink(missing_ok=True)
                raise
    os.replace(final.name, output)
    return {"hinted_postings": postings}


def filter_prefix(key):
    """A key's filter block is chosen by its first two code points, 16 bits each (0xFFFF
    beyond the BMP), the second 0 for a one-character key: nearly every candidate searched
    at one cursor keeps the cursor's two."""
    text = key.decode("utf-8")  # converted keys are valid UTF-8; anything else is refused
    return min(ord(text[0]), 0xFFFF) << 16 | (min(ord(text[1]), 0xFFFF) if len(text) > 1 else 0)


def filter_bits(h, hashes=FILTER_HASHES):
    """The Bloom bits of a key hash within its 32,768-bit block (double hashing)."""
    h1, h2 = h & 0xFFFFFFFF, (h >> 32) | 1
    return [(h1 + i * h2) & (FILTER_BLOCK_BITS - 1) for i in range(hashes)]


def build_key_filter(keys):
    """keys: (key bytes, key_hash) of every index key. Returns (directory, blocks, stats).

    Keys are grouped by filter_prefix and the groups packed in prefix order into
    4 KiB blocks of about FILTER_BITS_PER_KEY bits a key. A first character's whole
    range goes into one block whenever it fits one, so a lookup's candidates stay in
    one block; a group larger than a block gets one to itself. The directory holds
    each block's first prefix (block 0 starts at 0)."""
    groups, n_keys = {}, 0
    for key, h in keys:
        groups.setdefault(filter_prefix(key), []).append(h)
        n_keys += 1
    order = sorted(groups)
    by_first = {}
    for prefix in order:
        by_first[prefix >> 16] = by_first.get(prefix >> 16, 0) + len(groups[prefix])
    bits_per_key = FILTER_BITS_PER_KEY
    while True:
        capacity = FILTER_BLOCK_BITS // bits_per_key
        starts, load, previous = [], 0, None
        for prefix in order:
            size, first = len(groups[prefix]), prefix >> 16
            whole = first != previous and by_first[first] <= capacity
            if not starts or (load and (load + size > capacity or (whole and load + by_first[first] > capacity))):
                starts.append(prefix)
                load = 0
            load += size
            previous = first
        if len(starts) <= FILTER_MAX_BLOCKS or bits_per_key == 1:
            break
        bits_per_key -= 1  # very large dictionaries: fewer bits a key, more false positives
    if len(starts) > FILTER_MAX_BLOCKS:
        raise ValueError("too many keys for the key filter")
    starts[0] = 0
    blocks = [bytearray(PAGE_SIZE) for _ in starts]
    loads = [0] * len(starts)
    block = 0
    for prefix in order:
        while block + 1 < len(starts) and starts[block + 1] <= prefix:
            block += 1
        loads[block] += len(groups[prefix])
        bits = blocks[block]
        for h in groups[prefix]:
            for bit in filter_bits(h):
                bits[bit >> 3] |= 1 << (bit & 7)
    return starts, blocks, {"filter_blocks": len(starts), "filter_bytes": len(starts) * PAGE_SIZE,
                            "filter_keys": n_keys, "filter_max_block_keys": max(loads),
                            "filter_bits_per_key": bits_per_key}


def write_key_filter(final, keys, size):
    """Appends the key filter at the next page boundary after `size` bytes of a version
    2+ file and announces it in page 0. Returns the new file size and statistics."""
    directory, blocks, stats = build_key_filter(keys)
    offset = (size + PAGE_SIZE - 1) // PAGE_SIZE * PAGE_SIZE
    if offset + len(blocks) * PAGE_SIZE > MAX_FILE_SIZE:
        raise ValueError(TOO_LARGE)
    final.seek(size)
    final.write(bytes(offset - size))
    for bits in blocks:
        final.write(bits)
    end = final.tell()
    final.seek(FILTER_DESCRIPTOR_OFFSET)
    final.write(FILTER_DESCRIPTOR.pack(FILTER_MAGIC, 1, len(blocks), FILTER_HASHES, offset, stats["filter_keys"], 0))
    final.seek(FILTER_DIRECTORY_OFFSET)
    final.write(struct.pack(f"<{len(directory)}I", *directory))
    final.seek(40)  # the header's exact file size
    final.write(struct.pack("<Q", end))
    return end, stats


def index_keys(stream):
    """(key bytes, hash) of every index key of an opened version 2+ file, page by page."""
    stream.seek(0)
    header = HEADER.unpack(stream.read(HEADER.size))
    if header[0][:6] != b"VJDICT" or header[1] < 2:
        raise ValueError("a key filter needs a version 2 or later dictionary")
    for page in range(header[9]):
        stream.seek(PAGE_SIZE * (1 + page))
        block = stream.read(PAGE_SIZE)
        magic, count, _ = PAGE_HEADER.unpack_from(block)
        if magic != b"VJHP" or count > HASH_PAGE_RECORDS:
            raise ValueError("invalid hash index page")
        for slot in range(count):
            h, group, _, length, _ = HASH_RECORD.unpack_from(block, PAGE_HEADER.size + slot * HASH_RECORD.size)
            stream.seek(group)
            key = stream.read(length)
            if len(key) != length or key_hash(key) != h:
                raise ValueError("an index key does not match its hash")
            yield key, h


def add_key_filter(path, output=None):
    """Adds a key filter to an existing version 2+ dictionary without changing anything a
    reader without it uses (deterministic: the same input gives the same bytes)."""
    path, output = Path(path), Path(output or path)
    with path.open("rb") as stream:
        page0 = stream.read(PAGE_SIZE)
        if len(page0) < PAGE_SIZE or page0[:6] != b"VJDICT" or struct.unpack_from("<I", page0, 8)[0] < 2:
            raise ValueError(f"{path}: not a version 2 or later dictionary")
        if page0[FILTER_DESCRIPTOR_OFFSET:FILTER_DESCRIPTOR_OFFSET + 4] == FILTER_MAGIC:
            raise ValueError(f"{path}: already has a key filter")
        # Not the filter's: the posting hints' descriptor (160..167) and a dictionary title (192..255).
        hints_end = HINTS_DESCRIPTOR_OFFSET + HINTS_DESCRIPTOR.size
        if (any(page0[FILTER_DESCRIPTOR_OFFSET:HINTS_DESCRIPTOR_OFFSET]) or any(page0[hints_end:FILTER_TITLE_GAP[0]])
                or any(page0[FILTER_DIRECTORY_OFFSET:])):
            raise ValueError(f"{path}: page 0's bytes {FILTER_DESCRIPTOR_OFFSET}..{HINTS_DESCRIPTOR_OFFSET - 1}, "
                             f"{hints_end}..{FILTER_TITLE_GAP[0] - 1} or {FILTER_DIRECTORY_OFFSET}..{PAGE_SIZE - 1} "
                             f"are not empty")
        size = struct.unpack_from("<Q", page0, 40)[0]
        if size != path.stat().st_size:
            raise ValueError(f"{path}: size does not match its header")
        buckets = struct.unpack_from("<Q", page0, 48)[0]
        if buckets < FILTER_MIN_BUCKETS:
            raise ValueError(f"{path}: {buckets} hash bucket(s); below {FILTER_MIN_BUCKETS} a key filter "
                             "costs a lookup more reads than it saves")
        with tempfile.NamedTemporaryFile(prefix=output.name + ".", suffix=".tmp", dir=output.parent,
                                         delete=False) as final:
            try:
                stream.seek(0)
                shutil.copyfileobj(stream, final, length=1024 * 1024)
                end, stats = write_key_filter(final, index_keys(stream), size)
                final.flush()
                os.fsync(final.fileno())
                os.chmod(final.name, path.stat().st_mode & 0o777)
            except BaseException:
                Path(final.name).unlink(missing_ok=True)
                raise
    os.replace(final.name, output)
    return {"bytes": end, **stats}


def write_v2(db, data, final, count, frequency_title=None, key_filter=True):
    """Version 2 (or, with frequency_title, version 3: the same layout)."""
    # Sorting and all per-key metadata live on the computer's disk, not the Vita.
    db.execute("CREATE INDEX lookup_order ON lookups(key, priority, score, seq)")
    unique = db.execute("SELECT COUNT(DISTINCT key) FROM lookups").fetchone()[0]
    buckets = 1
    # About 82% target occupancy; rare overflow pages keep padding modest.
    while buckets * 140 < unique:
        buckets *= 2
    counts = [0] * buckets
    db.execute("CREATE TABLE hash_keys(key BLOB PRIMARY KEY, hash BLOB, bucket INTEGER, n INTEGER, off INTEGER, rules INTEGER)")
    for (key,) in db.execute("SELECT DISTINCT key FROM lookups ORDER BY key"):
        h = key_hash(key)
        bucket = h & (buckets - 1)
        counts[bucket] += 1
        # Big endian blob order is unsigned integer order in SQLite.
        db.execute("INSERT INTO hash_keys VALUES (?,?,?,0,0,0)", (key, h.to_bytes(8, "big"), bucket))
    page_counts = [max(1, (n + HASH_PAGE_RECORDS - 1) // HASH_PAGE_RECORDS) for n in counts]
    if max(page_counts) > MAX_HASH_CHAIN:
        raise ValueError("hash bucket chain exceeds the supported limit")
    pages = sum(page_counts)
    data_off = PAGE_SIZE * (1 + pages)
    final.seek(data_off)
    data.seek(0)
    shutil.copyfileobj(data, final, length=1024 * 1024)
    current, group_off, n, rules_union = None, 0, 0, 0
    for key, entry_off, rules, length in db.execute(
            "SELECT key, entry_off, rules, size FROM lookups ORDER BY key, priority, score, seq"):
        if key != current:
            if current is not None:
                db.execute("UPDATE hash_keys SET n=?, off=?, rules=? WHERE key=?", (n, group_off, rules_union, current))
            current, group_off, n, rules_union = key, final.tell(), 0, 0
            final.write(key)
        hint = 0
        if frequency_title is not None:  # version 3: rank the posting without its head
            data.seek(entry_off)
            hint = posting_hint(key, data.read(min(length, 4 + HEAD_BYTES)), length)
        final.write(POSTING.pack(data_off + entry_off, length, rules, hint))
        n += 1
        rules_union |= rules
    db.execute("UPDATE hash_keys SET n=?, off=?, rules=? WHERE key=?", (n, group_off, rules_union, current))
    size = final.tell()
    if size > MAX_FILE_SIZE:
        raise ValueError(TOO_LARGE)
    final.seek(0)
    version = 2 if frequency_title is None else 3
    final.write(HEADER.pack(MAGIC_V2 if version == 2 else MAGIC_V3, version, HEADER.size, count, HASH_RECORD.size,
                           PAGE_SIZE, data_off, size, buckets, pages))
    if frequency_title is not None:
        final.write(FREQ_TITLE.pack(frequency_title.encode("utf-8")))
        final.seek(HINTS_DESCRIPTOR_OFFSET)
        final.write(HINTS_DESCRIPTOR.pack(HINTS_MAGIC, 1))
    records = iter(db.execute("SELECT hash, off, n, key, rules FROM hash_keys ORDER BY bucket, hash, key"))
    overflow = buckets
    for bucket, number in enumerate(page_counts):
        left = counts[bucket]
        page_number = bucket
        for page_in_bucket in range(number):
            take = min(left, HASH_PAGE_RECORDS)
            following = 0 if page_in_bucket + 1 == number else PAGE_SIZE * (1 + overflow)
            block = bytearray(PAGE_SIZE)
            PAGE_HEADER.pack_into(block, 0, b"VJHP", take, following)
            for slot in range(take):
                h, group_off, n, key, flags = next(records)
                HASH_RECORD.pack_into(block, PAGE_HEADER.size + slot * HASH_RECORD.size,
                                      int.from_bytes(h, "big"), group_off, n, len(key), flags)
            final.seek(PAGE_SIZE * (1 + page_number))
            final.write(block)
            left -= take
            if following:
                page_number = overflow
                overflow += 1
    result = {"bytes": size, "unique_keys": unique, "hash_buckets": buckets, "index_pages": pages}
    if key_filter and buckets >= FILTER_MIN_BUCKETS:
        keys = ((bytes(key), int.from_bytes(h, "big")) for key, h in db.execute("SELECT key, hash FROM hash_keys"))
        size, filter_stats = write_key_filter(final, keys, size)
        result.update(bytes=size, **filter_stats)
    return result


def convert(inputs, output, max_definition_bytes=4096, temp_dir=None, format_version=None,
            deduplicate=False, frequency=None, key_filter=True):
    """frequency: a rank-based Yomitan frequency ZIP; it makes the output format 3."""
    if format_version is None:
        format_version = 2 if frequency is None else 3
    if format_version not in (1, 2, 3):
        raise ValueError("format-version must be 1, 2 or 3")
    if (format_version == 3) != (frequency is not None):
        raise ValueError("format 3 needs a frequency dictionary, and a frequency dictionary needs format 3")
    output = Path(output)
    if output.suffix != ".vjdict":
        raise ValueError("output filename must end in .vjdict")
    if not 256 <= max_definition_bytes <= 7000:
        raise ValueError("max-definition-bytes must be between 256 and 7000")
    if any(Path(p).resolve() == output.resolve() for p in [*inputs, *([frequency] if frequency else [])]):
        raise ValueError("output must not replace an input archive")
    output.parent.mkdir(parents=True, exist_ok=True)
    stats = {"format_version": format_version, "entries": 0, "index_records": 0, "truncated_definitions": 0,
             "reading_only_entries": 0,
             "skipped_empty_headwords": 0, "skipped_long_headwords": 0, "dictionaries": []}
    if deduplicate:
        stats.update(deduplication="first dictionary per spelling and normalized reading",
                     deduplicated_rows=0, source_entries=[])
    freq = None
    # The final temporary file is beside the destination, for atomic replacement.
    final_name = None
    try:
        with tempfile.TemporaryDirectory(prefix="vjo-dict-", dir=temp_dir) as tmp:
            db = sqlite3.connect(str(Path(tmp) / "index.sqlite"))
            try:
                db.executescript("""
                    PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;
                    PRAGMA temp_store=FILE; PRAGMA cache_size=-4096;
                    CREATE TABLE lookups (key BLOB, priority INTEGER, score REAL,
                        seq INTEGER, key_off INTEGER, entry_off INTEGER, rules INTEGER, size INTEGER);
                    CREATE TABLE selected_pairs (spelling TEXT, reading BLOB, priority INTEGER,
                        PRIMARY KEY (spelling, reading));
                    CREATE TABLE freq (ord INTEGER PRIMARY KEY, term TEXT, reading TEXT, nterm BLOB,
                        nreading BLOB, rank INTEGER, kana INTEGER);
                    CREATE TABLE ranked_keys (nspelling BLOB, nreading BLOB);
                """)
                if frequency is not None:
                    freq = load_frequency(frequency, db)
                    matches = [m + n for m in MATCHES.values() for n in ("", "_normalized")]
                    freq.update(entries_ranked=0, by_match=dict.fromkeys([*matches, "unranked"], 0),
                                borrowed_ranks=0, by_dictionary=[])
                with (Path(tmp) / "payload.bin").open("w+b") as data:
                    for priority, source in enumerate(inputs):
                        before = stats["entries"]
                        ranked_before = freq["entries_ranked"] if freq else 0
                        try:
                            with zipfile.ZipFile(source) as archive:
                                names = archive.namelist()
                                if len(names) != len(set(names)):
                                    raise ValueError("ZIP contains duplicate filenames")
                                title = read_index(archive)
                                stats["dictionaries"].append(title)
                                banks = sorted((n for n in names if re.fullmatch(r"term_bank_[0-9]+\.json", n)),
                                               key=lambda n: int(n[10:-5]))
                                if not banks:
                                    raise ValueError("no term_bank_*.json files (frequency/pitch/kanji-only dictionaries are "
                                                     "unsupported as inputs; a rank-based frequency dictionary goes in "
                                                     "--frequency)")
                                for name in banks:
                                    with archive.open(name) as raw, io.TextIOWrapper(raw, encoding="utf-8-sig") as bank:
                                        for rownum, row in enumerate(stream_array(bank), 1):
                                            context = f"{name}, row {rownum}"
                                            if not isinstance(row, list) or len(row) != 8:
                                                raise ValueError(f"{context}: expected an 8-field Yomitan term row")
                                            spelling, reading, _, rules, score, gloss, _, _ = row
                                            if (not isinstance(spelling, str) or not isinstance(reading, str)
                                                    or not isinstance(rules, str) or not isinstance(gloss, list)
                                                    or not isinstance(score, (int, float)) or isinstance(score, bool)
                                                    or not math.isfinite(score)):
                                                raise ValueError(f"{context}: invalid spelling, reading, rules, score, or glossary")
                                            if clean(spelling) != spelling or clean(reading) != reading:
                                                raise ValueError(f"{context}: headwords contain control characters")
                                            if not (spelling or reading):
                                                stats["skipped_empty_headwords"] += 1
                                                continue
                                            if not spelling:
                                                spelling = reading
                                                stats["reading_only_entries"] += 1
                                            spelling.encode("utf-8")  # Reject lone surrogates before writing.
                                            reading.encode("utf-8")
                                            keys = list(dict.fromkeys(normalize(x) for x in (spelling, reading) if x))
                                            if (max(len(spelling), len(reading)) > MAX_SCAN_CHARS
                                                    or max(len(spelling.encode()), len(reading.encode())) > MAX_KEY
                                                    or any(len(k) > MAX_KEY for k in keys)):
                                                stats["skipped_long_headwords"] += 1
                                                continue
                                            text = "\n".join(glossary_text(g).strip() for g in gloss)
                                            text = re.sub(r"\n[ \t]*\n+", "\n", clean(text)).strip()
                                            if deduplicate:
                                                # Preserve distinct spellings and all senses from the
                                                # winning source. Empty kana readings match explicit ones.
                                                pair = (spelling, normalize(reading or spelling))
                                                db.execute("INSERT OR IGNORE INTO selected_pairs VALUES (?,?,?)",
                                                           (*pair, priority))
                                                winner = db.execute(
                                                    "SELECT priority FROM selected_pairs WHERE spelling=? AND reading=?",
                                                    pair).fetchone()[0]
                                                if winner != priority:
                                                    stats["deduplicated_rows"] += 1
                                                    continue
                                            definition = f"[{title}]\n{text or '(No text definition)'}"
                                            if len(definition.encode()) > max_definition_bytes:
                                                suffix = "\n[definition truncated]"
                                                definition = utf8_clip(definition, max_definition_bytes - len(suffix)) + suffix
                                                stats["truncated_definitions"] += 1
                                            payload = (spelling + "\0" + reading + "\0" + definition + "\0").encode()
                                            if freq is not None:
                                                word = frequency_word(db, spelling, reading)
                                                match = "unranked"
                                                if word:
                                                    freq["entries_ranked"] += 1
                                                    freq["borrowed_ranks"] += bool(word & FREQ_BORROWED)
                                                    match = MATCHES[word & (FREQ_READING | FREQ_SPELLING)]
                                                    match += "_normalized" if word & FREQ_NORMALIZED else ""
                                                    db.execute("INSERT INTO ranked_keys VALUES (?,?)",
                                                               (normalize(spelling), normalize(reading or spelling)))
                                                freq["by_match"][match] += 1
                                                payload = FREQ_WORD.pack(word) + payload
                                            if len(payload) > MAX_ENTRY:
                                                raise ValueError(f"{context}: converted entry is too large")
                                            entry_off = data.tell()
                                            data.write(payload)
                                            flags = rule_flags(rules)
                                            for key in keys:
                                                key_off = data.tell()
                                                if format_version == 1:
                                                    data.write(key)
                                                db.execute("INSERT INTO lookups VALUES (?,?,?,?,?,?,?,?)",
                                                           (key, priority, -score, stats["entries"], key_off, entry_off, flags, len(payload)))
                                                stats["index_records"] += 1
                                            stats["entries"] += 1
                                    db.commit()
                                if deduplicate:
                                    stats["source_entries"].append(stats["entries"] - before)
                                if freq is not None:
                                    freq["by_dictionary"].append({"dictionary": title,
                                                                  "entries": stats["entries"] - before,
                                                                  "ranked": freq["entries_ranked"] - ranked_before})
                        except (ValueError, KeyError, UnicodeError, zipfile.BadZipFile, RecursionError, OverflowError) as exc:
                            raise ValueError(f"{source}: {exc}") from exc
                    count = stats["index_records"]
                    if not count:
                        raise ValueError("no installable term entries found")
                    if count > 0xFFFFFFFF:
                        raise ValueError("dictionary has too many index records")
                    data_off = HEADER.size + count * RECORD.size
                    size = data_off + data.tell()
                    if size > MAX_FILE_SIZE:
                        raise ValueError(TOO_LARGE)
                    with tempfile.NamedTemporaryFile(prefix=output.name + ".", suffix=".tmp", dir=output.parent,
                                                     delete=False) as final:
                        final_name = Path(final.name)
                        if format_version >= 2:
                            stats.update(write_v2(db, data, final, count, freq["title"] if freq else None,
                                                  key_filter))
                            size = stats["bytes"]
                        else:
                            final.write(HEADER.pack(MAGIC, 1, HEADER.size, count, RECORD.size,
                                                    HEADER.size, data_off, size, 0, 0))
                            for key, key_off, entry_off, flags, length in db.execute(
                                    "SELECT key, key_off, entry_off, rules, size FROM lookups ORDER BY key, priority, score, seq"):
                                final.write(RECORD.pack(key_off + data_off, entry_off + data_off, len(key), flags, length))
                            data.seek(0)
                            shutil.copyfileobj(data, final, length=1024 * 1024)
                        final.flush()
                        os.fsync(final.fileno())
                    os.replace(final_name, output)
                    final_name = None
                    stats["bytes"] = size
                    if freq is not None:
                        stats["frequency"] = frequency_coverage(db, freq, stats["entries"])
            finally:
                db.close()
    finally:
        if final_name is not None:
            final_name.unlink(missing_ok=True)
    return stats


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="*", type=Path, help="Yomitan term ZIP(s), in priority order")
    parser.add_argument("-o", "--output", type=Path, help="output .vjdict file")
    parser.add_argument("--add-key-filter", type=Path, metavar="VJDICT",
                        help="add the key filter to an existing version 2 or 3 file (in place, or to -o)")
    parser.add_argument("--no-key-filter", action="store_true",
                        help="write no key filter (lookups then read more of the index)")
    parser.add_argument("--max-definition-bytes", type=int, default=4096)
    parser.add_argument("--format-version", type=int, choices=(1, 2, 3),
                        help="2: page hash index (default); 3: version 2 with a frequency rank in every entry "
                             "(the default with --frequency); 1: legacy binary-search format")
    parser.add_argument("--frequency", type=Path,
                        help="rank-based Yomitan frequency dictionary ZIP: every entry gets a rank (0 = unranked)")
    parser.add_argument("--temp-dir", type=Path, help="scratch disk for sorting large dictionaries")
    parser.add_argument("--deduplicate", action="store_true",
                        help="keep each spelling/reading pair only from its first input dictionary; preserve its senses")
    args = parser.parse_args()
    if args.add_key_filter:
        if args.inputs:
            parser.error("--add-key-filter takes no input ZIPs")
        try:
            stats = add_key_filter(args.add_key_filter, args.output)
        except (ValueError, OSError, UnicodeError) as exc:
            parser.exit(1, f"Adding the key filter failed: {exc}\n")
        print(json.dumps(stats, indent=2))
        return
    if not args.inputs or not args.output:
        parser.error("give the input ZIPs and -o OUTPUT")
    try:
        stats = convert(args.inputs, args.output, args.max_definition_bytes, args.temp_dir,
                        args.format_version, args.deduplicate, args.frequency, not args.no_key_filter)
    except (ValueError, OSError, sqlite3.Error, RuntimeError) as exc:
        parser.exit(1, f"Conversion failed: {exc}\n")
    print(json.dumps(stats, ensure_ascii=False, indent=2))
    print(f"Saved {args.output}. Copy to ux0:data/VitaJPOverlay/dictionaries/ and enable dictionary = local.")


if __name__ == "__main__":
    main()
