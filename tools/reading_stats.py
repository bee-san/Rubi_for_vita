#!/usr/bin/env python3
"""Reading statistics from Rubi's reading history (docs/ocr-history.md): the characters read and
the characters per hour, in all, by text source (a native text hook or OCR), per game and per day.

    python3 tools/reading_stats.py history.csv [MORE.csv ...] [--time-rule adaptive|break]
                                   [--break-minutes 5] [--utc]

Copy ux0:data/VitaJPOverlay/history.csv from the Vita first (VitaShell's USB or FTP connection).
Several files (copies made at different times) are read as one history: a record in more than one
of them counts once (by its uuid and given_identifier, as GameSentenceMiner's import does).

Characters: each record's `characters` column, the count of the dialogue's kanji, kana and other
Japanese letters and its ASCII letters and digits; punctuation, brackets, symbols, spaces and line
breaks count nothing (docs/ocr-history.md, core/reading_chars.h). A file Rubi 0.8.0 wrote has no
such column, and a file it began keeps its header when a newer Rubi appends to it: those rows carry
the count as an extra, 42nd cell. Where a row has no count this tool counts its `line` itself, the
same way.

Text source: where each line came from, a native text hook or Rubi's OCR: the record's `line_source`
cell (hook or ocr, the 43rd), or, in a row an older Rubi wrote (0.9.1 and before: no such cell), its
`engine` cell, which says hook for a hook's line and vocr for OCR's (0.8.0 had no hooks: all vocr).
A record that says neither (another tracker's) is unknown. Each record's reading time goes to its
line's source.

Reading time: each record's line is credited the time to the next record of the same game, at most 2.5
times what the reader's pace would take for it (2 s to 5 minutes; the pace, per game, is learned from
the history: docs/ocr-history.md#reading-time). This is the overlay's rule (config.ini reading_time =
adaptive, its default), in the same integers, so the totals here are the Triangle view's for a history in
time order (this tool sorts by time; the overlay breaks its interval where the clock was set back). A
change of game is a break. --time-rule break (or --break-minutes N) is Rubi 0.9.7's rule instead: a gap of more
than N minutes (default 5) is a break, and the record before it gets none (how long it was read is
unknown). Characters per hour divides the characters of the records that got a time by that time; the
totals count every record.

A record cut short (power lost while it was written: no line break at the file's end, or fewer
cells than Rubi writes), even when more records were appended after it, is skipped and counted;
blank lines and repeated header lines (files joined together) are ignored. A file whose first
append was cut short in its header (the next record follows the cut, maybe after zeros) is read with
Rubi's columns, the record that append held counted as cut short.
A file that is not the writer's own bytes, such as Rubi's re-saved by a spreadsheet or another
tracker's ExStatic lines CSV (uuid, given_identifier, name, line, time), is read as plain CSV: the
characters cells it has are used, and this tool counts the other lines. A record whose time is a
number but not Unix seconds between 1970 and 3000 (another tracker's milliseconds, say), or one this
computer cannot place on a day, is skipped and counted. The output holds numbers and game names
only, never dialogue text. Exit status: 0, or 2 when a file cannot be read (or read as an ExStatic
lines CSV: a missing column, or a first line that is not CSV).
"""
from __future__ import annotations

import argparse
import bisect
import csv
import datetime
import io
import itertools
import math
import re
import sys
import unicodedata
from dataclasses import dataclass
from pathlib import Path

# The columns Rubi 0.8.0 wrote (#187), in order: every row has them in these positions. The
# `characters` column (0.9.0) and the `line_source` column come after them.
HEADER_080 = (
    "uuid,given_identifier,name,line,time,platform,title_id,engine,model,raw_line,scene,region_seq,"
    "monotonic_us,elapsed_us,job_seq,region_x,region_y,width,height,fb_width,fb_height,fb_pitch,"
    "fb_format,lines,recognized,frames,find_us,load_us,recognize_us,capture_us,captures,tears,"
    "region_sum,source,capture_memory,capture_bytes,strip_rows,workspace_bytes,resident_bytes,"
    "fallback,line_metrics").split(",")
