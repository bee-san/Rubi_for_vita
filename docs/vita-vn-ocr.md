# vita-vn-ocr: local OCR on the Vita CPU

`ocr_backend = vocr` reads the selected dialogue region on the console with
[vita-vn-ocr](https://github.com/bee-san/vita-vn-ocr): a small Japanese CTC line
recognizer made for visual-novel dialogue, and its line finder, both plain C99
with no `malloc`. Nothing leaves the Vita: no image upload, no Lens connection
and no cloud fallback. With `dictionary = local` the whole lookup is offline.

**Status: default in v0.7.5; physical recognition validation is still pending.** It builds with VitaSDK and
passes the host tests; everything below the "Host measurements" heading was
measured on a PC. On-console memory, speed and stability are what the
[checklist](#on-console-checklist) is for.

## Experimental VOCR2 trial

Select **VOCR2** on the Overlay app's OCR & lookup page, or set
`ocr_backend = vocr2` in `config.ini`. It uses the same selected dialogue crop,
automatic/manual modes and game worker as VOCR. Select **VOCR** to return to the
original backend. The saved `vocr_model` setting applies only to VOCR.

VOCR2 pairs the native textbox finder with a separately trained
`VOCR2_FL10_w8.vocr` recognizer. The model remains 657,664 bytes and its
recognition arena remains 124,832 bytes. Workspace sizing and one-pass capture
ownership are unchanged. The extra finder code and bounded stack use still
occupy memory; unchanged workspace does not mean unchanged total plugin memory.
Console speed, memory and compatibility require physical testing.

**This is an opt-in experiment, rejected for promotion.** Shared screenshot
aggregate errors improved on both platforms and with 4/8-pixel crop margins,
but eleven title/margin groups had small regressions and the predeclared strict
per-title guard failed. It is not the default or a qualified replacement for
VOCR. The [measured results and exact candidate pins](benchmarks/vocr2-candidate-20261012.json)
are also published with the [OCR research release](https://github.com/bee-san/vita-vn-ocr/releases/tag/v0.2.3-vocr2.1).

The trial bundle includes the separate model. For a source build:

```sh
python3 tools/prepare_ocr.py --backend vocr2 --output ocr
```

An authenticated GitHub CLI is needed while the OCR repository is private;
`--from FILE` or `--from DIRECTORY` verifies a previously downloaded candidate.
Copy `VOCR2_FL10_w8.vocr` to `ux0:data/VitaJPOverlay/ocr/`. Its SHA-256 is
`d3adc52e5cf62b548af04c5ab07a2f57cf0419386bbdf49ba19c9703be6e1766`.
Renaming old FL10 weights does not enable VOCR2: the worker rejects that hash.
A missing or corrupt candidate reports an error without changing engines.

## What a recognition does

1. The game reads its own screen ([in-game capture](ingame-capture.md)): the
   display hook writes the selected region's rows, already grey
   (`(77 R + 150 G + 29 B) >> 8`, the luma both the finder and the recognizer
   use), a strip at a time into the game worker's own capture block. No copy
   of the region exists; the kernel gives OCR no buffer.
2. The polarity-agnostic line finder (`vocr_find_lines`, gradient/contrast
   projection) finds up to 16 lines. Unlike the PP-OCRv5 backend's bright-pixel
   finder it reads dark text on light boxes, outlined text and coloured names,
   and never rejects a region. A region with no line ends here: the weights
   file is not read.
3. Nothing is recognized before every row of the region has read the same for
   `ocr_settle_ms` (150 ms): text that types out is read once it is whole
   ([stable text](#stable-text-and-blank-lines-api-20)).
4. With `ocr_skip_blank_lines = on` (off by default), a found line whose crop
   holds no text-like ink (an even area, a smooth part of a picture, the box's
   gradient) is skipped.
5. The existing kernel worker opens the pinned model from
   `ux0:data/VitaJPOverlay/ocr` and reports its length and modification
   time. The first job, and any job after the file changed, copies it in
   32 KiB reads into its own weights block and checks size and SHA-256
   against the pinned release asset. Later jobs of the same game reuse those
   verified weights without reading the card again. The runtime validates the
   model either way. A region whose lines were all skipped opens no file.
6. Each line is recognized (int8 weights, int32 sums, NEON); lines of dark
   text are inverted first, as the model was trained.
7. The job workspace is freed before the dictionary lookup and before anything
   is shown; the verified weights stay for the next job (see below). A failed
   or cancelled job shows no partial text.

A job's memory is three blocks in the game: the weights, kept between jobs;
the workspace; and the capture block (one strip of the region's grey rows,
and per row its checksum, its recent states and two bits; within 64 KiB at any
region). The last two
are freed after the job. Since API 19 the workspace holds no copy of the
region, so a job hardly depends on the region's size:

```
weights (kept)                                  page(model bytes)
max(line finder arena, recognizer arena)
+ the runtime's two structs (3,760 bytes)       page(...)          (per job)
capture block: one strip; per region row 4 bytes, 2 bytes per earlier state
  (16, or as many as fit: 6 at 960x544) and 2 bits              <= 64 KiB (per job)
```

A line taller than a 64-row strip is kept whole in one more block of its own
size (at most 516,096 B), only while that job runs. The table's totals are the
three page-rounded blocks together, the peak of a job (`vjo_vocr_job_bytes`,
`vjo_vocr_capture_bytes`; the allocation simulator's "game USER peak" lines
are the same figures); between jobs only the weights remain.

| model (`vocr_model`) | weights | recognizer arena | full 960×544 region | 880×144 dialogue box |
|---|---|---|---|---|
| `VOCR3_H15_w8.vocr` (default), `H15_w8.vocr` (v0.2.0) | 1,155,968 B | 182,944 B | 1,409,024 B (1.34 MiB) | 1,404,928 B (1.34 MiB) |
| `VOCR3_FL10_w8.vocr` (automatic fallback), `FL10_w8.vocr` (v0.2.0) | 657,664 B | 124,832 B | 851,968 B (0.81 MiB) | 847,872 B (0.81 MiB) |
| `F20_w8.vocr` (larger) | 1,558,016 B | 241,056 B | 1,871,872 B (1.79 MiB) | 1,867,776 B (1.78 MiB) |

The line finder's arena (83,323 B at 960×544) is smaller than every
recognizer arena, so it costs nothing extra.

**Default and fallback.** `vocr_model` (default `VOCR3_H15_w8.vocr`) is the
model a job uses when the game has room for it. The game worker checks free
`USER` memory before each job; if the reserve, the model's workspace and
(unless already resident) its weights do not fit but the smaller
`vocr_fallback_model` (default `VOCR3_FL10_w8.vocr`) does, that job runs the
fallback. Resident fallback weights stay while the default still does not fit,
and are released for it once it does. A missing, wrong or corrupt `vocr_model`
file also makes the game's later jobs use the fallback (until the game
restarts). The kernel opens only the two pinned names the Shell submitted; the
log names the model that ran and, after a memory fallback, what the default
would have needed. Every pinned model is inside
the console budget ceiling (weights ≤ 1,600,000 B, recognizer arena ≤
262,144 B; bee-san/vita-vn-ocr `docs/vita-memory-budget.md`). Since v0.7.1 inference runs in
`VitaJPOverlay_OCR.suprx`, now registered per game in its own `*TITLEID` section
(the installer's `--title`; releases up to v0.7.12 used `*ALL`). The `*main` plugin handles
UI/dictionary results and has no vocr inference code. The game worker checks
its process title and only accepts jobs for the foreground game after the
Shell has confirmed its per-game policy.

The workspace is one private `USER_RW` memblock in that game's process.
The worker requires enough reported free USER memory for the page-rounded
workspace plus a 4 MiB game reserve; there is no Paf fallback for OCR.
It does not borrow the game's heap. Spare bytes inside a game-owned memblock
remain owned by the game and are not available through this allocator. Free
USER memory, the process budget and available physical pages all affect fit.
A 333 MiB game allocation mode is not enabled or required by this release.

Rows travel through a role-restricted kernel syscall tied to the job's game,
capture sequence and foreground generation. A cancelled/suspended worker
cannot read a newer capture or publish old results. Model reads finish short
reads in at most 32 KiB calls and check cancellation between calls. Each call
waits up to 50 ms in the kernel for the worker thread's result; only a slower
read returns BUSY, which the game repeats after a 1 ms yield. The kernel
allows only FL10, H15, F20 or the separate VOCR2 trial model under the exact directory above, with their pinned
file lengths; it does not accept a caller-supplied file path. Each async Open,
Read or Close is tied to the claimed job's PID and sequence. Reads also require
the current foreground epoch, and completion copies use the current syscall's
destination pointer. No pointer is retained across a file operation. Close
remains available after cancellation or Shell restart, and process exit makes
the kernel close an orphaned descriptor itself. A timeout or failed Close
retains ownership until cleanup succeeds; a new job cannot replace it.
The existing kernel worker performs file I/O outside the shared lock, with one
static 32 KiB buffer and no additional thread or full-model allocation. The log
records the game worker's OCR timings, workspace, whether the weights were
read or already resident, the resident bytes, I/O and stat errors and cleanup
status. The workspace is freed before results return to the Shell. The
verified weights are released when a job's memory check fails, when the file is
missing, changed in size or corrupt, when another model is selected, when the
game stops being the active one (suspend, another app, title disabled) and when
the game exits. If the kernel cannot read the file's modification time, every
job reads and checks the weights again, as before. Local
lookup uses one 64 KiB result arena, with no Lens connection pool. The prior
view/cache is retired before a local job begins, so a second arena is unnecessary;
a refresh of the open overlay (docs/touch-lookup.md) keeps the view and the arena
and leaves its text in the worker's result until the control thread compares it;
network jobs retain their two arenas. Local OCR-only/subtitle jobs use a static
4 KiB filtered-text buffer and allocate no dictionary arena. The UI owns a
separate 4 KiB copy for character navigation. Left/right moves one Unicode
character and requests zero or one best entry at that position; it does not
recapture or recognize text. Region and foreground changes invalidate
saved text; a scene change never does while the overlay shows it (a refresh
replaces it quietly when it reads another), while rapid cursor moves supersede earlier lookup results. If Shell refuses
lookup memory after successful OCR, this buffer preserves the recognized
sentence/header; the error names Shell UI memory and applies dictionary backoff
without marking recognition failed. The 1.5 MiB Shell UI reserve is unchanged.

Physical logs from the previous direct game-file implementation show
`rc=-115`, one found line and `io=0x80010013` (ENODEV): the game did not expose
the installed `ux0:` model file. The kernel broker addresses that process
namespace boundary. The host integration reproduces the unavailable game
namespace and exercises both pinned models through the actual broker and
worker, with injected short reads, corruption, cancellation and failed Close.
These tests and bounds do not establish real Vita memory availability, speed
or stability. A physical in-game recognition remains required.

## Stable text and blank lines (API 20)

Two checks in the game worker (`core/vocr_ocr.h` `VjoVocrPolicy`), set in
`config.ini` and sent with every job in the request's flags:

```ini
ocr_settle_ms = 150         ; 0-1000, 0 = off; sent in 10 ms steps (1-14 ms: 10 ms)
ocr_settle_max_ms = 2000    ; 100-10000, in 50 ms steps; above ocr_settle_ms
ocr_skip_blank_lines = off  ; on | off
```

**Stable text.** Visual novels type their text out a character at a time. A
job reads the region a strip per game frame, and before API 20 it recognized
whatever its first two passes saw, as long as they agreed: on a line that
types slowly, or pauses longer than those passes take, that can be half a
line. Now nothing is recognized until every row of the region has read the
same, at full resolution (the rows' FNV-1a checksums the job already keeps),
on passes that span at least `ocr_settle_ms`. A job the Shell asks for when
the kernel has already seen the region settle reads its first pass and finds
its lines at once, then re-reads the region until the span is long enough. A
job asked while the region is still changing (a "quiet" screen, the overlay
opened over typing text, the subtitles' 2 s refresh) waits first, re-reading
the region without finding lines, and takes its first pass, the capture the
Shell is told of, only once the rows hold still. When the screen changes
under a job, it voids the capture it published (the Shell then treats the job
as not captured yet), waits, and publishes the retry's (before API 20 the Shell
kept the first capture's screen for a retried job's text). A row that goes
back to one of its recent states (up to 16 earlier ones; 6 at a full 960×544
frame, as many as fit the 64 KiB block) is not a change: a "next" icon that
blinks or bobs does not hold the wait up, nor tear the job while it reads, in
up to 64 rows of a job. A page of new text, or a box of several lines that
clears back to what the job saw before, changes more rows than that; a one-
or two-line box that does so while the job reads it can be taken for an icon
(that job's text then misses those lines; it is tagged with the screen it
read, which is no longer shown). Once a re-read has matched, the job sleeps
until a re-read can be the last one rather than reading passes too early to
count, so the display hook copies fewer strips. A region that never holds
still (a video behind the text) stops waiting after `ocr_settle_max_ms` for
the whole job and is read as before; a value under `ocr_settle_ms` ends every
wait that way. Whatever the clock says, a job's waits read at most 4,096 strips
(a real job reads a few hundred at most). The wait costs frames, not CPU: one
FNV-1a pass over each row read. Without the wait (`ocr_settle_ms = 0`) a job
reads, compares and retries exactly as API 19 did.

Not learning a row as an icon's once the job has stopped waiting would close
the one- and two-line limit above, and it was tried (audit-overlay-dialogue-1,
N3): then a "next" icon that blinks slower than the wait, and so changed only
once while the job waited, makes the job read again when it goes back, in 4
of `vjo-settle-bench`'s 88 rows (+11 to +25 frames: the blinking icon at 30
fps and at 960×544). That icon is on most screens a player reads, while the
limit needs the player to advance within the job's first reads after the text
held still, so it stays as it is.

**Blank lines.** The line finder also reports boxes with no text in them.
With `ocr_skip_blank_lines = on` a found line is skipped, not recognized, when
its crop has fewer than 50 edges per 10,000 pixels: pixels from which the grey
level changes, within two pixels to the right or below, by at least a quarter
of the crop's contrast (98th minus 2nd percentile), 24 grey levels at the
least. Two pixels and a quarter, not one and a half: text whose edges are soft
is strokes too, and a PSP game reaches the display hook as its 480×272 frame
shown 2x through the emulator's bilinear filter, which turns each one-pixel
step into two half steps (audit-overlay-dialogue-1, B2). An even area, a
smooth part of a picture or the box's gradient has no such edges. If every
line of a region is skipped, the weights are not opened. The check reads each
crop twice (a histogram, then the edges). It is off by default: once it keeps
soft text it skips no line of the development set (below), and which frames a
console game's hook reads, and how soft, is still unmeasured (#81's
`--collect` logs carry each skipped line's contrast and edges).

**Logs.** Each job adds `gstep job settle` (rc 1: the rows held still, 2: the
limit ran out first; arg: passes read waiting; bytes: row changes seen, i.e.
text typing out; the high half of sum: rows that toggled; us: the wait) and a
`gstep job blank` line for each of the first four skipped lines (its index,
size, contrast and edges; rc: the job's total). `tools/analyse_selftest.py
--collect` sums them up: how often the rows held still or the limit ran out,
the wait's range, whether an icon toggled, and the lines skipped. A retried
job logs its first pass and its signature once (`pass`, `sig`; `tear` counts
the tries), and when its 20 steps run out the other steps make room for the
`free` of every block the worker holds, so the log always shows the job's
memory given back.

**Measured on the console** (#81's `--collect` of v0.8.0-test.1, API 19, so
without the wait; CLANNAD, an 883×112 region, two strips a pass): 4 of the 16
game jobs ended with the screen changed under all three tries (38 to 192
changed rows counted over the three; a try stops at the first strip that
changed, so at most 64 rows count in one), and a fifth read its text only on
its third try (26 changed rows counted before it). CLANNAD types its text out, the case the wait is for. A still
screen's job read its 4-5 strips in 43-106 ms, about a frame a strip, as the
host bench below assumes.

**Measured on the host** (`vjo-settle-bench`, `docs/benchmarks/settle-wait-20261014.json`): a
simulated screen where every strip read is one game frame, at 60 and 30 frames
a second; no weights, so the numbers are the capture and the wait. Beyond the
one console sample above, frame pacing and strip time are unmeasured, so the
ms are estimates.

| region (strips per pass), fps | screen | without the wait | with 150 ms |
|---|---|---|---|
| 917×125 (3), 60 | still, asked settled | 7 frames, 116 ms | 12 frames (10 strips read), 199 ms |
| 917×125 (3), 60 | still, asked while it changed | 7 frames, 116 ms | 22 frames, 366 ms |
| 960×544 (10), 60 | still, asked settled | 21 frames, 349 ms | 21 frames, 349 ms (the finder's second pass is the span) |
| 917×125 (3), 60 | 12 of 25 characters shown, typing resumes 8 frames (133 ms) after the job is asked | **reads 12 of 25** | the change is seen; the retry reads **25 of 25** (65 frames, 1,083 ms) |
| 917×125 (3), 30 | the same, 8 frames (267 ms) | reads 12 of 25 | reads 25 of 25 (60 frames, 1,999 ms) |
| 960×544 (10), 60 | the same, 8 frames | the screen changed under all 3 tries: no text | reads 25 of 25 |
| 917×125 (3), 60 | the same, 24 frames (400 ms) | reads 12 of 25 | reads 12 of 25: the pause outlasts the job's reads |
| 917×125 (3), 60 | asked while 25 characters type, one per 3 frames | 3 tries, the screen changed: no text | first pass at frame 91 (typed by frame 75): 25 of 25 |
| 917×125 (3), 30 | the same typing (2.5 s) | no text | the 2 s limit runs out: read without it, the screen changed: no text (as before) |
| 917×125 (3), 60 | blinking icon (8 frames) beside a still line | - | 31 frames, 516 ms; its rows counted as cycling, no timeout |

A pause in the typing longer than `ocr_settle_ms` plus the job's own reads
(the 400 ms case) is still read as a part line, as before; raise
`ocr_settle_ms` for a game that pauses longer mid-line.

**Measured on development data** (`tools/blank_lines_eval.py --rules`,
`docs/benchmarks/blank-lines-20261014.json`): the linefind `vita_dev` and
`psp_dev` regions (317 regions of 84 VNs; 16 reserved VNs skipped), read by
the finder and the recognizer exactly as the worker reads them, as captured
and in seven softer display paths of the same screenshots (`--how`). A found
line is text when it overlaps a hand-marked line box, else empty. Each line is
also recounted in numpy under the rule this check first had (one-pixel steps
of half the contrast), and under the current one, whose count equals the C's
on every line.

| display path (VOCR3-FL10) | text lines skipped, current rule | lowest text line | empty lines skipped | the first rule: text lines skipped |
|---|---|---|---|---|
| as captured (x1; H15 the same) | **0 of 643** | 1,553 | 0 of 105 | 0 of 643 (13 of 105 empty) |
| PSP 2x, unfiltered | **0 of 267** | 1,289 | 0 of 44 | 0 of 267 |
| PSP 2x, bilinear | **0 of 266** | 350 | 0 of 35 | **163 of 266** (1,631 characters) |
| PSP 2x, bilinear, 1-pixel box blur | **0 of 263** | 190 | 1 of 43 | **263 of 263** |
| Vita, rendered 720×408, upscaled bilinear | **0 of 380** | 1,011 | 0 of 68 | **104 of 380** |
| Vita, rendered 640×368, upscaled bilinear | **0 of 384** | 866 | 0 of 77 | **178 of 384** |
| Gaussian blur σ 1.0 | **0 of 632** | 270 | 0 of 102 | **585 of 632** |
| Gaussian blur σ 1.5 | **0 of 583** | 209 | 0 of 134 | **583 of 583** |

The check costs 30-50 ms on the host for all lines of a path (753 lines as
captured). So it now keeps every text line in every path, and in exchange it
skips no line of the development set as captured: the boxes without text that
the finder reports are textured (parts of a portrait, scene art, frame lines;
one frame line alone made 48 of H15's 129 characters there, "ニニニ…"), which no
image statistic tried here separates from text without losing dialogue
(sharp-edge direction, mid-tones and ink runs each lost text lines first).
What is left for it is an even area or a smooth gradient. Telling the rest
apart needs the recognizer's own confidence, or a finder that cuts frame
lines.

## Text normalisation (shown and looked up)

The recognizer's vocabulary holds both widths of the same marks (`!` and `！`, `~` and `～`, `.`
and `．`), several dashes, and both the long-vowel mark ー and the kanji 一. What the overlay shows,
saves to Anki and looks up is normalised to one form of each (`core/textnorm.h`), the form
vita-vn-ocr's scorer (`eval/metrics.py` `norm()`) reads as the same text, so a sentence from the
console compares with the evaluation directly:

- the full-width ASCII forms U+FF01–U+FF5E become ASCII, as NFKC does (`！` → `!`, `１` → `1`);
- `〜` and `∼` become `~`; the dashes `— – ‐ ‑ ‒ − ─ ━` become `―` (the ASCII hyphen stays);
- `...` becomes `…`, and a run of two or more `・` becomes `…` for every three (a lone `・` in a name
  stays);
- by context: `一` becomes `ー` after a katakana when katakana, a small kana, a mark or the end of the
  line follows (`コ一ヒ一` → `コーヒー`, while `コーヒー一杯` stays), and after a hiragana when a small
  kana, `ー`, `~`, `!`, `?`, an ellipsis, `」` or `』` follows (`だよ一っ` → `だよーっ`); `ー` between
  two kanji becomes `一` (`玄ー郎` → `玄一郎`). The counters `ヶ` and `ヵ` do not count as small kana
  (`あと一ヶ月` and `ハネムーン一ヶ月` stay). After a hiragana, `一` at the end of a line also stays,
  because a text box wraps mid-word and `もう一` / `度` is far more common there than `すごー`.

The text never grows, so the game worker normalises it in place before it returns it, and keeps
the recognizer's own text after it in the same 4 KiB buffer (`vjo_text_normalize_keep`; nothing
is added to SceShell). The Shell writes the recognizer's text to `log.txt`, one line per
recognized line (`game vocr text N: …`, 60 characters at most), and, for a line that changed,
what is shown (`game vocr text N shown: …`). The job's `gstep job norm` step counts the changes
(`bytes`: full-width forms; `sum`: tildes << 24 | dashes << 16 | ellipses << 8 | 一→ー << 4 | ー→一).

**Measured on development data** (`tools/textnorm_eval.py`, `docs/benchmarks/textnorm-20261014.json`):
teacher_val and devnative v3 (2,629 lines, 28,544 reference characters), the console-polarity
readings of the C runtime (W8I). "Scored" is CER with vita-vn-ocr's scorer; "exact" compares the
text itself with the references, which are written in the scorer's form.

| model | scored CER | exact CER (raw readings) | ー/一 changes right / wrong |
|---|---|---|---|
| VOCR3-H15 (default) | 2.116 → 2.113 % | **3.906 → 2.113 %** (513 full-width forms) | 1 / 0 |
| VOCR3-FL10 (fallback) | 2.291 → 2.281 % | **4.043 → 2.281 %** (504 full-width forms) | 3 / 0 |
| VOCR3-W20 | 1.815 → 1.815 % | – | 0 / 0 |
| VOCR4-FL10 | 2.459 → 2.452 % | – | 2 / 0 |
| VOCR4-H15 | 2.113 → 2.109 % | – | 1 / 0 |
| VOCR4-W20 | 1.703 → 1.703 % | – | 0 / 0 (its one change is on a line misread around it) |

The scorer already folds the forms, so only the ー/一 rules move scored CER, and only a little:
these recognizers rarely confuse the two (one `一` after a katakana in all six models' 15,774 readings).

**On correct text**, every change the ー/一 rules make is a wrong one (`--corpus` in the same tool
and benchmark file). The text is vita-vn-ocr's training corpus, `shared/data/corpus`: 1.6 million
lines of dialogue, narration and Wikipedia text, none of it from a test set. The rules change:

- 0.25–0.31 characters per 100,000 on whole lines;
- 0.33–0.44 per 100,000 when the lines are wrapped every 20 or 26 characters, as a text box wraps
  them.

Most of these changes come after a katakana: the numeral before a katakana counter or unit (`ビール一ケース`),
or a word cut by the wrap right before the numeral. In Wikipedia text, `ー` written between kanji
accounts for most of the rest. What the
console shows moves a lot: against the references the raw text differs in 1,115 characters
(H15), the normalised text in 603, exactly the scored errors. Lookups see the ー/一 changes only
(punctuation is never looked up).

## Models

**The default is VOCR3-H15 (`VOCR3_H15_w8.vocr`), with VOCR3-FL10 (`VOCR3_FL10_w8.vocr`) as
its automatic fallback.** They are vita-vn-ocr #67's recognizers, published as the
[v0.2.5-vocr3.1 pre-release](https://github.com/bee-san/vita-vn-ocr/releases/tag/v0.2.5-vocr3.1):
H15's and FL10's shapes retrained, so the same sizes, arenas and line finder, and a job needs
exactly the memory it needed with v0.2.0's H15 and FL10 (`host/tests/test_vocr.c` checks the
sizes, arenas and job bytes are equal; the Vita build's memory ledger is unchanged).

| file | bytes | sha256 | W8I arena | release |
|---|---|---|---|---|
| `VOCR3_H15_w8.vocr` (default) | 1,155,968 | `cb3c61a8f91f9aeeb3efcc0e79313f664112f25c4bd55865bae9df52522b360c` | 182,944 | v0.2.5-vocr3.1 |
| `VOCR3_FL10_w8.vocr` (fallback) | 657,664 | `87fb4d0c18416f5f3771cecd274c8db3cf69b8b4a4ee9f204edebb4b82079154` | 124,832 | v0.2.5-vocr3.1 |
| `H15_w8.vocr` | 1,155,968 | `316ebc377ec34520440991c5085199d8e74031f01a867938be90a60c1e3f03cd` | 182,944 | v0.2.0 |
| `FL10_w8.vocr` | 657,664 | `500e0b842e5f0cb326f55e7d991a52ab66dd24977ccc89bd188228aedc825e84` | 124,832 | v0.2.0 |

vita-vn-ocr's locked report (host, plain-C W8I runtime, `docs/vocr3.md` there) gives pooled
vita-test + psp-test fixed-line CER 1.73% for VOCR3-H15 against 2.21% for v0.2.0 H15, and 2.13%
for VOCR3-FL10 against 3.82% for v0.2.0 FL10. None of the three VOCR3 models passed that
repository's promotion gate (each regresses some titles on its whole-dialogue guard; on whole
dialogue VOCR3-H15 is slightly behind v0.2.0 H15, +0.30 points, VN-cluster CI −0.49 to +1.06),
and no console has run them: they are the default of the test builds by the owner's choice
(2026-10-14), and the v0.2.0 models stay in the package as the documented fallback. To go back
to them, set in `config.ini`:

```ini
vocr_model = H15_w8.vocr
vocr_fallback_model = FL10_w8.vocr
```

`vocr_model = VOCR3_FL10_w8.vocr` always runs the smaller model; `vocr_fallback_model = none`
never falls back. `install-vita.sh` sets the package's defaults each time it runs, so set the choice
again after a reinstall. The game worker and the kernel's model broker accept exactly these names,
F20 and VOCR2's model, each at its pinned size and SHA-256.

The v0.2.0 weights are the int8 assets of the vita-vn-ocr
[v0.2.1 release](https://github.com/bee-san/vita-vn-ocr/releases/tag/v0.2.1)
(unchanged from v0.2.0). On its hand-checked real screenshots (int8, C runtime, its
`eval/run.py`, with hand-drawn line boxes):

| model | vita-test CER / exact lines | psp-test CER / exact lines | est. Cortex-A9 time, 640-px line at 444 MHz |
|---|---|---|---|
| H15 | 1.56% / 83.5% | 2.63% / 82.4% | 166-302 ms |
| FL10 | 2.30% / 77.7% | 5.18% / 69.8% | 94-178 ms |
| F20 | 1.79% / 84.0% | 2.79% / 79.6% | 235-416 ms |
| for scale: JIS-trimmed int8 PP-OCRv5 mobile | 2.30% / 79.1% | 5.34% / 66.1% | |

The times are vita-vn-ocr's estimates from the Vita compiler's machine code
(`vita/a9_cycles.py`), not console measurements. The runtime itself reproduces
the PC bit for bit in Vita3K. Pretraining used NVIDIA
OCR-Synthetic-Multilingual-v1 (CC BY 4.0); see the release notes for the full
attribution. The weights are GPL-3.0-or-later, like this overlay.

## Build

The runtime is not copied into this repository. Check out vita-vn-ocr at the
pinned commit and point the build at it (the repository is private, so this
needs access to it):

```sh
git clone https://github.com/bee-san/vita-vn-ocr /path/to/vita-vn-ocr
git -C /path/to/vita-vn-ocr checkout a5d5aad68bb8e719e44e2e50b5836cf9afeae796

# Vita plugins and release zip (VitaJPOverlay-0.7.5.zip)
cmake -S vita -B build/vita -DVJO_WITH_VOCR=ON -DVJO_VOCR_SOURCE_DIR=/path/to/vita-vn-ocr
cmake --build build/vita --target release

# Host tools and tests; VJO_VOCR_MODEL_DIR adds the real-model checks
cmake -S . -B build/host -G Ninja -DVJO_WITH_VOCR=ON -DVJO_VOCR_SOURCE_DIR=/path/to/vita-vn-ocr \
  -DVJO_VOCR_MODEL_DIR=$PWD/ocr
cmake --build build/host && ctest --test-dir build/host --output-on-failure
```

`cmake/VocrOCR.cmake` checks the four runtime files (`runtime/vocr.c`,
`vocr.h`, `vocr_lines.c`, `vocr_lines.h`) against their SHA-256 at that commit
and refuses others unless `-DVJO_VOCR_ALLOW_UNPINNED=ON`. Copying those files to
`third_party/vita-vn-ocr/runtime/` works too and needs no source directory.
Without `VJO_WITH_VOCR` the Shell is the Lens-only build; `ocr_backend = vocr`
then reports that the backend is missing instead of uploading anything.

## Install

The complete v0.7.5 ZIP already includes both int8 models and the local
configuration; use its `install-vita.sh` as described in the README. The
steps below cover a source build or adding another model.

1. Install the vocr build's plugins as in the [README](../README.md#install).
2. Download and check the weights on a computer:

   ```sh
   python3 tools/prepare_ocr.py --backend vocr --output ocr          # VOCR3_H15_w8.vocr
   python3 tools/prepare_ocr.py --backend vocr --model VOCR3_FL10_w8.vocr --output ocr
   python3 tools/prepare_ocr.py --backend vocr --model H15_w8.vocr --output ocr   # v0.2.0
   ```

   While vita-vn-ocr is private this uses the GitHub CLI (`gh auth login`
   with access to it). `--from FILE` checks a copy downloaded another way.
3. Copy the file to `ux0:data/VitaJPOverlay/ocr/`, or let the FTP installer do
   it: `tools/install_ftp.sh --vocr-model ocr/VOCR3_H15_w8.vocr --set ocr_backend=vocr VITA_IP`.
4. In `ux0:data/VitaJPOverlay/config.ini`:

   ```ini
   ocr_backend = vocr
   ocr_model_dir = ux0:data/VitaJPOverlay/ocr
   vocr_model = VOCR3_H15_w8.vocr
   vocr_fallback_model = VOCR3_FL10_w8.vocr
   ocr_mode = auto
   log_file = on
   ```

   `ocr_mode = auto` also works: each settled dialogue screen is read in the
   background, which costs a model load and recognition per screen.
5. In the game, open the overlay, press □ and draw a box around the dialogue
   text (the whole text box is fine, name plate included), then × to keep it.

## Host measurements

The v0.7.0 rebuild reproduced both int8 models' region CER on 2026-10-11:
[release measurements](benchmarks/vocr-release-20261011.json). FL10 scored
217/2,506 character errors (8.66%) on 84 Vita regions and 443/2,510 (17.65%)
on 93 PSP regions. H15 scored 170/2,506 (6.78%) and 369/2,510 (14.70%). Neither
model refused or failed a region. Historical whole-sentence lookup used
110,976–111,248 peak arena bytes across seven illustrative full-JMdict queries,
five runs each ([commands and results](benchmarks/local-result-memory-20261011.json)).
v0.7.5 instead requests one entry at the selected character. Nine illustrative
positions in the same full JMdict, one warmup plus five measured runs each,
used at most 54,000 arena bytes; unmatched punctuation used 263 bytes and no
file I/O ([commands and results](benchmarks/local-character-memory-20261011.json)).
Generated limit tests also fit an 8,192-byte entry and a 4,095-byte input in the
64 KiB arena: 53,584 peak bytes with static text, or 61,776 with two full text
copies. These bounds include selected output metadata and temporary
page/deinflection workspace; they are neither total-process peaks nor a
worst-case guarantee for every dictionary. Dynamic local result allocation
falls from two 128 KiB arenas to one 64 KiB arena (192 KiB less requested heap).
New static text/UI buffers and code also occupy Shell memory, so that reduction
is not a claim of 192 KiB more total free memory. These remain host measurements.

`tools/benchmark_local_ocr.py` runs the host `vjo-ocr` tool (the same
finder, workspace and model checks as the game worker) once per region, in a fresh
process, on vita-vn-ocr's hand-checked regions: vita-test (84 regions from 16
Vita games, 960×544 screenshots) and psp-test (93 regions from 14 PSP games, at
the PSP's native 480×272). Accuracy is eval's region-level concat CER: all
lines a backend read, top to bottom, against all lines of the region; a refused
region counts all its characters as errors. The ncnn row is this repository's
own PP-OCRv5 mobile backend (`-DVJO_WITH_NCNN=ON`, the pinned 8,242,276-byte
model) with its bright-pixel line finder. Details:
[benchmarks/vocr-vs-ncnn-20261010.json](benchmarks/vocr-vs-ncnn-20261010.json).

| backend | set | regions refused | concat CER | workspace peak, max (median) | process peak RSS, max |
|---|---|---|---|---|---|
| ncnn PP-OCRv5 mobile | vita-test | 25 of 84 | 37.55% | 42.41 MiB (32.38) | 56.3 MiB |
| vocr H15 | vita-test | 0 | **6.78%** | **1.40 MiB (1.33)** | 6.1 MiB |
| vocr FL10 | vita-test | 0 | 8.66% | 0.87 MiB (0.80) | 5.5 MiB |
| ncnn PP-OCRv5 mobile | psp-test | 41 of 93 | 60.72% | 38.36 MiB (31.95) | 45.3 MiB |
| vocr H15 | psp-test | 0 | **14.70%** | **1.32 MiB (1.30)** | 5.7 MiB |
| vocr FL10 | psp-test | 0 | 17.65% | 0.79 MiB (0.77) | 5.1 MiB |

- The workspace peak is what the job allocates: ncnn's tensor heap inside its
  64 MiB block, vocr's whole exact workspace. The RSS is the tool process's
  own VmHWM and includes its 4.3 MiB baseline (code, libc, the RGBA input).
- The ncnn backend refuses regions with dark text on a light box, light boxes
  and very tall lines (`VJO_E_OCR_REGION`). The vocr finder refuses none.
- Most of the remaining vocr error is the line finder's: with hand-drawn line
  boxes the same model reads 1.56% / 2.63%. The finder found 95.2% of the Vita
  lines and 90.2% of the PSP lines (`eval/run.py --finder console --polarity
  console`, which gives the same 6.78% / 14.70% concat CER as the overlay).
- Host time per region (process start, model load and check, recognition) was
  62 ms for H15 and 204 ms for ncnn, median. This says nothing about the Vita.

## On-console checklist

For a PS Vita on firmware 3.60-3.74 with the vocr build installed, the model
copied, and the settings above. Close other apps first.

1. **Boot.** Reboot and start a VN (CLANNAD, PCSG00415, is the game the
   previous memory tests used). `status.txt` should end with
   `bootstrap: worker started`.
2. **Region.** Open the overlay (L+R), press □, draw a box around the dialogue
   and press ×.
3. **One recognition.** Reopen the overlay on a dialogue line. Note what it
   shows: the text, or an error message.
4. **Log.** Fetch `ux0:data/VitaJPOverlay/log.txt` with every other log and record:
   `bash tools/install_ftp.sh --collect VITA_IP` (one zip; [self-test](self-test.md#collecting-everything---collect)). A completed job logs
   `game vocr rc=... VOCR3_H15_w8.vocr: WxH, lines=N USER workspace=B
   weights=read|resident resident=R, find=X ms load=Y ms recognize=Z ms
   frames=F io=0x... stat=0x... cleanup=...`. Success requires `rc=0` and
   `cleanup=0`. The first text-bearing job of a game logs `weights=read`;
   later ones log `weights=resident` with a much smaller `load`.
   A job that fell back logs `VOCR3_FL10_w8.vocr (fallback)` and a `game vocr
   fallback:` line with the default's needs. Three more lines follow:
   - `game vocr memory: free USER=U, needs workspace J + weights W + reserve
     4194304: check passed|refused, headroom H`: the game's free memory at that
     moment, also for a refused job (`rc=-103`), i.e. this title's real OCR
     budget, and what was left (H < 0: refused).
   - `game vocr speed: F frames in T us, P us per frame`: the recognizer's
     on-console time per CTC frame (a 640-px line at the model height is about 80 frames).
   - `game vocr lines: 917x30 114f 52.1ms ...`: each recognized line's crop
     (pixels), CTC frames and recognition time, six per log line. Record the shown text and a photo
   beside this log; host tests alone cannot prove recognition on the Vita.
5. **Errors.** A "not enough memory" message names its stage; record its
   wording with the `game vocr memory:` log line:
   - *the kernel could not allocate this region's screen capture*: kernel
     memory; select a tighter region (the model does not matter) or restart;
   - *the game has too little free memory for any OCR right now*: below its
     4 MiB reserve; try later or restart the game;
   - *... for OCR of this region*: select a tighter region;
   - *... for this OCR model's weights*: select FL10 (the smallest), also a
     tighter region unless the message says FL10 would fit;
   - *... the OCR model's weights, already the smallest*: a tighter region or
     try again later;
   - *could not be allocated although the memory check passed*: try again;
   - *the Shell has too little memory for the overlay's results*: restart the
     Vita (region and model do not matter).
   A Shell UI memory error after successful recognition preserves text
   while dictionary results are unavailable; restart the Vita and retry. A
   smaller OCR region does not reclaim another Shell plugin's retained heap.
   For an unavailable worker, check that the game has its own `*TITLEID` section with
   the worker (install with `--title TITLEID`) and reboot. A model error with nonzero `io=` identifies game-process file access
   or a kernel read failure; check the installed model against the package manifest.
   Install all three API 15 plugins together and reboot before testing.
6. **Speed.** With `rc=0`, repeat on five lines and note `load`, the
   `speed` line's microseconds per frame and the `lines` entries. The estimate
   for H15 is 166-302 ms per 640-px (80-frame) line at 444 MHz, about
   2.1-3.8 ms per frame, and about 1.33× that at the default 333 MHz.
7. **Polarity.** Try one game with dark text on a light box (for example
   VARIABLE BARRICADE or 偽りの仮面) and one with outlined text.
8. **Stability.** Leave `ocr_mode = auto` with subtitles on (Select+R) for ten
   minutes of reading. Watch for game slowdown, audio stutter, or a SceShell
   crash (`tools/install_ftp.sh --dumps VITA_IP` fetches crash dumps).
9. **Back to Lens.** Set `ocr_backend = lens`: nothing else changes.

## Limits

- Horizontal text only; vertical text is not read.
- Adjacent lines 1-2 px apart can merge on PSP-sized text, and decorated
  frames can add a stray line; tight regions read best.
- Recognition runs on a low-priority thread in the enabled game process at
  the game's clock. The overlay does not change the CPU clock or memory mode.
- The pinned table accepts the three v0.2.0 int8 files and the separate VOCR2 trial; a new release
  needs its sizes and hashes added (`core/vocr_ocr.c`, `tools/prepare_ocr.py`).
