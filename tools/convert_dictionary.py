#!/usr/bin/env python3
"""Convert Yomitan v3 term ZIPs to a bounded-memory Vita dictionary.

Python 3.9+ standard library only. ZIP banks are streamed one row at a time;
SQLite sorts the index on disk, and definitions are never retained wholesale.
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
PAGE_SIZE = 4096
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
RULES = {"v1": 1, "v5": 2, "vs": 4, "vk": 8, "vz": 16, "adj-i": 32, "iru": 64}
# Key filter (docs/local-dictionaries.md): page-sized Bloom blocks after all other data,
# announced in page 0 after the header. Readers that predate it read only the header.
FILTER_MAGIC = b"VJKF"
FILTER_DESCRIPTOR = struct.Struct("<4sIIIQII")  # magic, version 1, blocks, hashes, offset, keys, 0
FILTER_DESCRIPTOR_OFFSET = 128
FILTER_DIRECTORY_OFFSET = 256  # one u32 first prefix per block
FILTER_MAX_BLOCKS = (PAGE_SIZE - FILTER_DIRECTORY_OFFSET) // 4
FILTER_BLOCK_BITS = PAGE_SIZE * 8
FILTER_HASHES = 7
FILTER_BITS_PER_KEY = 10
# Below this many hash buckets the few index pages mostly stay in the lookup's page cache and
# the filter's own block read costs more than it saves (JMdict subsets: 2 buckets 2.82 -> 3.08
# reads a lookup, 4 buckets 4.07 -> 3.16; docs/lookup-performance.md).
FILTER_MIN_BUCKETS = 4


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


def key_hash(key):
    """FNV-1a-64 plus avalanche; identical to core/local_dict.c."""
    mask = (1 << 64) - 1
    h = 14695981039346656037
    for byte in key:
        h = ((h ^ byte) * 1099511628211) & mask
    h = ((h ^ (h >> 33)) * 0xff51afd7ed558ccd) & mask
    h = ((h ^ (h >> 33)) * 0xc4ceb9fe1a85ec53) & mask
    return h ^ (h >> 33)


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
    if offset + len(blocks) * PAGE_SIZE > 0x7FFFFFFFFFFFFFFF:
        raise ValueError("dictionary exceeds the supported file size")
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
        if any(page0[FILTER_DESCRIPTOR_OFFSET:]):
            raise ValueError(f"{path}: page 0 after byte {FILTER_DESCRIPTOR_OFFSET} is not empty")
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


def write_v2(db, data, final, count, key_filter=True):
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
        final.write(POSTING.pack(data_off + entry_off, length, rules, 0))
        n += 1
        rules_union |= rules
    db.execute("UPDATE hash_keys SET n=?, off=?, rules=? WHERE key=?", (n, group_off, rules_union, current))
    size = final.tell()
    if size > 0x7FFFFFFFFFFFFFFF:
        raise ValueError("dictionary exceeds the supported file size")
    final.seek(0)
    final.write(HEADER.pack(MAGIC_V2, 2, HEADER.size, count, HASH_RECORD.size,
                           PAGE_SIZE, data_off, size, buckets, pages))
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


def convert(inputs, output, max_definition_bytes=4096, temp_dir=None, format_version=2,
            deduplicate=False, key_filter=True):
    if format_version not in (1, 2):
        raise ValueError("format-version must be 1 or 2")
    output = Path(output)
    if output.suffix != ".vjdict":
        raise ValueError("output filename must end in .vjdict")
    if not 256 <= max_definition_bytes <= 7000:
        raise ValueError("max-definition-bytes must be between 256 and 7000")
    if any(Path(p).resolve() == output.resolve() for p in inputs):
        raise ValueError("output must not replace an input archive")
    output.parent.mkdir(parents=True, exist_ok=True)
    stats = {"format_version": format_version, "entries": 0, "index_records": 0, "truncated_definitions": 0,
             "reading_only_entries": 0,
             "skipped_empty_headwords": 0, "skipped_long_headwords": 0, "dictionaries": []}
    if deduplicate:
        stats.update(deduplication="first dictionary per spelling and normalized reading",
                     deduplicated_rows=0, source_entries=[])
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
                """)
                with (Path(tmp) / "payload.bin").open("w+b") as data:
                    for priority, source in enumerate(inputs):
                        before = stats["entries"]
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
                                    raise ValueError("no term_bank_*.json files (frequency/pitch/kanji-only dictionaries are unsupported)")
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
                        except (ValueError, KeyError, UnicodeError, zipfile.BadZipFile, RecursionError, OverflowError) as exc:
                            raise ValueError(f"{source}: {exc}") from exc
                    count = stats["index_records"]
                    if not count:
                        raise ValueError("no installable term entries found")
                    if count > 0xFFFFFFFF:
                        raise ValueError("dictionary has too many index records")
                    data_off = HEADER.size + count * RECORD.size
                    size = data_off + data.tell()
                    if size > 0x7FFFFFFFFFFFFFFF:
                        raise ValueError("dictionary exceeds the supported file size")
                    with tempfile.NamedTemporaryFile(prefix=output.name + ".", suffix=".tmp", dir=output.parent,
                                                     delete=False) as final:
                        final_name = Path(final.name)
                        if format_version == 2:
                            stats.update(write_v2(db, data, final, count, key_filter))
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
                        help="add the key filter to an existing version 2 file (in place, or to -o)")
    parser.add_argument("--no-key-filter", action="store_true",
                        help="write no key filter (lookups then read more of the index)")
    parser.add_argument("--max-definition-bytes", type=int, default=4096)
    parser.add_argument("--format-version", type=int, choices=(1, 2), default=2,
                        help="2: page hash index (default); 1: legacy binary-search format")
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
                        args.format_version, args.deduplicate, not args.no_key_filter)
    except (ValueError, OSError, sqlite3.Error, RuntimeError) as exc:
        parser.exit(1, f"Conversion failed: {exc}\n")
    print(json.dumps(stats, ensure_ascii=False, indent=2))
    print(f"Saved {args.output}. Copy to ux0:data/VitaJPOverlay/dictionaries/ and enable dictionary = local.")


if __name__ == "__main__":
    main()