REQUIRED = ("uuid", "given_identifier", "name", "line", "time")
# The header this writer writes (0.8.0's columns, then `characters` and `line_source`): what a file whose first
# append was cut short in its header is read with.
HEADER = HEADER_080 + ["characters", "line_source"]
HEADER_LINE = (",".join(HEADER) + "\n").encode()
# Where a line came from (docs/ocr-history.md#line-source): `line_source` says hook or ocr. A row without it
# (Rubi 0.9.1 and older) says it in its `engine`: every Rubi wrote vocr for OCR's line, and since native text
# hooks came (0.8.0-test.7) hook for a hook's. Any other row's source is unknown.
SOURCES = ("hook", "ocr", "unknown")
CELL_SOURCES = {"hook": "hook", "ocr": "ocr"}  # a line_source cell's values (the shared strings, not the cell's)
ENGINE_SOURCES = {"hook": "hook", "vocr": "ocr"}
# A time is Unix seconds from 1970 to before 3000 (Rubi's are this century), so another tracker's
# milliseconds are not taken for seconds (GameSentenceMiner's import refuses them too). datetime places
# those on a day where time_t has 64 bits; where it has 32, it cannot after 2038, and such a time is
# skipped too (add_row).
TIME_MAX = 32503680000.0  # 3000-01-01T00:00:00Z
# The most of a file's first line read as its header: Rubi's is 408 bytes, and after a cut header the next
# record's first line follows it (under 10 KB). A file with no line break is not read whole for it.
HEADER_READ = 1 << 20

# core/utf.c's japanese_letters: Unicode 17.0's Han, Hiragana and Katakana letters (and the
# combining dakuten), first and last code point. tools/tests/test_reading_stats.py checks that the
# two tables are the same.
JAPANESE_LETTERS = (
    (0x3005, 0x3007), (0x3021, 0x3029), (0x3031, 0x3035), (0x3038, 0x303C), (0x3041, 0x3096),
    (0x3099, 0x309A), (0x309D, 0x309F), (0x30A1, 0x30FA), (0x30FC, 0x30FF), (0x31F0, 0x31FF),
    (0x3400, 0x4DBF), (0x4E00, 0x9FFF), (0xF900, 0xFA6D), (0xFA70, 0xFAD9), (0xFF66, 0xFF9F),
    (0x16FE3, 0x16FE3), (0x16FF2, 0x16FF6), (0x1AFF0, 0x1AFF3), (0x1AFF5, 0x1AFFB), (0x1AFFD, 0x1AFFE),
    (0x1B000, 0x1B122), (0x1B132, 0x1B132), (0x1B150, 0x1B152), (0x1B155, 0x1B155), (0x1B164, 0x1B167),
    (0x20000, 0x2A6DF), (0x2A700, 0x2B81D), (0x2B820, 0x2CEAD), (0x2CEB0, 0x2EBE0), (0x2EBF0, 0x2EE5D),
    (0x2F800, 0x2FA1D), (0x30000, 0x3134A), (0x31350, 0x33479),
)
_FIRSTS = [first for first, _ in JAPANESE_LETTERS]


def is_japanese_letter(cp: int) -> bool:
    k = bisect.bisect_right(_FIRSTS, cp) - 1
    return k >= 0 and cp <= JAPANESE_LETTERS[k][1]


def counts(ch: str) -> bool:
    """Whether one character (a code point) counts: an ASCII letter or digit, or a Japanese letter."""
    cp = ord(ch)
    return ch.isalnum() if cp < 0x80 else is_japanese_letter(cp)


def characters(text: str) -> int:
    """The characters of a line as the history counts them (core/reading_chars.h). Decode a line
    from bytes that are not UTF-8 with errors="replace": U+FFFD counts nothing, as in the C count."""
    return sum(1 for ch in text if counts(ch))


@dataclass
class Record:
    game: str        # uuid: platform and title ID
    name: str
    time: float      # UTC Unix seconds
    characters: int
    stored: bool     # the count came from the file (else this tool counted the line)
    seconds: float | None = None  # reading time credited to it, None after a break
    source: str = "unknown"       # where the line came from: hook, ocr or unknown (SOURCES)
    us: int = 0      # the time in whole microseconds, as written (Rubi's six digits: exact)


