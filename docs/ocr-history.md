# Offline OCR history (internal)

Automatic OCR silently appends to `ux0:data/VitaJPOverlay/history.csv`. There is
no UI, setting, network connection or export job. Copy the file with VitaShell.
Only successful, current-game OCR in `ocr_mode = auto` is eligible: ordinary
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

The last column, `characters`, is how many characters `line` has as a reading
tracker counts them: its kanji, kana and other Japanese letters and its ASCII
letters and digits ([Characters](#characters)). Its sum is the characters read;
divided by the reading time it is the characters per hour, which
`tools/reading_stats.py` works out ([Reading statistics](#reading-statistics)).

VjoNet publishes `job_done` before appending. The UI/control thread can consume
the result while the existing worker streams it to disk; that worker cannot
replace the existing transport result until the append returns. Only game/job
facts are copied on its existing stack. The writer has a fixed 192-byte output
buffer and a 16-byte static fingerprint (source sizes, not a console memory
measurement); no allocation, thread, growing queue, second OCR-text buffer or
history read is added. A seek obtains the append offset even for a large file.
The character count is one more pass over the text, in the writer and on its
frame: 76 B more of `vjo_history_append`'s code, paid for by moving the writer's
own strings (93 B: its path and formats) into the data segment with its header,
whose new name adds 11 B. The Shell's code segment is 8 B smaller in the normal
build and 16 B in the debug one, its data 104 B larger in both; the pages and the
writer's stack frame (420 B) stay as they were [measured: the build log's module
rows, `arm-vita-eabi-nm -S` and the writer's disassembly, both builds of main
`44d4e8d` and this change]. The count runs 107 Cortex-A9 instructions a code
point, 11,070 for the mean three-line textbox (104 code points) [measured: the
Vita compiler's code under `qemu-arm -cpu cortex-a9` with vita-vn-ocr's
instruction-count plugin, over the 50 real VOCR lines of
`host/fixtures/word_lookup`], about 25 µs at 444 MHz [estimate: one instruction
a cycle], after the result is published and before the record's file writes.
Both module-page budgets and thread-stack budgets remain enforced by the ledger.

Consecutive identical normalized text within a game is suppressed, including
rescans caused by a blinking icon or reopening/resuming. A different sentence,
game, or successful blank scan permits the same text again. Fingerprints reset
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

Rubi's count is ttsu's but for rare characters: ttsu also counts ○ ◯ (used to
hide letters of a name) and the CJK radicals, and does not count ゟ ヽ ヾ ヿ, the
half-width ﾞ ﾟ, the small katakana ㇰ-ㇿ, the combining dakuten or most
compatibility ideographs, which Rubi does. GameSentenceMiner also counts the
letters of other scripts (Greek, Cyrillic, accented Latin), other numbers (①)
and each line break inside a line (`\n` is a control character, not in `\p{Z}`).
Its import reads only the first five columns, so its own totals for imported
lines count `line` its way, not from this column. exSTATic counts 𠮟 as 2 and
keeps the marks its list leaves out (・ ! ~ 〜 ○).

## Reading statistics

`tools/reading_stats.py` (Python 3, nothing to install) reads one or more
copies of `history.csv` and prints the records, characters, reading time and
characters per hour, in all, per game and per day. It prints no dialogue text.

```sh
python3 tools/reading_stats.py history.csv      # days in this computer's time zone
python3 tools/reading_stats.py old/history.csv history.csv --break-minutes 10 --utc
```

The reading time is the time from one record to the next, credited to the
first. A gap of more than `--break-minutes` (5 by default) or a change of game
is a break: it is not reading time, and the record before it gets none, since
how long it was read is unknown. Characters per hour counts only the records
that got a time; the totals count them all. A record in more than one copy
counts once (by `uuid` and `given_identifier`, as GameSentenceMiner's import
does), and a record cut short by a power loss is skipped and counted.

Other trackers time reading differently, so the same reading gives them
somewhat different hours: GameSentenceMiner credits a line with at most 2.5
times what its session's median pace would take for it (never over 5 minutes)
and starts a new session after a 30-minute gap (`session_gap_seconds`);
exSTATic's visual-novel tracker stops its clock 60 s after the last line
(`afk_max_time`).

## The format and older files

The file has no version number: its first line names its columns. Rubi 0.8.0
wrote the 41 columns up to `line_metrics`; `characters` is the 42nd, and every
other column keeps its position. A file is never rewritten, so a `history.csv`
that 0.8.0 began keeps its 41-name header, and the rows appended to it since
have one cell more than the header names. Readers that go by the header's names
ignore that cell, as GameSentenceMiner's import (Python's `csv.DictReader`)
does; `tools/reading_stats.py` reads it as the count and counts `line` itself
for the older rows. A reader that wants as many cells in every row as the
header has names stops at the first longer row (pandas' `read_csv` does, by
default): add `,characters` to the end of the file's first line (on Linux,
`sed -i '1s/$/,characters/' history.csv`), or move the file away on the Vita so
that Rubi starts a new one with the new header.
