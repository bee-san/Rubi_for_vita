# Offline OCR history (internal)

Automatic text reads silently append to `ux0:data/VitaJPOverlay/history.csv`,
whether the text comes from OCR or a native hook; each record says which, in its
`line_source` column ([Line source](#line-source)). Recording adds no setting,
network connection or export job. Copy the file with VitaShell; the overlay's
[line history](line-history.md) also reads earlier dialogue from it.
Only successful, current-game reads in `ocr_mode = auto` are eligible: ordinary
background scans, scans on opening, and refreshes of an open overlay. Dictionary
failure does not discard recognized text. Cursor lookups, self-tests, cancelled
or failed scans, and `on_press` mode produce no records. Blank scans reset the
consecutive-text fingerprint without writing a record.

The first five columns are GSM's ExStatic lines CSV format:

| Column | Meaning |
| --- | --- |
| `uuid` | Platform and actual game title/disc ID |
| `given_identifier` | Precise observation timestamp and byte offset of the append |
| `name` | Game title, falling back to its ID |
| `line` | Entire normalized OCR dialogue, preserving its wrapped lines |
| `time` | UTC Unix seconds with six fractional digits, at the job's start |

The timestamp uses the console RTC at completion minus the job's elapsed
process time. It is an observation time, not an import time, and relies on a
correct console clock. An unavailable/invalid RTC skips the record. IDs are
written into the file; repeatedly importing the same file into GSM's
`/api/import-exstatic` endpoint skips records with those IDs. The importer
accepts extra columns; Japanese should be selected as GSM's target language
before import. Text/timestamps support reading totals, kanji history and GSM's
estimated time/speed; Anki-card counts need separate mining metadata.

The remaining columns preserve platform/title ID, engine and actual model,
raw recognized text (as much as the existing transport retained), scene and
region sequence, process time and job duration, job sequence, region and frame
geometry/format, detected and recognized line counts, CTC frames, find/load/
recognition/capture timings, capture strips and changed rows, region checksum,
capture route/memory, workspace/resident bytes and model fallback. `line_metrics`
is a `|`-separated list of `width:height:frames:flags:us` for each reported line.
The low ten flag bits are confidence in thousandths (1023 means unavailable);
0x4000 means LM decoded, 0x8000 means LM changed the line (see
`include/vjo_game_ocr.h`). No screenshot, model or dictionary content is copied.

The 42nd column, `characters`, is how many characters `line` has as a reading
tracker counts them: its kanji, kana and other Japanese letters and its ASCII
letters and digits ([Characters](#characters)). Its sum is the characters read;
divided by the reading time it is the characters per hour, which
`tools/reading_stats.py` works out ([Reading statistics](#reading-statistics)).

The last column, `line_source`, is where `line` came from: `hook` when a native
text hook supplied it, `ocr` when Rubi's OCR read it off the screen
([Line source](#line-source)).

VjoNet publishes `job_done` before appending. The UI/control thread can consume
the result while the existing worker streams it to disk; that worker cannot
replace the existing transport result until the append returns. Only game/job
facts are copied on its existing stack. The writer has a fixed 192-byte output
buffer and a 16-byte static fingerprint (source sizes, not a console memory
measurement); an append adds no allocation, thread, growing queue, second OCR-text
buffer or read of the file. A seek obtains the append offset even for a large file.
The character count is one more pass over the text, in the writer and on its
frame: 76 B more of `vjo_history_append`'s code, paid for by moving the writer's
own strings (93 B: its path and formats) into the data segment with its header,
whose new name adds 11 B. The Shell's code segment is 24 B smaller in the normal
build and 32 B in the debug one (8 and 16 B on main `f482c21`: the sections'
alignment moves), its data 104 B larger in both; the pages stay as they were
[measured: the build log's module rows and `arm-vita-eabi-nm -S`, both builds of
main `75041a0` and this change]. The writer's frame stays 456 B with its saved
registers (`-fstack-usage`), and the count's calls (`vjo_utf8_next`, 20 B;
`vjo_text_selectable` and `vjo_is_japanese_letter`, 16 + 12 B) go no deeper than
its calls that were there before (`number`, 40 B, and `sceClibSnprintf` under
it) [measured: `-fstack-usage` with the Shell's flags]. The count runs 112
Cortex-A9 instructions a code point, about 3,560 for a mean textbox of
`host/fixtures/word_lookup`'s 50 real VOCR regions (32 code points on 2.8 lines,
27 characters) [measured: the Vita compiler's code under `qemu-arm -cpu
cortex-a9` with vita-vn-ocr's instruction-count plugin, each region one text
with its line breaks]: about 8 µs at 444 MHz, and about 0.34 ms for the longest
text a result holds (1,365 kana) [estimates: one instruction a cycle]. It runs
after the result is published, while the record is written: the writer sends its
192-byte buffer to the file each time it fills, and the count comes before the
last of those writes, the sync and the close.
The line's source costs no code: the writer chooses, once for the record, a
pointer into one table of both sources' cells (`source_cells`, 35 B, in the data
segment beside the header, whose new name adds 12 B; it replaces the engine's two
cells, 18 B), keeps it in its own state on its frame, and writes the engine's cell
and, at the record's end, the `line_source` cell and the line break from it. The
data segment was full in the normal build, so the append now opens the file by
the code segment's copy of its path, which line history's reader has already (the
data segment's copy, 35 B, is gone). `vjo_history_append` stays 1,068 B of code
with a 456 B frame, the Shell's code segment keeps its size in both builds (90,100 B
normal, 100,988 B debug), and its data segment is 8 B smaller in both (73,728 →
73,720 B normal, 73,680 → 73,672 B debug), in the same pages [measured: the build
log's module rows, `arm-vita-eabi-nm -S` and `-fstack-usage`, both builds of main
`ebd9e22` and this change]. A record is 6 B longer for OCR's line (`,"ocr"`) and 7 B
for a hook's.
Both module-page budgets and thread-stack budgets remain enforced by the ledger.

The overlay reads the file back for two things. [Line history](line-history.md): when Up
first goes past the overlay's top row, the control thread reads the file's last 24,512 B once
(into its existing 24 KiB scratch) and keeps up to 32 of this game's lines, by `uuid`, in a 16 KiB Paf
block while the user looks back. And the [total characters read](#total-characters-read) that △ shows,
when it is not known yet: the control thread counts the file 24,512 B at a time in the same scratch.
Neither reader writes `history.csv`. An append in progress can only cut the last record: the reader
skips it until its line and the time's first digit after it are written, then reads its line, which is
whole ([line-history.md](line-history.md#where-the-lines-come-from-and-why)); the count leaves out the
record an append may be writing and counts it once the append is done.

Consecutive identical normalized text within a game is suppressed, including
rescans caused by a blinking icon or reopening/resuming. A different sentence,
game, or successful blank scan permits the same text again; so does, until
[#252](https://github.com/bee-san/Rubi_for_vita/issues/252) is fixed, a read
whose exchange with the game worker failed (no worker, a timeout, or a cancellation
that the history's check does not see), which the writer takes for a blank scan:
the next read of the same text is then recorded again. Fingerprints reset
on Shell restart. This is deliberately lightweight: an immediately repeated
identical sentence with no observed change may be collapsed, and a long pause
while text types out can still produce a partial sentence followed by its full
version. The existing stable-text wait mitigates this; history adds no further
text buffering or heuristics.

Short writes are retried. Write/flush/close errors are best effort and do not
change OCR or its UI; a failed append attempts to restore the previous file
size, and does not advance the fingerprint. Power loss during an append can
leave an incomplete final CSV record; discard that record before importing if
necessary (`tools/reading_stats.py` skips it). History is never rotated or
erased automatically. Disk use grows with the saved text; RAM use does not.
Console I/O latency and memory headroom for this change remain unmeasured until
a physical-console run.

## Characters

`characters` is the number of Unicode code points of `line` (the normalised
text) that are

- Japanese letters: the letters of the Han, Hiragana and Katakana scripts in
  Unicode 17.0, the table in [core/utf.c](../core/utf.c) that the overlay's
  "Japanese characters only" cursor stops on (kanji, kana, ー, 々 〆 ヶ, the
  iteration marks ゝ ゞ ヽ ヾ, half-width katakana); or
- ASCII letters and digits. In the normalised line the full-width forms
  ０-９ Ａ-Ｚ ａ-ｚ are ASCII ([core/textnorm.h](../core/textnorm.h)), so they
  count too.

Nothing else counts: no punctuation or brackets (「」『』（）。、…‥！？・―〜),
no symbols (♪ ○ ×, emoji), no spaces (U+3000 neither) and no line breaks. A
character counts once, whatever its size: 日本語 (9 bytes of UTF-8) is 3, 𠮟
(4 bytes, two UTF-16 units) is 1. Bytes that are not well-formed UTF-8 count
nothing. 「また明日、一緒に見に行こう!」 is 12, `10時に駅で待ってるね。` 11.
The Shell counts the line as it writes the record
([core/reading_chars.h](../core/reading_chars.h)), and `tools/reading_stats.py`
counts the same for rows without the column.

Punctuation does not count because the trackers Japanese learners compare their
numbers with all leave it out:

| Tracker | What it counts | Where |
| --- | --- | --- |
| GameSentenceMiner (its statistics) | code points, once every Unicode punctuation mark, symbol and separator (`[\p{P}\p{S}\p{Z}]`) is removed: the daily rollups behind its dashboard, charts and reading speed read lines that way (a few other views, such as its games list's totals, `/api/games-list`, take SQLite's `LENGTH` of the raw line) | [`util/database/db.py`](https://github.com/bpwhelan/GameSentenceMiner/blob/7a46a00e8816f3cd39891cef2c9d01e5ac88fc78/GameSentenceMiner/util/database/db.py#L37) `PUNCTUATION_REGEX_PATTERN`, `clean_text_for_stats` |
| exSTATic | UTF-16 units, once a fixed list of brackets and marks (Japanese ones, and `( ) [ ] . , ' " ?`), spaces, tabs and line breaks is removed | [`src/calculations.ts`](https://github.com/KamWithK/exSTATic/blob/8543967fb9bdf70154f521e3c8c68aa18da1339f/src/calculations.ts#L4) `IGNORE`, `charsInLine` |
| ッツ Ebook Reader (ttsu) | code points of 0-9 and A-Z (either case, either width), kana, ー, 々〆〇〻, ○◯, half-width katakana, CJK radicals and ideographs; nothing else | [`get-character-count.ts`](https://github.com/ttu-ttu/ebook-reader/blob/e7ec1d2680b5121b97102753d14a7a49a9003761/apps/web/src/lib/functions/get-character-count.ts#L13) `isNotJapaneseRegex` |

Where the counts differ, computed on every code point with each tracker's own
pattern (ttsu's and exSTATic's run by Node 22.23.1, ICU 78.2, Unicode 17.0;
GameSentenceMiner's by its pinned `regex` 2026.7.19):

- ttsu counts and Rubi does not: ○ ◯ (U+25CB, U+25EF: they hide a name's
  letters), the CJK radicals (U+2E80-2E99, U+2E9B-2EF3, U+2F00-2FD5), and ſ
  (U+017F) and K (U+212A, the Kelvin sign), which its case-insensitive match folds
  to s and k. (It counts the full-width ０-９ Ａ-Ｚ ａ-ｚ too, which Rubi's
  normalised line holds as ASCII, so both count them.)
- Rubi counts and ttsu does not: the Hangzhou numerals 〡-〩 〸-〺 (U+3021-3029,
  U+3038-303A), the vertical repeat marks 〱-〵 (U+3031-3035), 〼 (U+303C), the
  combining dakuten and handakuten (U+3099-309A), ゟ (U+309F), ヽ ヾ ヿ
  (U+30FD-30FF), the small katakana ㇰ-ㇿ (U+31F0-31FF), the half-width ﾞ ﾟ
  (U+FF9E-FF9F), the CJK compatibility ideographs but for the twelve unified ones
  among them (U+F900-FAD9, U+2F800-2FA1D), and the table's letters of the
  supplementary planes below U+20000: U+16FE3, U+16FF2-16FF6 and the kana of
  U+1AFF0-1B167 (the kana extensions, hentaigana and small kana).
- exSTATic counts every character not on its list, in UTF-16 units, so also
  what Rubi leaves out and the list does not have: ・, ! and ~ (which Rubi's
  normalised line holds for ！, 〜 and ～), ○, the other symbols and emoji, with
  𠮟 and an emoji as 2 each. Rubi counts one character exSTATic does not: the
  half-width ｰ (U+FF70).
- GameSentenceMiner counts every code point that is not punctuation, a symbol or
  a separator, and Rubi's characters are among them. It also counts the letters
  of other scripts (é Я 한), the other numbers (① Ⅳ ² ㊀, Han ones too) but for 〇
  and the Hangzhou numerals, which Rubi counts as well, every combining mark but
  the dakuten and handakuten Rubi counts (so ❤️, a symbol and a variation
  selector, is 1), the control and format characters inside a line (its line
  breaks, `\t`, a CRLF's `\r`, a zero-width joiner) and the private-use and
  unassigned code points.

On the 50 real VOCR textboxes of `host/fixtures/word_lookup` (138 lines), once
the writer normalises them: Rubi's count is 1,329, ttsu's 1,329,
GameSentenceMiner's 1,417 (the 88 line breaks inside them) and exSTATic's 1,344
(11 !, 3 ~ and one ・) [measured: each tracker's own pattern on the writer's
`line`]. GameSentenceMiner's import reads only the first five columns, so its
own totals for imported lines count `line` its way, not from this column.

## Line source

`line_source`, the 43rd column, is where the record's `line` came from:

| Value | The line |
| --- | --- |
| `hook` | a native text hook's stream supplied it ([native-hooks.md](native-hooks.md)): it agreed with OCR's reading of the same screen and replaced it, or a stream Rubi trusts answered the read without recognition |
| `ocr` | Rubi's OCR read it off the screen |

The writer takes it from the read's result (`VjoGameOcrResult.text_from`: 0 when OCR's
text is shown, else the ID of the stream whose text is), as `engine` has done since
native text hooks came: `engine` is `hook` exactly when `line_source` is, and `vocr`
when it is `ocr`, both written from one choice. Every result says where its text
came from, so the writer writes nothing else; `unknown` is `tools/reading_stats.py`'s,
for a record that says neither. For a hook's line, `raw_line` is OCR's (normalised)
reading of the screen when OCR ran and the hook's text replaced it, or the hook's
text before normalisation when the hook answered without recognition.

The column is not called `source` because the file has one: the 34th, the route the
game worker read the screen's rows through (`VJO_GAME_OCR_SRC_*`, where 1 is the
kernel's display hook: a capture hook, nothing to do with text hooks). Two columns of
one name would read as one in a reader that goes by names (`csv.DictReader` keeps the
last, pandas renames the second `source.1`).

A hook's line is recorded as OCR's is: with **Read text: Automatically**, in both
builds, every completed read of the game in front whose text is not blank and is not
the text of the record before it (a background read, a read as the overlay opens, a
refresh of the open overlay). Each of these reads asks the game worker for its
automatic choice between OCR and the hook streams, and its result is recorded,
whichever supplied it. Not recorded:

- With **Only when I press L + R** (`ocr_mode = on_press`): no line, from either
  source.
- A read that failed, was cancelled or found the screen changed under it; a cursor
  move (it reads nothing); the debug build's self-test (OCR only).
- Dialogue the game showed and replaced between two reads (skipping, or the game's
  auto mode): a hook stream keeps its latest text only, and the history has one record
  per read, not one for each line a hook saw.
- A line the same as the record before it, whatever its source: OCR's reading, then
  the same text from a hook, is one record, `ocr` (unless a read between them failed:
  [#252](https://github.com/bee-san/Rubi_for_vita/issues/252), above).
- A line of a text box's border read as text (API 36, with `non_japanese_filter = lines`,
  the default: [vita-vn-ocr.md](vita-vn-ocr.md#text-normalisation-shown-and-looked-up)):
  the game worker drops it from OCR's text and from a hook's alike, before the text is
  shown or recorded; a text that was only such lines is blank, and is not recorded.

PSP games in Adrenaline have no native hooks, and **Text source: OCR only** uses
none: their lines are all `ocr`.

Rubi 0.9.1 and older wrote no `line_source`, but their `engine` says the same: `hook`
for a hook's line (since 0.9.0; the test build 0.8.0-test.7 before it, the first with
native hooks) and `vocr` for OCR's (0.8.0 had no hooks, so all of its lines are OCR's).
`tools/reading_stats.py` takes the source of those rows from there.

## Reading statistics

`tools/reading_stats.py` (Python 3, nothing to install; in the release zip's
`tools/` folder and in the repository) reads one or more copies of
`history.csv` and prints the records, characters, reading time and characters
per hour, in all, by text source, per game and per day. It prints no dialogue text.

```sh
python3 tools/reading_stats.py history.csv      # days in this computer's time zone
python3 tools/reading_stats.py old/history.csv history.csv --break-minutes 10 --utc
```

The reading time is the time from one record to the next, credited to the first.
A gap of more than `--break-minutes` (5 by default) or a change of game is a
break: it is not reading time, and the record before it gets none, since how
long it was read is unknown. Characters per hour counts only the records that
got a time; the totals count them all. A record in more than one copy counts
once: by `uuid` and `given_identifier`, as GameSentenceMiner's import does, and
another tracker's record (whose `given_identifier` can name just its game, as
exSTATic's does) by those, its time and its line. A record cut short by a power
loss is skipped and counted, also when more records were appended after it;
blank lines and the header lines of files joined together are ignored. A file
that is not the writer's own bytes (Rubi's re-saved by a spreadsheet, or another
tracker's ExStatic lines CSV) is read as plain CSV: its `characters` cells are
used where it has them, and the tool counts the other lines. A record whose time
is a number but not Unix seconds between 1970 and 3000 (another tracker's
milliseconds, which GameSentenceMiner's import refuses too), or one this
computer cannot place on a day (after 2038 where `time_t` has 32 bits), is
skipped and counted. A file that cannot be read, is not CSV, or lacks one of the
five ExStatic columns makes the tool stop with exit status 2 and say why.
100,000 records (a 42 MB file) take about 2 s and 60 MB [measured on the host].

**By text source** splits the totals by where each line came from: a record's
`line_source` (Native text hook, OCR), or in a row without one (Rubi 0.9.1 and
older) its `engine` (`hook`, or `vocr`: OCR); a row with neither (another tracker's)
is Unknown, a row of the table only when there is one. The `Text source:` line says
how many records were placed each way. A record's reading time is its own line's, so
it goes to that line's source, and the sources' lines, characters and times add up
to the totals.

The totals count what was recorded: a line that types out slowly can be
recorded twice, once part-way and once whole (see the duplicate text above), and
then counts twice, so the totals and the characters per hour can come out a
little high.

Other trackers time reading differently, so the same reading gives them
somewhat different hours. GameSentenceMiner credits each line with the time to
the next line up to a cap: 2.5 times what its session's median pace would take
for the line (3 s a character when the session has no usable pace: no line of
five or more characters read in 1 s to 5 minutes), but at least 2 s and at most
5 minutes; a line it counts no character in gets nothing, and a gap of more than
30 minutes ends the session
(`session_gap_seconds`; [`util/stats/stats_util.py`](https://github.com/bpwhelan/GameSentenceMiner/blob/7a46a00e8816f3cd39891cef2c9d01e5ac88fc78/GameSentenceMiner/util/stats/stats_util.py),
`web/stats.py` `calculate_actual_reading_time`). exSTATic's visual-novel
tracker counts time while it runs and stops its clock 60 s after the last line
(`afk_max_time`, [`src/vn/vn.svelte`](https://github.com/KamWithK/exSTATic/blob/8543967fb9bdf70154f521e3c8c68aa18da1339f/src/vn/vn.svelte)).

## Total characters read

With the overlay open, △ (Triangle) shows the line's source view
([native-hooks.md](native-hooks.md#hidden-line-diagnostics)), and its first line is the total:

```
Total characters read: 1,234,567
```

It is the sum of [`characters`](#characters) over every record of `history.csv`, every game's: the
characters column of `tools/reading_stats.py`'s **All games** row for the same file. The two differ only
where a power loss cut a record: the Shell counts a cut record when its line and the time's first digit
after it were written, never the record that the next boot appended right after the cut (it runs on from
the cut one, with no line break before it), nor one after a header cut short; the tool counts the record
after the cut and never the cut one (it is skipped as incomplete). `characters` is not the line's length:
punctuation, brackets, symbols, spaces and line breaks count nothing ([Characters](#characters)). On the 50
real VOCR textboxes of `host/fixtures/word_lookup`, written by the writer, the column's sum is 1,329, the
lines' code points 1,593 (`len()`, 19.9 % more) and 1,505 without their 88 line breaks (13.2 % more; 176
marks: `。、「」！？…` and the like), and their UTF-8 bytes 4,533 [measured: the writer's records, Python's
`len` on each `line`]. The total counts what was recorded, so a line recorded twice (a part-way read,
#252) counts twice, as in the tool. It is every game's: a total of the game in front would need another
count of the whole file at each change of game, or a table of every game's, and the normal Shell's code
pages have 4 B of room left (docs/memory-ledger.md#the-reading-total).

**How it is kept, so that a press costs nothing.** The Shell does not count the file at a press. It
keeps the total, the characters of the records that start in `history.csv`'s first `covered` bytes, in its
data (56 B), under the view's lock:

- While the total is known, the writer (VjoNet) adds each record's `characters` as it appends the
  record, when the record starts where the total's bytes end; otherwise it marks the total unknown and
  wakes the control thread to count what it does not know. Every append, whatever the total's state, also
  takes and releases the view's mutex (two system calls, unmeasured) and raises a sequence number twice,
  with a memory barrier each time: some 40 instructions a record (`vjo_history_append` 1,068 → 1,100 B of
  code, `flush` +6 B), after the result was published and the record written, synced and closed, as before
  [measured: the normal Shell's compiled code].
- It is counted from the file only when it is not known: at the first press ever, at the first press after
  the Shell starts (from where the kept total ends: only the records appended since it was kept), and when
  `history.csv` was replaced, cut or changed under it (the writer then asks for the count itself). The view
  says `counting…` until the count ends and then shows the number (it is drawn again). The control thread
  reads the file 24,512 B at a time, with the byte before them, into its existing 24 KiB scratch (line
  history's window), one window a wakeup, after the wakeup's other work, and wakes itself for the next. A
  count that an append ran beside goes on from there at the next wakeup (50 ms at most). A file that ends in
  a record with no line break after it (one a power loss cut) is counted but for that record, which waits
  for the next record's append: the count of the two then takes the cut one only, as a fresh count does. A
  failed open or read makes the view say `unavailable`; the next press tries again.
- `ux0:data/VitaJPOverlay/history_total.bin` keeps it between starts: 16 bytes, the total (64-bit), the
  bytes of `history.csv` it is the total of and a check (32-bit each, little-endian), written through a
  temporary file and a rename when a count ends and at each game's exit when lines were added. A missing,
  torn or wrong file (its check, a size past the file's end, no record starting where its bytes end), or
  one of another `history.csv`, is not used: the whole file is counted again. Moving `history.csv` away
  (Rubi then begins a new one) starts the total again: it is the file's. (A file put in its place with a
  record starting exactly where the kept total ends is taken for the same file.)
- A `history.csv` of 4 GiB or more (some ten million records) has no total: `unavailable`.

What it costs [host: measured, an AMD EPYC 9R14, the median of 5 runs of `vjo-stage-bench`; A9: the Vita
compiler's code of the normal build under `qemu-arm -cpu cortex-a9`, `tools/bench_stages.py a9`, costed
with vita-vn-ocr's Cortex-A9 model: an estimate; nothing of this has run on a console]:

| | host | Cortex-A9, 333 MHz (444 MHz) |
| --- | --- | --- |
| a press, the total known: the number with its commas (`total_number`) | 38 ns | 578 instructions: 0.9-1.8 µs (0.7-1.4 µs) |
| a record's append | | some 40 instructions and the mutex's two system calls (unmeasured) |
| the count, a record (`total_count`, 2,000 records of 512 B) | 0.80 µs | 9,060 instructions: 14-30 µs (10-22 µs) |
| line history's read, for comparison (`history_scan`, 24,513 B) | 21 µs | 297,258 instructions; 316,983 → 297,608 (−6.1 %) on main's and this change's libraries in a copy of the bench that stubs the total's functions (main's library has none): its record parser is shared now |

A press also has paf lay out one more line of the view (33 characters), whose cost is unmeasured. The count
of a whole file was measured on the host with `vjo-history-writer --total` (the Shell's code, its windows
and reads; records of the 50 textboxes in turn, 408 B each as the writer writes them; the medians of 21
counts in four sessions on a shared 64-core host, which differed by up to 18 %: the range). On the console it is the CPU, 14-30 µs a record (the A9 figure above, 333 MHz), and the reads, whose
speed from the Shell is not measured for 24 KiB reads. Three bounds [estimates]: (a) the kernel broker's
rate, 7.0-14.6 MB/s (on the console H15's 1,155,968 B weights were read through it, in 32 KiB pieces, and
SHA-256-checked in 218-231 ms; the SHA-256's own 66-139 ms is itself an A9 estimate: `docs/performance.md`);
(b) each window's read costing what a dictionary read costs the Shell, 3.82 ms a `sceIoLseek` +
`sceIoRead` [measured on the console: `docs/performance.md`], plus its bytes at (a)'s rate; (c) 3.82 ms for
each 4 KiB of it, if a read's cost grows with its bytes as with small random reads:

| records | bytes | windows | host | console: (a) | (b) | (c) |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1,000 | 407,617 | 17 | 0.77-0.92 ms | 0.04-0.09 s | 0.11-0.15 s | 0.4 s |
| 10,000 | 4,072,498 | 168 | 7.5-8.9 ms | 0.4-0.9 s | 1.1-1.5 s | 3.9-4.1 s |
| 100,000 | 40,721,308 | 1,676 | 76-90 ms | 4.2-8.8 s | 10.6-15.2 s | 39-41 s |

(b) is the likeliest; (c) the most it could take. That is the first press's wait at most, once after an
install; afterwards a press costs the number's formatting, and a restart's first press counts only what was
recorded since the total was kept (each game's exit keeps it): for up to some 60 lines, one window, est.
35-55 ms (two opens at the dictionary's 15.3 ms and one to six 4 KiB reads at its 3.82 ms:
`docs/performance.md`). While a count runs:

- The control thread's other work in each wakeup comes first, so a job, a refresh or a lookup it starts
  waits at most for one window (est. 5.5-7.3 ms of reading by (b), 23 ms by (c), and 0.8-1.8 ms of counting
  at 333 MHz). The overlay's input and drawing are the paf thread's and do not wait.
- Words are not looked up ahead in the background (`prepare_words` runs only in a wakeup with no event, and
  a count's self-wakeup is one), so until the count ends a cursor move waits for its own lookup (70-81 ms on
  the console: `docs/performance.md`) where it could have found it prepared [read from the code; not
  measured].
- The count's reads share the memory card with the game's own reads and with the OCR worker's model reads
  through the kernel's broker, which can slow both while it lasts [not measured].

Nothing of it runs in the game: the game worker and the kernel plugin are the same bytes, and the OCR job's
code is unchanged; the record's append, which the next job already waited for, takes the lock after its
file is closed. CPU3 use is unmeasured (AGENTS.md rule 8).

## The format and older files

The file has no version number: its first line names its columns. Rubi 0.8.0
wrote the 41 columns up to `line_metrics`; 0.9.0 added `characters`, the 42nd,
and this version `line_source`, the 43rd; every other column keeps its position.
A file is never rewritten, so a `history.csv` that 0.8.0 began keeps its 41-name
header, and one that 0.9.0 or 0.9.1 began its 42-name one; the rows appended to it
since have more cells than the header names. Readers that go by the header's names
ignore those cells, as GameSentenceMiner's import (Python's `csv.DictReader`)
does; `tools/reading_stats.py` reads them by position, the 42nd as the count and
the 43rd as the line's source, counts `line` itself for the rows without a count and
takes the source of the rows without one from their `engine`
([Line source](#line-source)). A reader that wants as many cells in every row as the
header has names stops at the first longer row (pandas' `read_csv` does, by
default). So pandas, which reads a file that 0.9.0 or 0.9.1 began as they left it,
refuses it from the first row this version appends (`Expected 42 fields ..., saw 43`),
as it refused a file 0.8.0 began from 0.9.0's first row [checked: pandas 2.2.3 on
the fixtures]. Add the missing names to the end of the file's first line (on Linux,
`sed -i '1s/line_metrics$/&,characters/; 1s/characters$/&,line_source/' history.csv`
adds both to a 0.8.0 header and `line_source` to a 0.9 one, and changes nothing in a
new one), or move the file away on the Vita so that Rubi starts a new one with the
new header.

A power loss in a new file's very first append can cut its header short, and the
record that append held is lost. The writer writes the header only into an empty
file, so the next record then follows the cut on the first line (maybe after the
lost write's zeros, and maybe cut short itself). GameSentenceMiner's import then
refuses the file, when the cut left less than
`uuid,given_identifier,name,line,time,` (its first 37 bytes), or leaves out the
record on the first line [checked: its import's header check and `DictReader` at
`7a46a00`, every cut of the header]; `tools/reading_stats.py` reads every record
after the cut with Rubi's columns, counts the lost one as cut short and says so.
To import the file whole, replace what comes before the first record (`"vita-`
or `"psp-`) with this header line and a line break:

```
uuid,given_identifier,name,line,time,platform,title_id,engine,model,raw_line,scene,region_seq,monotonic_us,elapsed_us,job_seq,region_x,region_y,width,height,fb_width,fb_height,fb_pitch,fb_format,lines,recognized,frames,find_us,load_us,recognize_us,capture_us,captures,tears,region_sum,source,capture_memory,capture_bytes,strip_rows,workspace_bytes,resident_bytes,fallback,line_metrics,characters,line_source
```