@dataclass
class Totals:
    lines: int = 0
    characters: int = 0
    timed_characters: int = 0
    seconds: float = 0.0

    def add(self, r: Record) -> None:
        self.lines += 1
        self.characters += r.characters
        if r.seconds is not None:
            self.timed_characters += r.characters
            self.seconds += r.seconds

    def per_hour(self) -> float | None:
        return self.timed_characters * 3600.0 / self.seconds if self.seconds > 0 else None


@dataclass
class Report:
    records: list
    files: int = 0
    duplicates: int = 0
    incomplete: int = 0
    stored: int = 0        # rows whose count came from the file
    mismatched: int = 0    # of those, counts this tool's count disagrees with
    bad_times: int = 0     # records skipped for a time that is not Unix seconds
    cut_headers: int = 0   # files whose header a power loss cut short
    source_cells: int = 0    # records whose line_source cell says where the line came from
    source_engines: int = 0  # records without one whose engine cell says it (an older Rubi's)


class HistoryError(Exception):
    pass


# Where a record of Rubi's writer starts: its uuid (the writer's platform, "vita" or "psp", and a title
# ID) and its given_identifier, both quoted. Inside a quoted cell every quote is doubled, so this never
# matches there; among a record's cells only its first two can look like this pair (a game's name that
# is "vita-..." followed by a line that is exactly "vjo-N-N" could, which no game has).
RECORD_START = re.compile(rb'"(?:vita|psp)-[^",\n]*","vjo-[0-9]+-[0-9]+",')
RUBI_ID = re.compile(r"vjo-[0-9]+-[0-9]+")
BOM = b"\xef\xbb\xbf"
# The longest record Rubi writes is under 10 KB (the text and the recognizer's text share 4,096 bytes,
# and a quote is written twice): a record's end is looked for in the first 16 KiB after its start.
RECORD_MAX = 16384


def starts_a_record(data: bytes) -> bool:
    """Whether data begins with the start of a record of Rubi's writer (its quoted "vita-" or "psp-" uuid),
    whole or cut short."""
    return any(data.startswith(p) or p.startswith(data) for p in (b'"vita-', b'"psp-'))


def split_records(lines):
    """Each record's bytes, from its start to the next one's: a record cut short by a power loss and
    then followed by more appends (the writer appends after it) ends where the next one starts."""
    current = bytearray()
    for line in lines:
        at = 0
        for m in RECORD_START.finditer(line):
            current += line[at:m.start()]
            if current:
                yield bytes(current)
                current = bytearray()
            at = m.start()
        current += line[at:]
    if current:
        yield bytes(current)


def csv_rows(data: bytes):
    """The non-empty csv rows of data, or None when csv refuses it (a NUL before Python 3.11, a cell
    over its field limit)."""
    try:
        return [r for r in csv.reader(io.StringIO(data.decode("utf-8", errors="replace"), newline="")) if r]
    except csv.Error:
        return None


def whole_record(data: bytes, cells: int):
    """data's cells when data is one whole record: one row of at least `cells` cells, ended by a line
    break (the writer ends every record with one). None otherwise."""
    if not data.endswith(b"\n"):
        return None
    rows = csv_rows(data)
    return rows[0] if rows and len(rows) == 1 and len(rows[0]) >= cells else None


def is_header(row) -> bool:
    return bool(row) and row[0].strip().lstrip("\ufeff") == "uuid"


def ignorable(data: bytes) -> bool:
    """Blank lines, or whole header lines (a file appended to another): no record, and none cut short. A
    header cut short (a joined file's own first append, cut) is not: the record that append held is lost."""
    if not data.strip():
        return True
    if not data.rstrip(b" \t").endswith(b"\n") or not data.strip().lstrip(BOM).startswith(b"uuid"):
        return False
    rows = csv_rows(data)
    return bool(rows) and all(is_header(r) for r in rows)


def parse_chunk(data: bytes, cells: int):
    """The whole record at the start of what split_records gave (none, or one) and how many records
    were cut short in it. What follows the record's last line break and is not blank or a header is a
    record cut short (one whose first two cells did not even make it into the file, or the remains of
    one)."""
    if ignorable(data):
        return [], 0
    row = whole_record(data, cells)
    if row:
        return [row], 0
    # A quoted cell can hold line breaks: the record ends at the first line break after which it is one
    # whole record.
    nl = data.find(b"\n")
    while 0 <= nl < RECORD_MAX:
        row = whole_record(data[:nl + 1], cells)
        if row:
            return [row], int(not ignorable(data[nl + 1:]))
        nl = data.find(b"\n", nl + 1)
    return [], 1


