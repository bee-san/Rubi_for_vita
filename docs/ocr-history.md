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
  of other scripts (é Я 한), every number that is not a digit 0-9 (① Ⅳ ² ㊀,
  Han ones too), every combining mark (so ❤️, a
  symbol and a variation selector, is 1), the control and format characters
  inside a line (its line breaks, `\t`, a CRLF's `\r`, a zero-width joiner)
  and the private-use and unassigned code points.

On the 50 real VOCR textboxes of `host/fixtures/word_lookup` (138 lines), once
the writer normalises them: Rubi's count is 1,329, ttsu's 1,329,
GameSentenceMiner's 1,417 (the 88 line breaks inside them) and exSTATic's 1,344
(11 !, 3 ~ and one ・) [measured: each tracker's own pattern on the writer's
`line`]. GameSentenceMiner's import reads only the first five columns, so its
own totals for imported lines count `line` its way, not from this column.

## Reading statistics

`tools/reading_stats.py` (Python 3, nothing to install; in the release zip's
`tools/` folder and in the repository) reads one or more copies of
`history.csv` and prints the records, characters, reading time and characters
per hour, in all, per game and per day. It prints no dialogue text.

```sh
python3 tools/reading_stats.py history.csv      # days in this computer's time zone
python3 tools/reading_stats.py old/history.csv history.csv --break-minutes 10 --utc
```

The reading time is the time from one record to the next, credited to the
first. A gap of more than `--break-minutes` (5 by default) or a change of game
is a break: it is not reading time, and the record before it gets none, since
how long it was read is unknown. Characters per hour counts only the records
that got a time; the totals count them all. A record in more than one copy
counts once: by `uuid` and `given_identifier`, as GameSentenceMiner's import
does, and another tracker's record (whose `given_identifier` can name just its
game, as exSTATic's does) by those, its time and its line. A record cut short
by a power loss is skipped and counted, also when more records were appended
after it; blank lines and the header lines of files joined together are
ignored. A file that is not the writer's own bytes (Rubi's re-saved by a
spreadsheet, or another tracker's ExStatic lines CSV) is read as plain CSV: its
`characters` cells are used where it has them, and the tool counts the other
lines. A record whose time is not Unix seconds between 1970 and 3000 (another
tracker's milliseconds, say) is skipped and counted, as GameSentenceMiner's
import refuses it. A file that is not CSV, or has none of the five ExStatic
columns, makes the tool stop with exit status 2 and say why. 100,000 records
(a 42 MB file) take about 2 s and 60 MB [measured on the host].

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

A power loss in a new file's very first append can cut its header short. The
writer writes the header only into an empty file, so the next record then
follows the cut on the first line. GameSentenceMiner's import then refuses the
file, when the cut left less than `uuid,given_identifier,name,line,time,` (its
first 37 bytes), or leaves out the record on the first line [checked: its
import's header check and `DictReader` at `7a46a00`, every cut of the header];
`tools/reading_stats.py` reads every record with Rubi's columns and says so. To
import it whole, replace what comes before the first record (`"vita-` or
`"psp-`) with this header line and a line break:

```
uuid,given_identifier,name,line,time,platform,title_id,engine,model,raw_line,scene,region_seq,monotonic_us,elapsed_us,job_seq,region_x,region_y,width,height,fb_width,fb_height,fb_pitch,fb_format,lines,recognized,frames,find_us,load_us,recognize_us,capture_us,captures,tears,region_sum,source,capture_memory,capture_bytes,strip_rows,workspace_bytes,resident_bytes,fallback,line_metrics,characters
```
