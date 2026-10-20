#!/usr/bin/env python3
"""Reading statistics from Rubi's reading history (docs/ocr-history.md): the characters read and
the characters per hour, in all, per game and per day.

    python3 tools/reading_stats.py history.csv [MORE.csv ...] [--break-minutes 5] [--utc]

Copy ux0:data/VitaJPOverlay/history.csv from the Vita first (VitaShell's USB or FTP connection).
Several files (copies made at different times) are read as one history: a record in more than one
of them counts once (by its uuid and given_identifier, as GameSentenceMiner's import does).

Characters: each record's `characters` column, the count of the dialogue's kanji, kana and other
Japanese letters and its ASCII letters and digits; punctuation, brackets, symbols, spaces and line
breaks count nothing (docs/ocr-history.md, core/reading_chars.h). A file Rubi 0.8.0 wrote has no
such column, and a file it began keeps its header when a newer Rubi appends to it: those rows carry
the count as an extra, 42nd cell. Where a row has no count this tool counts its `line` itself, the
same way.

Reading time: the time from one record to the next, credited to the first. A gap of more than
--break-minutes (default 5) between two records, or a change of game, is a break: it is not
reading time, and the record before it gets none (how long it was read is unknown). Characters per
hour divides the characters of the records that got a time by that time; the totals count every
record.

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
# `characters` column comes after them.
HEADER_080 = (
    "uuid,given_identifier,name,line,time,platform,title_id,engine,model,raw_line,scene,region_seq,"
    "monotonic_us,elapsed_us,job_seq,region_x,region_y,width,height,fb_width,fb_height,fb_pitch,"
    "fb_format,lines,recognized,frames,find_us,load_us,recognize_us,capture_us,captures,tears,"
    "region_sum,source,capture_memory,capture_bytes,strip_rows,workspace_bytes,resident_bytes,"
    "fallback,line_metrics").split(",")
REQUIRED = ("uuid", "given_identifier", "name", "line", "time")
# The header this writer writes (0.8.0's columns, then `characters`): what a file whose first append was
# cut short in its header is read with.
HEADER = HEADER_080 + ["characters"]
HEADER_LINE = (",".join(HEADER) + "\n").encode()
# A time is Unix seconds from 1970 to before 3000 (Rubi's are this century), so another tracker's
# milliseconds are not taken for seconds (GameSentenceMiner's import refuses them too). datetime places
# those on a day where time_t has 64 bits; where it has 32, it cannot after 2038, and such a time is
# skipped too (add_row).
TIME_MAX = 32503680000.0  # 3000-01-01T00:00:00Z
# The most of a file's first line read as its header: Rubi's is 396 bytes, and after a cut header the next
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
    # the first 41 only), else the header's `characters`.
    count_col = len(HEADER_080) if rubi else col.get("characters")
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
                add_row(report, seen, names, col, count_col, row)
    else:
        for row in plain_rows(lines, cells, report):
            add_row(report, seen, names, col, count_col, row)


def placeable(t: float) -> bool:
    """Whether datetime can place the Unix time t on a day, in this computer's time zone and in UTC (not
    after 2038 where time_t has 32 bits)."""
    try:
        datetime.datetime.fromtimestamp(t)
        datetime.datetime.fromtimestamp(t, datetime.timezone.utc)
    except (OverflowError, OSError, ValueError):
        return False
    return True


def add_row(report: Report, seen: set, names: dict, col: dict, count_col, row) -> None:
    """Adds one whole record."""
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
    cell = row[count_col].strip() if count_col is not None and count_col < len(row) else ""
    stored = cell.isascii() and cell.isdigit() and len(cell) <= 9  # a line holds at most 4,095 characters
    if stored:
        report.stored += 1
        report.mismatched += int(cell) != counted
    name = row[col["name"]].strip() or uuid
    # one string object per game and name, however many records: a long history stays small
    report.records.append(Record(game=names.setdefault(uuid, uuid), name=names.setdefault(name, name),
                                 time=t, characters=int(cell) if stored else counted, stored=stored))


def read_history(paths) -> Report:
    report, seen, names = Report(records=[]), set(), {}
    for path in paths:
        read_file(Path(path), report, seen, names)
    report.records.sort(key=lambda r: r.time)
    return report


def credit_time(records, break_seconds: float) -> None:
    """Each record's reading time: the gap to the next record of the same game, unless it is over
    break_seconds (a break). The records are in time order."""
    for r, nxt in zip(records, records[1:]):
        gap = nxt.time - r.time
        r.seconds = gap if nxt.game == r.game and 0 <= gap <= break_seconds else None
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


def render(report: Report, break_minutes: float, utc: bool) -> str:
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
    out += [f"Characters: {report.stored:,} records carry their count, {total.lines - report.stored:,} counted here"
            + (f"; {report.mismatched:,} stored counts differ from this tool's" if report.mismatched else ""),
            f"Reading time: a gap of more than {break_minutes:g} minutes between two records, or a change of game,"
            " is a break", ""]
    out += table([("All games", total)])
    out += ["", "By game"]
    out += table([(f"{names[g]} ({g})", t) for g, t in sorted(games.items(), key=lambda kv: -kv[1].characters)])
    out += ["", f"By day ({'UTC' if utc else 'local time'})"]
    out += table(sorted(days.items()))
    return "\n".join(out) + "\n"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("history", nargs="+", help="history.csv files copied from the Vita")
    ap.add_argument("--break-minutes", type=float, default=5.0,
                    help="a longer gap between two records is a break, not reading time (default 5)")
    ap.add_argument("--utc", action="store_true", help="days in UTC instead of this computer's time zone")
    args = ap.parse_args(argv)
    if not (args.break_minutes > 0 and math.isfinite(args.break_minutes)):
        ap.error("--break-minutes must be a positive number")
    try:
        report = read_history(args.history)
    except HistoryError as e:
        print(f"reading_stats: {e}", file=sys.stderr)
        return 2
    credit_time(report.records, args.break_minutes * 60.0)
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")  # a console that cannot show a game's name shows "?"
    sys.stdout.write(render(report, args.break_minutes, args.utc))
    return 0


if __name__ == "__main__":
    sys.exit(main())