def plain_rows(lines, cells: int, report: Report):
    """The rows of a file that is not the writer's own bytes (another tracker's ExStatic lines CSV, or
    Rubi's re-saved by a spreadsheet, without the writer's quotes): plain csv, one row a record."""
    reader = csv.reader(line.decode("utf-8", errors="replace") for line in lines)
    while True:
        try:
            row = next(reader)
        except StopIteration:
            return
        except csv.Error:
            report.incomplete += 1
            continue
        if not row or is_header(row):
            continue
        if len(row) < cells:
            report.incomplete += 1
            continue
        yield row


def read_file(path: Path, report: Report, seen: set, names: dict) -> None:
    """Adds one history file's records to report."""
    try:
        with path.open("rb") as f:
            read_lines(path, f, report, seen, names)
    except OSError as e:  # the open, or a read (a failing memory card or reader)
        raise HistoryError(f"{path}: {e.strerror or e}") from e


def read_lines(path: Path, f, report: Report, seen: set, names: dict) -> None:
    """read_file's work on the open file f."""
    first = f.readline(HEADER_READ)
    while True:  # blank lines (or a BOM alone) before the header
        if first.startswith(BOM):
            first = first[len(BOM):]
        if not first or first.strip():
            break
        first = f.readline(HEADER_READ)
    if not first.strip():
        raise HistoryError(f"{path}: empty file")
    data, writers = None, False
    # A power loss cut the file's first append short in its header, and the writer, which writes the
    # header only into an empty file, appended the next record right after the cut: before the first
    # line's first quote (Rubi's header has none) a part of the header, then maybe the zeros a lost write
    # leaves, and at the quote a record (cut short too, maybe).
    quote = first.find(b'"')
    part = first[:quote] if quote > 0 and (starts_a_record(first[quote:]) or RECORD_START.search(first, quote)) else b""
    if part and HEADER_LINE.startswith(part.rstrip(b"\0")):
        header, data, writers = HEADER, first[quote:], True
        report.cut_headers += 1
        report.incomplete += 1  # the record that append held, lost with the header's end
    else:
        # A cut header with a whole one after it on the same line (a file whose first append was cut in its
        # header and held nothing more, joined to another by cat): the whole one is the header.
        glued = first.rfind(b"uuid,given_identifier,name,line,time")
        before = first[:glued]
        before = (before[:-len(BOM)] if before.endswith(BOM) else before).rstrip(b"\0")
        if glued > 0 and before and HEADER_LINE.startswith(before):
            first = first[glued:]
            report.cut_headers += 1
            report.incomplete += 1
        text = first.decode("utf-8", errors="replace").rstrip("\r\n")
        try:
            header = [h.strip() for h in next(csv.reader([text]))]
        except csv.Error as e:
            if "\r" in text and all(name in text.split("\r", 1)[0] for name in REQUIRED):
                raise HistoryError(f"{path}: its lines end in a carriage return alone (an old Mac format): "
                                   "save it with LF or CRLF line endings") from e
            raise HistoryError(f"{path}: not a Rubi history.csv (its first line is not CSV: {e})") from e
    missing = [name for name in REQUIRED if name not in header]
    if missing:
        raise HistoryError(f"{path}: not a Rubi history.csv (no {', '.join(missing)} column)")
    report.files += 1
    col = {}
    for i, name in enumerate(header):
        col.setdefault(name, i)  # a name twice (a cut header with another file's after it): its first
    rubi = header[:len(HEADER_080)] == HEADER_080
    # The count's cell: in Rubi's rows always the 42nd (also in a file that 0.8.0 began, whose header names
    # the first 41 only), else the header's `characters`; the line's source the 43rd, after it (also in a file
    # that 0.8.0 or 0.9 began), else the header's `line_source`; the engine, which says it in older rows, the 8th.
    cols = ((len(HEADER_080), len(HEADER_080) + 1, HEADER_080.index("engine")) if rubi else
            (col.get("characters"), col.get("line_source"), col.get("engine")))
    # every cell Rubi writes up to line_metrics, or the columns another file names
    cells = len(HEADER_080) if rubi else max(col[name] for name in REQUIRED) + 1
    if data is None:
        data = f.readline()
        while data and not data.strip():
            data = f.readline()
    lines = itertools.chain([data], f)
    if writers or rubi and RECORD_START.search(data):
        for chunk in split_records(lines):
            rows, cut = parse_chunk(chunk, cells)
            report.incomplete += cut
            for row in rows:
                add_row(report, seen, names, col, cols, row)
    else:
        for row in plain_rows(lines, cells, report):
            add_row(report, seen, names, col, cols, row)


