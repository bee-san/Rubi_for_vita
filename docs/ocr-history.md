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
python3 tools/reading_stats.py old/history.csv history.csv --utc
python3 tools/reading_stats.py history.csv --break-minutes 10   # Rubi 0.9.7's rule, 10-minute breaks
```

The reading time is the overlay's ([Reading time](#reading-time)): each record's
line gets the time to the next record of its game, at most 2.5 times what the
reader's pace would take for it, in the same integers as the overlay, so the
tool's totals are the Triangle view's (`reading_time = adaptive`) for a history in time order (the tool
sorts the records by time; the overlay goes in file order, and a clock set back breaks its interval, so
after one the two can differ). A change of
game is a break. `--time-rule break` (or `--break-minutes N`) is Rubi 0.9.7's
rule instead: a gap of more than N minutes (5 by default) is a break, and the
record before it gets none, since how long it was read is unknown;
`--break-minutes 0.25` is the overlay's old 15 s. Characters per hour counts
only the records that got a time; the totals count them all. A record in more than one copy counts
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
(`session_gap_seconds`; [`util/stats/stats_util.py`](https://github.com/bpwhelan/GameSentenceMiner/blob/5190b613f5ff2c70bfe505377b297854aacf6b27/GameSentenceMiner/util/stats/stats_util.py),
`web/stats.py` `calculate_actual_reading_time`). Rubi's adaptive rule is this cap
without the sessions, with a running 75th percentile for the median
([Reading time](#reading-time)). exSTATic's visual-novel
tracker counts time while it runs and stops its clock 60 s after the last line
(`afk_max_time`, [`src/vn/vn.svelte`](https://github.com/KamWithK/exSTATic/blob/8543967fb9bdf70154f521e3c8c68aa18da1339f/src/vn/vn.svelte)).

## Reading time

A line is read from the moment Rubi records it to the moment it records the next line of the same
game, and that time is the line's, credited when the next line comes. What a gap between two records
holds besides reading is unknown: word lookups and slow reading, but also an AFK, a suspend (the PS
button, the Rubi app), standby or the next day. `reading_time` in `config.ini` says how much of a gap
counts, from the Vita's next start (the Shell takes it once, as it starts):

- **adaptive** (the default): each line gets the gap, but at most **2.5 times what your pace would take
  for it**, and no less than 2 s nor more than 5 minutes. The pace is learned per game from its own
  history: the 75th percentile of the time a character took in its lines of 5 or more characters that
  took 1 s to 5 minutes, as a running estimate (3/32 of itself up after a slower line, 1/32 down after a
  faster one; 1.2 s a character, 3,000 an hour, until it has learned; never under 2 ms a character, where
  those steps would round to nothing, nor over 4.19 s). A long pause of any kind gives the
  line before it its cap, 2.5 times its expected time, and no more; there are no sessions. In the
  simulated timelines below, a 30-character line could count up to about 2 minutes for a beginner
  (3,000 characters an hour with many lookups), 1 minute for a learner (6,000) and 20 s for a fluent
  reader (18,000).
- **15s**: Rubi 0.9.7's rule. Only a gap of 15 s or less counts, whole; a line that took longer gets
  nothing, however much of the gap was reading, and its characters leave characters/hour.

This is GameSentenceMiner's adaptive cap (`util/stats/stats_util.py` `adaptive_cap_seconds`,
`reading_speed_sample`, at `5190b613`) without its 30-minute sessions, and with a running 75th
percentile where it takes a session's median. On simulated reading timelines (readers at 3,000-18,000
characters an hour, with lookups, AFK breaks, suspends and standby, menus and the backlog, skip and
auto mode; [docs/reading-time.md](reading-time.md)) the 15 s rule counted 53 % too little time on
average (93 % for a beginner) and characters/hour 63 % too high; the adaptive rule is off by 2.2 % on
average and 9 % at most [estimates: simulated, no console history was available]. GameSentenceMiner's
own history of AFK rules, and why each changed, is there too.

**Cost.** The rule is integer arithmetic in the counters the writer and the count already share
(`core/history_lines.c` `vjo_reading_time_add`): the pace is 16 bits of the existing cache record, and
the normal Shell's code segment is 80 B smaller, its data 328 B larger, in the same pages, with three
of the overlay's cold error messages moved to the data segment (docs/memory-ledger.md) [measured: the
build log's module rows, both builds]. The time a count takes on a console is unmeasured.

## Total characters read

With the overlay open, △ (Triangle) shows the selected line's source, followed by two reading blocks:

> This line came in through: hooking
>
> **TODAY · THIS GAME**\
> **8,420 characters**\
> 48 min reading · 10,525 chars/hour
>
> **THIS GAME · TOTAL**\
> **142,680 characters**\
> 16 h 25 min reading · 8,691 chars/hour

The source is `hooking`, `ocr`, or `Unknown` for a line without provenance, including older history.
The technical stream, flags, similarity, model and capture fields are no longer displayed here.
Both blocks cover the current game's recorded lines, identified separately for PSP and Vita.
Today means the Vita's local calendar day, across closes and restarts, not a session. Its UTC
bounds come from both local midnights. A change of day or console timezone rebuilds the daily
counters on the next press or recorded line after midnight. Intervals crossing midnight are
excluded from today's time/speed; they can still count in the game's total. The heading uses
larger character-count text with dimmer supporting time/speed, in the existing PAF text pane.
Characters retain the [character definition](#characters). Repeated lines recorded separately
count separately. Only automatically recorded text contributes; on-press mode records none.

**Time played is an estimate of active reading**, using the existing Unix timestamps, in file order:
each line is credited when the next recorded line of the same game arrives, by the rule of
[Reading time](#reading-time) (`reading_time` in `config.ini`: adaptive, the default, or Rubi 0.9.7's
15s). A game change, an invalid timestamp or a clock going backwards starts a new interval.
The last line before one has no measurable reading duration; time is added when the next recorded line
arrives. Characters/hour uses only the earlier lines of those timed intervals divided by their total
duration; untimed lines still count in total characters, but cannot inflate reading speed. With no timed
interval, speed is `—` and time is `0 min`. Hook and OCR records follow the same rule.

For an external report with the same numbers, run `python3 tools/reading_stats.py history.csv`
(with `reading_time = 15s`: `--break-minutes 0.25`).

The tool sorts by timestamp; the overlay follows append order so that backward clock changes break
an interval. The tool also accepts some externally edited CSV formats; the overlay reads Rubi's native
format (quoted fields and six fractional timestamp digits). Zero-duration intervals contribute no timed
characters in the overlay. The existing power-loss recovery rule for character counts remains: a cut
record with its line and time's first digit may count once, while a record glued to it without a newline
is skipped. Invalid or incomplete timestamps never contribute time.

**Counting and persistence.** A Triangle press does no file I/O. The control thread counts an unknown
history in existing 24 KiB scratch, one window per wakeup, after other control work. The view says
`Reading statistics: counting…` until ready, or `unavailable` on a failed read (the next press retries).
While the totals are known, successful appends update them in constant space. The scan and writer use
the same timing rule, including the interval across scan windows or a saved prefix. No new threads or
heap allocations are used. While a count runs, it shares storage with game reads and word lookups ahead
are deferred; console latency and CPU3 usage are unmeasured.

`ux0:data/VitaJPOverlay/reading-<platform>-<title_id>.bin` is a **112-byte cache**
(C `sizeof`, not a console measurement) for one game's identity, calendar bounds, character/time
counters and covered CSV prefix, protected by a checksum. The control thread checkpoints through
the existing temporary-file/rename path after counting, on game exit, before changing context,
and at most once per 60 seconds of newly recorded timestamps. A dialogue updates RAM counters;
it adds no cache write. Speed is formatted only when the menu is rendered. No new allocation,
thread or file-size-dependent RAM is added.

A missing, damaged, obsolete or different-day cache triggers a bounded recount; otherwise only
new CSV bytes are scanned. So does one counted with the other `reading_time` rule: the cache's checksum
starts from the rule, 15s's start being 0.9.7's, so a 0.9.7 cache stays valid with `reading_time = 15s`
and is counted again, once per game, with `adaptive` (`last_chars` was 32 bits in 0.9.7; the pace takes
its high half, always zero there, so the 112-byte layout is unchanged). Switching games restores that game's cache. `history.csv` is never
rewritten. The old all-game `history_total.bin` is ignored by the menu (offline test tools can
still use unfiltered totals). Detected history replacement/truncation resets the cached counters.
Histories of 4 GiB or more remain unsupported by on-device totals. Console latency, CPU3 usage
and the appearance on a physical Vita remain unmeasured until a console run.

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