def placeable(t: float) -> bool:
    """Whether datetime can place the Unix time t on a day, in this computer's time zone and in UTC (not
    after 2038 where time_t has 32 bits)."""
    try:
        datetime.datetime.fromtimestamp(t)
        datetime.datetime.fromtimestamp(t, datetime.timezone.utc)
    except (OverflowError, OSError, ValueError):
        return False
    return True


def cell(row, i) -> str:
    """Cell i of row, stripped and in lower case; "" when the row has none (or i is None)."""
    return row[i].strip().lower() if i is not None and i < len(row) else ""


def line_source(report: Report, row, source_col, engine_col) -> str:
    """Where a record's line came from (SOURCES): its line_source cell, hook or ocr; else its engine cell, as
    an older Rubi wrote it (hook, or vocr: OCR); else unknown. One string object per source, however many
    records (as for the games' names)."""
    said = CELL_SOURCES.get(cell(row, source_col))
    if said:
        report.source_cells += 1
        return said
    engine = ENGINE_SOURCES.get(cell(row, engine_col))
    if engine:
        report.source_engines += 1
        return engine
    return "unknown"


def add_row(report: Report, seen: set, names: dict, col: dict, cols, row) -> None:
    """Adds one whole record. cols: the indexes of its characters, line_source and engine cells (or None)."""
    count_col, source_col, engine_col = cols
    uuid, ident, when = row[col["uuid"]].strip(), row[col["given_identifier"]].strip(), row[col["time"]].strip()
    if not uuid or not ident or not when:
        report.incomplete += 1
        return
    try:
        t = float(when)
    except ValueError:
        t = math.nan
    if not math.isfinite(t):  # not a number (a damaged record, often): unreadable
        report.incomplete += 1
        return
    if not 0 < t < TIME_MAX or not placeable(t):
        report.bad_times += 1
        return
    # Rubi's given_identifier names one record; another tracker's may name a game (exSTATic's does),
    # so a record of theirs is also known by its time and text.
    key = (uuid, ident) if RUBI_ID.fullmatch(ident) else (uuid, ident, when, row[col["line"]])
    if key in seen:
        report.duplicates += 1
        return
    seen.add(key)
    counted = characters(row[col["line"]])
    count = row[count_col].strip() if count_col is not None and count_col < len(row) else ""
    stored = count.isascii() and count.isdigit() and len(count) <= 9  # a line holds at most 4,095 characters
    if stored:
        report.stored += 1
        report.mismatched += int(count) != counted
    name = row[col["name"]].strip() or uuid
    whole, dot, frac = when.partition(".")
    us = (int(whole) * 1_000_000 + int(frac) if whole.isascii() and whole.isdigit() and dot and len(frac) == 6
          and frac.isascii() and frac.isdigit() else round(t * 1e6))
    # one string object per game and name, however many records: a long history stays small
    report.records.append(Record(game=names.setdefault(uuid, uuid), name=names.setdefault(name, name),
                                 time=t, characters=int(count) if stored else counted, stored=stored,
                                 source=line_source(report, row, source_col, engine_col), us=us))


def read_history(paths) -> Report:
    report, seen, names = Report(records=[]), set(), {}
    for path in paths:
        read_file(Path(path), report, seen, names)
    report.records.sort(key=lambda r: r.time)
    return report


# The adaptive reading time: what the overlay's Triangle view counts with config.ini's reading_time =
# adaptive, its default (core/history_lines.c vjo_reading_time_add, docs/ocr-history.md#reading-time), in
# the same integers, so this tool gives the view's numbers (for records in time order: it sorts them, and the
# overlay, which goes in file order, breaks an interval where the clock was set back). Each line is credited
# the time to the next record of its game, at most 2.5 times what the reader's pace would take for it, 2 s to
# 5 minutes. The pace, per game, is a running 75th percentile of the time a character took (in 64 us units,
# 16 bits):
# 3/32 up after a line of 5 or more characters read in 1 s to 5 minutes more slowly than it, 1/32 down
# after one read faster; 1.2 s a character until it has learned, and kept between 2 ms and 4.19 s a character.
# As GameSentenceMiner's adaptive cap
# (util/stats/stats_util.py adaptive_cap_seconds, reading_speed_sample), without its sessions.
PACE_UNIT_SHIFT = 6       # VJO_READING_PACE_UNIT_SHIFT: 64 us a character
PACE_START = 18750        # VJO_READING_PACE_START: 1.2 s a character, 3,000 an hour
PACE_MIN = 32             # VJO_READING_PACE_MIN: 2 ms a character, the smallest pace whose 1/32 step is not 0
CAP_FLOOR_US = 2_000_000
CAP_CEILING_US = 300_000_000
CAP_PER_UNIT_US = 160     # 2.5 x 64 us


def adaptive_credit(gap_us: int, chars: int, pace: int) -> tuple[int, int]:
    """The microseconds credited to a line of `chars` characters whose next record came gap_us (> 0) later,
    and the game's pace after it (0: not learned yet)."""
    g = min(gap_us, 0xFFFFFFFF)
    p = pace or PACE_START
    expect = p * chars
    cap = CAP_CEILING_US if expect >= CAP_CEILING_US // CAP_PER_UNIT_US else expect * CAP_PER_UNIT_US
    cap = max(cap, CAP_FLOOR_US)
    if chars >= 5 and 1_000_000 <= g <= CAP_CEILING_US:
        p = p + 3 * (p >> 5) if (g >> PACE_UNIT_SHIFT) > expect else p - (p >> 5)
        pace = min(max(p, PACE_MIN), 0xFFFF)  # 16 bits; under PACE_MIN no line could move it again
    return min(g, cap), pace


def credit_time(records, break_seconds: float | None = None) -> None:
    """Each record's reading time: its line's share of the gap to the next record of the same game (the
    records are in time order). With break_seconds, a gap of more than that is a break and the line gets
    none (Rubi 0.9.7's rule: the overlay's 15 s, this tool's 5 minutes); without, the adaptive rule."""
    paces: dict = {}
    for r, nxt in zip(records, records[1:]):
        if nxt.game != r.game:
            r.seconds = None
        elif break_seconds is not None:
            gap = nxt.time - r.time
            r.seconds = gap if 0 <= gap <= break_seconds else None
        elif nxt.us > r.us:
            credit, paces[r.game] = adaptive_credit(nxt.us - r.us, r.characters, paces.get(r.game, 0))
            r.seconds = credit / 1e6
        else:  # the same time, or a clock set back: no interval
            r.seconds = None
    if records:
        records[-1].seconds = None


def summarize(records, utc: bool = False):
    total, games, days, names = Totals(), {}, {}, {}
    zone = datetime.timezone.utc if utc else None
    for r in records:
        total.add(r)
        games.setdefault(r.game, Totals()).add(r)
        names[r.game] = r.name  # the latest name the game was written with
        day = datetime.datetime.fromtimestamp(r.time, zone).date().isoformat()
        days.setdefault(day, Totals()).add(r)
    return total, games, names, days


def by_source(records) -> dict:
    """The totals of each source (SOURCES, in that order): hook and ocr always, unknown when a record is.
    A record's reading time is its own line's, so it goes to its line's source."""
    sources = {s: Totals() for s in SOURCES}
    for r in records:
        sources[r.source].add(r)
    if not sources["unknown"].lines:
        del sources["unknown"]
    return sources


SOURCE_LABELS = {"hook": "Native text hook", "ocr": "OCR", "unknown": "Unknown"}


def duration(seconds: float) -> str:
    minutes = int(round(seconds / 60.0))
    return f"{minutes // 60} h {minutes % 60:02d} m"


def width(text: str) -> int:
    """Columns text takes in a terminal: two for a wide (kanji, kana) or full-width character."""
    return sum(2 if unicodedata.east_asian_width(ch) in "WF" else 1 for ch in text)


def table(rows):
    """rows of (label, Totals) as aligned text lines."""
    head = ("", "lines", "characters", "reading time", "characters/hour")
    cells = [head] + [(label, f"{t.lines:,}", f"{t.characters:,}", duration(t.seconds),
                       "-" if t.per_hour() is None else f"{t.per_hour():,.0f}") for label, t in rows]
    widths = [max(width(c[i]) for c in cells) for i in range(len(head))]

    def pad(text, i):
        return " " * (widths[i] - width(text))
    return ["  ".join(c[0] + pad(c[0], 0) if i == 0 else pad(c[i], i) + c[i] for i in range(len(head))).rstrip()
            for c in cells]


def render(report: Report, break_minutes: float | None, utc: bool) -> str:
    total, games, names, days = summarize(report.records, utc)
    skipped = []
    if report.duplicates:
        skipped.append(f"{report.duplicates:,} in more than one file counted once")
    if report.incomplete:
        skipped.append(f"{report.incomplete:,} incomplete skipped")
    if report.bad_times:
        skipped.append(f"{report.bad_times:,} with a time that is not Unix seconds skipped")
    out = [f"{report.files} file{'s' if report.files != 1 else ''}, {total.lines:,} records"
           + (f" ({'; '.join(skipped)})" if skipped else "")]
    if report.cut_headers:
        out.append(f"A header cut short (a power loss in a new file's first append) in {report.cut_headers:,} "
                   f"file{'s' if report.cut_headers != 1 else ''}: read with Rubi's columns (docs/ocr-history.md)")
    unknown = total.lines - report.source_cells - report.source_engines
    out += [f"Characters: {report.stored:,} records carry their count, {total.lines - report.stored:,} counted here"
            + (f"; {report.mismatched:,} stored counts differ from this tool's" if report.mismatched else ""),
            f"Text source: {report.source_cells:,} records name it (line_source), {report.source_engines:,} known by "
            f"their engine (an older Rubi's), {unknown:,} unknown",
            (f"Reading time: a gap of more than {break_minutes:g} minutes between two records, or a change of game,"
             " is a break" if break_minutes is not None else
             "Reading time: each line at most 2.5 times what your pace takes for it (2 s to 5 minutes), as the"
             " overlay counts (adaptive); a change of game is a break"), ""]
    out += table([("All games", total)])
    out += ["", "By text source"]
    out += table([(SOURCE_LABELS[s], t) for s, t in by_source(report.records).items()])
    out += ["", "By game"]
    out += table([(f"{names[g]} ({g})", t) for g, t in sorted(games.items(), key=lambda kv: -kv[1].characters)])
    out += ["", f"By day ({'UTC' if utc else 'local time'})"]
    out += table(sorted(days.items()))
    return "\n".join(out) + "\n"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("history", nargs="+", help="history.csv files copied from the Vita")
    ap.add_argument("--time-rule", choices=("adaptive", "break"),
                    help="adaptive (the default, the overlay's reading_time = adaptive): each line at most 2.5 "
                         "times what the reader's pace takes for it; break: a gap of more than --break-minutes "
                         "is a break (Rubi 0.9.7 and older)")
    ap.add_argument("--break-minutes", type=float,
                    help="with --time-rule break (which it implies): a longer gap between two records is a break, "
                         "not reading time (default 5; 0.25 is the overlay's 15s rule)")
    ap.add_argument("--utc", action="store_true", help="days in UTC instead of this computer's time zone")
    args = ap.parse_args(argv)
    rule = args.time_rule or ("break" if args.break_minutes is not None else "adaptive")
    if rule == "adaptive" and args.break_minutes is not None:
        ap.error("--break-minutes goes with --time-rule break")
    break_minutes = 5.0 if args.break_minutes is None else args.break_minutes
    if rule == "break" and not (break_minutes > 0 and math.isfinite(break_minutes)):
        ap.error("--break-minutes must be a positive number")
    try:
        report = read_history(args.history)
    except HistoryError as e:
        print(f"reading_stats: {e}", file=sys.stderr)
        return 2
    credit_time(report.records, break_minutes * 60.0 if rule == "break" else None)
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")  # a console that cannot show a game's name shows "?"
    sys.stdout.write(render(report, break_minutes if rule == "break" else None, args.utc))
    return 0


if __name__ == "__main__":
    sys.exit(main())
