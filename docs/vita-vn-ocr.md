# vita-vn-ocr: local OCR on the Vita CPU

The overlay's OCR engine, its only one, reads the selected dialogue region on the
console with [vita-vn-ocr](https://github.com/bee-san/vita-vn-ocr): a small Japanese
CTC line recognizer made for visual-novel dialogue, and its line finder, both plain
C99 with no `malloc`. Nothing leaves the Vita: no image upload, no connection and
no cloud fallback (Google Lens, the upstream's online engine, was removed on
2026-10-15). With `dictionary = local` the whole lookup is offline.

**Status: default in v0.7.5; physical recognition validation is still pending.** It builds with VitaSDK and
passes the host tests; everything below the "Host measurements" heading was
measured on a PC. On-console memory, speed and stability are what the
[checklist](#on-console-checklist) is for.

## Model sizes: Small, Normal, Large (API 21)

The Rubi app's OCR page offers, under the local engine, its **Model size**:
**Small** (fastest), **Normal** (the default, balanced) and **Large** (most
accurate); `config.ini` keeps it as `vocr_tier = small | normal | large`. Each size
is one pinned recognizer: this package's Small is VOCR5-S, its Normal VOCR5-N and
its Large VOCR5-L, vita-vn-ocr's VOCR5 round (`eval/vocr5_protocol.json` there: one
shape per size, selected on its fixed development sample, published as the v0.3.0
release). A later generation's blobs replace them through the pins, with no change
to the app; a size whose new candidate does not beat its model keeps that model,
which is a pin edit too (`include/vjo_vocr_sizes.h` VJO_VOCR_TIERn and
`tools/model_tiers.json`, the model pinned where every model is), unless it gives
two sizes one model: then the one-VPK payload list must also name that file once,
with `VJO_PAYLOAD_KERNEL_INDEX` one less (**Two sizes, one model**, below).
`tools/model_tiers.json` is the one table of the sizes (their ceilings, reserves,
models and estimates); the self-test analyser reads it, and
`tools/tests/test_model_tiers.py` checks every C copy against it
(`include/vjo_vocr_sizes.h`, `include/vjo_ledger_budget.h`, `core/config.c`,
`core/vocr_models.c`).

| size | model | shape | int8 weights | W8I arena | est. time per 3-line textbox | game memory a job takes | its reserve | needs free |
|---|---|---|---:|---:|---:|---:|---:|---:|
| Small | VOCR5-S | FL10's topology, 4,987 classes | 819,792 B | 124,832 B | 0.63 s | 1,019,904 B | 4 MiB | 5,214,208 B |
| Normal (default) | VOCR5-N | H15's topology, 5,631 classes | 1,185,632 B | 182,944 B | 0.92 s | 1,441,792 B | 8 MiB | 9,830,400 B |
| Large | VOCR5-L | late-wide (channels 64/128/256/480/576), 5,631 classes | 2,473,696 B | 261,024 B | 2.08 s | 2,805,760 B | 16 MiB | 19,582,976 B |

The sizes and arenas are exact (vita-vn-ocr `train/export.py`; `host/tests/test_vocr.c`
checks the arenas against the files). The times are estimates from
`eval/vocr5_protocol.json`'s "estimates" table: `vita/a9_layers.py` on the exact shapes
with vita-vn-ocr #85's runtime (the pin since API 22) and no credit for its weight
prefetch, one core at 333 MHz, 240 CTC frames (3 lines of about 640 px) and 0.1 s for the
line finder, every phase's high bound calibrated so that VOCR3-H15 costs its measured
4,941.7 us per frame on v0.8.0-test.1's console. At 444 MHz (`ocr_boost_clock`, the
default since API 22) the same estimates are 0.50, 0.72 and 1.63 s; with credit for the
prefetch 0.57, 0.81 and 1.47 s at 333 MHz; parallel lines (`ocr_threads`) take less again
([OCR speed](ocr-speed.md)). No console has run a VOCR5 model yet: the buttons' times are
estimates until a test build's logs measure them. Each size is within the owner's
weights ceilings of 2026-10-14 (Small < 1 MB, Normal <= 1.6 MB, Large <= 2.5 MB) and its
box time (Small < 1 s, Normal < 2 s, Large < 3 s), and Normal near the 1 s box at 333 MHz
the owner set for the default (2026-10-15). "Game memory a job takes" is a job's
page-rounded weights, workspace and capture block at the full screen (the game worker's
memory check), without the character LM, the cascade's model or the PSP refinement, which
a job leaves out first when memory is short, and without the helper threads of
`ocr_threads` (API 22), which a job takes only when the game also has room for them beside
the same reserve, and never runs a smaller size for. The app's buttons show the time and
that memory (`0.9 s, 1.4 MB`).

**The class counts.** VOCR5's models have 4,987 (Small) and 5,631 (Normal, Large) classes,
VOCR3's 3,501 and 5,355. Nothing in the overlay is sized by a class count: the top-K keeps
16-bit class numbers (`VJO_VOCR_LM_TOPK` of them a frame), a line's greedy text is sized at
4 bytes a frame (every class is one code point of at most 3 UTF-8 bytes in every pinned
model), and the LM decoder maps each model's classes to its tokens by code point when it
is planned; its workspace grows with the classes (41,632 B for 5,631 against 40,816 B for
5,355 on the host) and stays within `VJO_VOCR_LM_WS_BYTES` (49,152 B), which
`host/tests/test_vocr.c` checks for every pinned model and LM it finds.

**The ladder.** The Shell asks for the chosen size and every smaller one
(`VjoGameOcrRequest.tiers`); the game worker tries them largest first and runs the
first whose job fits the game's free USER memory beside that size's reserve
(vita-vn-ocr `docs/model-tiers.md` 1.5, the reserve the largest size the job holds,
the cascade's included), its own resident blocks of another size counted as free
because they are released first. §1.5 gives each size its own reserve; the job keeps
the largest held size's because §1.3's reason for a larger reserve is the resident
block, not the size's name: Normal reading with Large's model as its cascade holds a
Large-sized block, so it leaves Large's 16 MiB. Each try is a `check` step of the job
(`include/vjo_game_ocr.h` VJO_GSTEP_CHECK: the size in the kind, refused or
passed, what it needed). Nothing on screen says that a smaller size ran (owner
2026-10-15 04:54Z): the Shell writes it to `log.txt`, e.g.

```
tier large did not fit in PCSG00415 (need 19582976, free 12000000); using normal
```

with need = the size's workspace, weights and reserve, and free = the game's free
USER plus the blocks it would release. The `--collect` summary groups these lines
per size and game (`model sizes that did not fit (log only, never shown)`). Only
when no size fits does a job fail with the usual out-of-memory message.

**The cascade** (#89) reads the lines a job doubts again with the next size up,
when its model fits too: Small's is VOCR5-N, Normal's VOCR5-L, Large has none.
The kernel broker opens a size's model only by its size
(`VJO_GAME_OCR_OPEN_TIER`): one of the request's sizes, or with a cascade ceiling
the size above its largest; never a name or a path.

**Two sizes, one model.** vita-vn-ocr's VOCR5 protocol lets a size keep an earlier
model (`eval/vocr5_protocol.json`, amendment 3: Normal is the better of VOCR5-N and
VOCR4-W20, and Large keeps VOCR4-W20 unless VOCR5-L beats it), so Normal and Large
can be one file. The worker then gives Normal no cascade (`cascade_of` takes only a
larger next size up: the same model would read a doubtful line the same again),
so a Normal job keeps Normal's 8 MiB reserve; Small's cascade is that model,
opened as Normal's. A job opens the file as its own size, the one its request
allows (a Large-only request as Large's), and the weights stay resident when the
other size runs them. A bad file leaves out both of its sizes at once. The release
zip ships the file once; its manifest entry names both sizes (`"tier": "normal"`,
`"tiers": ["normal", "large"]`). The one-VPK install's payload list
(`include/vjo_payload.h`) still names one file per size, and the Rubi app refuses a
payload that names a file twice, so `tools/package_vpk.py` refuses such a list: a pin
edit that gives two sizes one model has to name that file once there first. Host tests run this with Normal and Large both
VOCR4-W20 (`game_vocr_shared_tiers`, `shell_worker_shared_tiers`,
`host/tests/test_shared_tiers.h`).

**A missing or damaged file** of a size (`VJO_E_OCR_MODEL` from the worker) leaves
that size out for the rest of the game's session, and the job is repeated with the
next smaller one; a timeout or unavailable storage repeats it for that job only.
Only `log.txt` says so (`game vocr: VOCR5_N_w8.vocr is unusable here
(io=0x80010002), using VOCR5_S_w8.vocr for this game`). The Rubi app's
status line names the chosen size's file missing (`VOCR5-N missing. Reinstall
the package.`), a broken install rather than a memory fallback.

**Older settings.** `vocr_model` (API 20 and before) sets the size by its file's
shape, the FL10 family (`FL10_w8.vocr`, `VOCR3_FL10_w8.vocr`, the VOCR2 trial's)
Small, H15 Normal, F20 and W20 Large, unless a `vocr_tier` line says otherwise;
`vocr_fallback_model` is no longer read. The app's next save writes `vocr_tier`
and drops both lines; `tools/migrate_config.py` (the installer) does the same.
`install-vita.sh` sets no size, so an update keeps the user's choice, and its
size's file is now VOCR5's (VOCR5 has no file names of the older shapes, so a
`vocr_model` never names one). VOCR3's `VOCR3_H15_w8.vocr` and `VOCR3_FL10_w8.vocr`,
VOCR4's `VOCR4_W20_w8.vocr` and v0.2.0's `H15_w8.vocr`, `FL10_w8.vocr` and
`F20_w8.vocr` stay pinned for the host tools (a request by name) but are no longer
shipped; an update leaves the files of an earlier package on the card, unused, and
they can be deleted.

## The engine's name: the model generation

The Rubi app has no engine button since Google Lens's removal: the OCR card's
last line names the chosen size and its model, and the model's name carries its
generation (`Normal: default, balanced. VOCR5-N found.`): **VOCR5** for each of
this package's sizes (VOCR3 for Small and Normal and VOCR4 for Large before it). When
the chosen size's file is not there at its release size the line says so (`VOCR5-L
missing. Reinstall the package.`) and names no other model, though a job then runs the largest
smaller size whose file is there (a job leaves a missing size out; with no file at
or below the chosen size nothing runs). "Found" means the file is there at its
release size: the app reads no weights, so a damaged file of the right size shows
as found until the game worker's SHA-256 check refuses it and the game's jobs
leave that size out. A job that runs a smaller size for memory is never shown.

The overlay counts every v0.2.0 model as generation VOCR1, so the card says
`VOCR1-H15` for v0.2.0's `H15_w8.vocr`, which vita-vn-ocr's reports call
`VOCR_H15`; there VOCR1 is v0.2.0's `FL10_w8.vocr` alone
(vita-vn-ocr `docs/vocr3-report-20261013.md`).

A `.vocr` blob has no generation in it: its 64-byte header holds the layout, a
CRC and the first 16 bytes of the checkpoint's SHA-256 (vita-vn-ocr
`train/export.py`). The generation therefore comes with the pin, from the
vita-vn-ocr release that published the file (VOCR1: v0.2.0; VOCR3:
v0.2.5-vocr3.1; VOCR4: v0.2.5-vocr4.1; VOCR5: v0.3.0), and is kept in three places
that the tests compare:

- `core/vocr_models.c`, per file: generation, shape and size. The Rubi app
  reads it; the game worker's pins (`core/vocr_ocr.c`: SHA-256, arena) and the
  kernel broker's (`kernel/game_ocr.c`: size) name exactly the same files.
- `tools/prepare_ocr.py` `VOCR_GENERATION`, per release tag.
- `package-manifest.json` in the release zip: `generation` (and `tier`, a size's
  model) for every shipped model, and `defaults.ocr_engine` for the default size's.

A later generation needs no app change: pin its files where every model is
pinned (`core/vocr_ocr.c`: SHA-256 and arena; `kernel/game_ocr.c`: size;
`core/vocr_models.c`: generation, shape and estimates; `tools/prepare_ocr.py`: the
release assets and `VOCR_GENERATION`; the tests fail while one of them is
missing), make them the sizes (`include/vjo_vocr_sizes.h` VJO_VOCR_TIERn_NAME and
`tools/model_tiers.json` "model"), and the app shows its name, as it shows VOCR5's
(`host/tests/test_settings_app.c` also builds the app against a made-up VOCR6 table
whose Large keeps VOCR5's model). The weights blocks hold the Large size's model
(`VJO_VOCR_MAX_BYTES`, the ledger's weights and cascade budgets page-rounded): a
larger Large raises those, which the memory ledger's check and `host/tests/test_ledger.c`
hold to the owner's 2.5 MB ceiling (`VJO_LIMIT_GAME_WEIGHTS`).

The VOCR2 trial (`ocr_backend = vocr2`, a separately trained FL10-sized
`VOCR2_FL10_w8.vocr` with the native textbox finder, opt-in in 0.7.12 to
v0.8.0-test.1) has been removed: its strict per-title promotion guard failed
([its results](benchmarks/vocr2-candidate-20261012.json)). A saved
`ocr_backend = vocr2`, `meiki` or `ncnn` (or `lens`) loads as the local engine,
with a config warning in `log.txt`; the app's next save and the installer delete
the line, as they do any `ocr_backend` line (the engine is no setting). The kernel no
longer opens `VOCR2_FL10_w8.vocr`; the file may be deleted.

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

| model | weights | recognizer arena | full 960×544 region | 880×144 dialogue box |
|---|---|---|---|---|
| `VOCR3_H15_w8.vocr` (Normal, the default), `H15_w8.vocr` (v0.2.0) | 1,155,968 B | 182,944 B | 1,409,024 B (1.34 MiB) | 1,404,928 B (1.34 MiB) |
| `VOCR3_FL10_w8.vocr` (Small), `FL10_w8.vocr` (v0.2.0) | 657,664 B | 124,832 B | 851,968 B (0.81 MiB) | 847,872 B (0.81 MiB) |
| `VOCR4_W20_w8.vocr` (Large), `F20_w8.vocr` (v0.2.0) | 1,558,016 B | 241,056 B | 1,871,872 B (1.79 MiB) | 1,867,776 B (1.78 MiB) |

The line finder's arena (83,323 B at 960×544) is smaller than every
recognizer arena, so it costs nothing extra.

**The chosen size and the smaller ones.** A job runs the chosen model size
(`vocr_tier`, Normal by default) when the game has room for it, else the next
smaller one ([model sizes](#model-sizes-small-normal-large-api-21)). The game
worker checks free `USER` memory before each job; a smaller size's resident
weights stay while the chosen one still does not fit, and are released for it
once it does. A missing, wrong or corrupt file of a size leaves that size out of
the game's later jobs (until the game restarts). The kernel opens a size's model
only by its size (and, since API 20, the cascade's when the request sets its
ceiling: the next size up; the LMs are separate files); the log names the model
that ran and each size that did not fit, with what it needed. Every pinned
model is inside
the console budget ceiling (weights ≤ 1,600,000 B, recognizer arena ≤
262,144 B; bee-san/vita-vn-ocr `docs/vita-memory-budget.md`). Since v0.7.1 inference runs in
`VitaJPOverlay_OCR.suprx`, which the kernel plugin loads into each game ON in `games.ini`
since #138 ([worker-loading.md](worker-loading.md); before, one `*TITLEID` section per game,
the installer's `--title`; releases up to v0.7.12 used `*ALL`). The `*main` plugin handles
UI/dictionary results and has no vocr inference code. The game worker checks
its process title and only accepts jobs for the foreground game after the
Shell has confirmed its per-game policy.

The workspace is one private `USER_RW` memblock in that game's process.
The worker requires enough reported free USER memory for the page-rounded
workspace plus the size's game reserve (4, 8 or 16 MiB); there is no Paf fallback for OCR.
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
allows only these names under the exact directory above:<!-- names from kernel/game_ocr.c, written by tools/docs_model_names.py -->
the recognizers `VOCR5_N_w8.vocr`, `VOCR5_S_w8.vocr` and `VOCR5_L_w8.vocr`
(VOCR5, release v0.3.0); `VOCR3_H15_w8.vocr` and `VOCR3_FL10_w8.vocr` (VOCR3,
release v0.2.5-vocr3.1); `FL10_w8.vocr`, `H15_w8.vocr` and `F20_w8.vocr` (VOCR1,
release v0.2.0); `VOCR4_W20_w8.vocr` (VOCR4, release v0.2.5-vocr4.1); and the
character LMs `vocr_lm_o3_500k.vlm`, `vocr_lm_o3_1000k.vlm`,
`vocr_lm_o3_2000k.vlm` and `vocr_lm_o3_4000k.vlm` (release v0.2.6-ctclm.2)<!-- end of the names -->,
each with its pinned file length; it does not accept a caller-supplied file
path. Each async Open,
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
missing, changed in size or corrupt, when another model's job needs their memory or
reads its own, when the game has not been the active one for 5 s of its running
time (another app in front, title disabled; the process time stops while the game
is suspended, so a resume keeps them: docs/vita-memory.md section 5.2) and when
the game exits. A job that finds no line reads them (and the LM and the cascade's
model) if they are not resident, so the first job with text does not wait for them
(docs/first-ocr.md). If the kernel cannot read the file's modification time, every
job reads and checks the weights again, as before. Local
lookup uses one 64 KiB result arena, with no connection pool. The prior
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
`free` of every block the job can still free (its own blocks, and a resident
one only while its contents are unverified), so the log always shows the job's
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

## Beam + LM decoding (API 20)

```ini
ocr_lm = 1000k   ; 4000k | 2000k | 1000k | 500k | off: the largest language model a job may use
```

Each line is decoded with a CTC prefix beam search and a character n-gram language model instead
of the recognizer's best class per frame. Both come from bee-san/vita-vn-ocr #82
(`runtime/vocr_lm.c`, `vocr.c`'s top-K extras), with its four LM tiers from the
[v0.2.6-ctclm.2 pre-release](https://github.com/bee-san/vita-vn-ocr/releases/tag/v0.2.6-ctclm.2)
(CC BY-SA 4.0, attribution shipped as `ocr/LM-ATTRIBUTION.md`; ctclm.1 is superseded: it was built
before the protected-title filter). While the recognizer runs, it keeps each CTC frame's 8 best
classes and the blank's score. The decoder then searches 8 beams over them, scoring each prefix
with the LM (`core/vocr_ocr.c` `lm_opts()`: alpha 0.5, each tier's bonus per character, a gate of
8 and the bracket weight, as #82 recommends). With the LM's weights at 0 it gives the greedy text
bit for bit. A line the recognizer reads as empty is never decoded.

The default is the 1 MB tier: with the confidence cascade (below) it reads within 0.012 points of
the 4 MB one (0.729 % against 0.717 % on the lines measured), in a quarter of the memory and of
the first read, as #82's calibrated costs advise for the Normal tier.

**Memory.** The LM is a resident `USER_RW` block (`VjoGameVocrLM`), like the weights:
- **Reading:** read through the kernel's file broker 32 KiB at a time, with size and SHA-256 checked
  against the pins at its first use.
- **Keeping:** kept between jobs while the game is the active one, and through 5 s of its running
  time without being it (docs/vita-memory.md section 5.2). A job that finds no line reads it, so the
  first job with text does not.
- **Releasing:** released when a job's memory check fails, when `ocr_lm = off`, after those 5 s, and
  on exit.
- **Tier choice:** a job takes the largest tier up to `ocr_lm` that fits the memory check (free
  USER minus the job's reserve (4, 8 or 16 MiB: the largest size it holds) must cover the workspace, the weights, the LM and the cascade's
  model), else a smaller tier, else none (greedy).
- **Workspace:** the job's workspace holds the decoder's extras (the top-K of every frame, the LM
  context and the beam workspace: 56,928 B, bounded for every pinned model and LM) and 4 KiB for
  the greedy text.

| LM | bytes (block, page-rounded) |
|---|---|
| `vocr_lm_o3_500k.vlm` | 499,824 (503,808) |
| `vocr_lm_o3_1000k.vlm` (default) | 987,600 (991,232) |
| `vocr_lm_o3_2000k.vlm` | 1,971,568 (1,974,272) |
| `vocr_lm_o3_4000k.vlm` | 3,781,072 (3,784,704) |

**Logs (the A/B record).**
- `gstep job lm` per job: `kind` the tier (1 500k, 2 1000k, 3 2000k, 4 4000k), arg the lines the LM
  changed, rc 0 or why the job read greedily; bytes: read or resident, the lines under 0.7
  confidence, the lines decoded. It also carries the LM's load time and the decode time.
- `gstep job check`: `kind` is what the memory check chose, the LM tier, plus 8 with the cascade.
- `game vocr speed:` is the job's own model: its CTC frames and its recognition time (runs and
  decodes), so its µs per frame compares with #81's API 19 figure. The cascade's re-reads are not
  in it, nor in each line's time in `game vocr lines:`.
- The job's 20-step log reserves a slot only for the release of a block this job can still free:
  its capture and workspace blocks, a tall line's buffer, and a resident block (weights, LM, W20)
  only while its contents are not verified. The self-test's first stage, the job that first reads
  all three, then logs 19 steps, and 20 in a PSP game with the finder refinement's `refine` step;
  it keeps its `lm`, `cascade` and `norm` steps, which carry their first read times. The fallback
  that stage forces needs no step of its own: its `check` (arg 1) records it. A step more than the
  log holds (a torn read, the capture block refused by USER, a tall line's two) drops the last of
  them, `norm` first; in a PSP game a normal first job with a tall line also drops its `norm`
  (audit-overlay-dialogue-7's step-log cases).
- `game vocr lines:` gives each line's confidence (`c0`-`c1000`, the smallest top-class probability
  over the frames that emit a character), `*` when the text shown differs from the greedy
  reading, and `^` when the cascade read the line again.
- `game vocr text N:` is the greedy text, and `... shown:` the decoded and normalised text when
  they differ.
- `tools/analyse_selftest.py --collect` sums them.

## Confidence cascade (API 20)

```ini
ocr_cascade_ms = 2000   ; 0 (off) or 250-3750: the job's recognition time the re-reads stay within
```

A line whose confidence (above) is under 0.7, #82's mark for a doubtful line, is read again on the
same pixels by a larger recognizer, VOCR4-W20 (vita-vn-ocr #78,
[v0.2.5-vocr4.1](https://github.com/bee-san/vita-vn-ocr/releases/tag/v0.2.5-vocr4.1), F20's shape:
1,558,016 B, 241,056 B arena), and decoded with the same LM. Its reading is the one shown. On the
lines measured that is better than keeping whichever reading is more confident (0.717 % against
0.753 % with the 4 MB LM). VOCR4-W20 does not qualify as a default under vita-vn-ocr's gates; it only reads again
the lines the default doubts. Since API 21 the cascade reads with the next size up of the
job's: with VOCR5 VOCR5-L for Normal, VOCR5-N for Small, none for Large (VOCR4-W20 for
Normal and VOCR3-H15 for Small before it); a request by name, the host tools', still reads
with VOCR4-W20 ([model sizes](#model-sizes-small-normal-large-api-21)). The measurements
below are VOCR3's and VOCR4-W20's.

- **Time ceiling.** A doubtful line is read again only while the job's recognition (every line's
  run and decode, the re-reads, and an estimate for the lines still to read; not the weights'
  loads) stays within `ocr_cascade_ms`. The estimate is the line's own time on this console times
  the cascade's model's weights over the job's model's, until a re-read measures more (the weights
  follow vita-vn-ocr's A9 estimates within 6 % for FL10, H15 and W20, and within 16 % for VOCR5's
  three: VOCR5-L over VOCR5-N is 2.09x, against an estimated 1.94-2.48x; VOCR4-W20 over VOCR3-H15
  1.35x, against 235 / 166 ms per 640-px line, 1.42x). Before VOCR5 it was a size class's cost (94,
  166 or 235 ms by FL10's, H15's or F20's bytes and up), which would count a 2.47 MB Large's re-read
  at 1.42x Normal's line, 57-73 % of its estimate. Past the ceiling the line is counted and left. The ceiling is per
  capture attempt: a job read again after the screen changed starts it again. 2 s is the Normal
  tier's 3-line-box ceiling (vita-vn-ocr `docs/model-tiers.md`, an estimate at 444 MHz). In #81's
  CLANNAD log H15 took 5.0-5.2 ms per CTC frame, about 0.4 s for a full line, so a 3-line box
  leaves room for about one re-read and a 1- or 2-line box for more (estimates). With VOCR5 the
  same holds: VOCR5-N's box is an estimated 0.92 s and VOCR5-L's re-read of a 640-px line 0.66 s
  (80 frames at 8.24 ms, one core at 333 MHz, no prefetch credit; `eval/vocr5_protocol.json`).
- **Memory.** The cascade's weights are a resident `USER_RW` block (`VjoGameVocrCascade`, at
  most the Large size's 2,473,984 B; VOCR4-W20's was 1,560,576 B),
  read and SHA-256 checked at the first doubtful line, or (test.3 on) by the first job that finds no
  line when its memory check takes the cascade, before any line is doubted: from then on the block is
  held whether or not a line is ever doubted (299-307 ms through the broker in test.2, once a game
  process), and kept like the job's weights. The job's workspace grows by
  the cascade's arena and decoder (VOCR5-L's 316,160 B beside VOCR5-N, VOCR5-N's 238,080 B beside
  VOCR5-S; W20's was 296,192 B). The memory check takes, in order: the cascade with the
  largest LM tier that fits, the LM alone, the cascade alone, neither. A resident block the job
  would give back counts as free. The block is given back when `ocr_cascade_ms = 0`, when the check
  leaves the cascade out, when memory is refused, and when the game has not been the active one for
  5 s of its running time.
- **Logs.** `gstep job cascade` per job:
  - `kind`: the lines W20 read as empty where the job's model read text. Those lines leave the text
    shown, and with them their greedy reading leaves the log. Keeping the first reading in that case
    changes nothing on the dialogue lines measured below: the `nonempty` sweep rows of
    `lm-cer-20261014.json` give 0.717 % with the 4 MB LM either way, and a run with
    `--lm vocr_lm_o3_1000k.vlm --sweep 700` gives 0.729 % either way. On those lines W20 empties
    none, so the rule stays "W20's reading is shown".
  - arg: the lines read again; rc: 0, or why the cascade could not run (W20 missing or wrong, no
    memory).
  - bytes: its weights read or resident, the lines it changed << 24, the doubtful lines left for the
    ceiling << 16, and W20's CTC frames.
  - Its load time and the re-reading time.

  `game vocr cascade speed:` gives W20's frames and time, and its µs per frame. In
  `game vocr lines:` a line read again ends in `^`, and its confidence is W20's.

**Measured on the host** (`docs/benchmarks/lm-lines-20261014.json`, `lm-cer-20261014.json`):

- **Parity.** The overlay's adapter (`vjo-ocr --lm`, `--cascade`) was run on the 318 linefind
  development regions (777 found lines) with VOCR3-H15 and VOCR3-FL10, each with all four LM tiers
  and the cascade. Against vita-vn-ocr's reference (`runtime/pyvocr.py`) on the same crops
  it had 0 differences in greedy text, decoded text, confidence, and which lines the cascade read
  again and what they show (`tools/lm_lines_eval.py`; 302-315 lines read again).
- **Dialogue CER** (`tools/lm_cer_eval.py`): 1,666 dialogue lines (25,379 characters) of
  vita-vn-ocr's development line crops (teacher_val, devnative v3 ink and finder-cut crops), scored
  with vita-vn-ocr's `eval/metrics.py`. #82 tuned the LM's two weights on these sets, so the LM
  columns are in-sample for them.

| recognizer | greedy | 500k | 1000k | 2000k | 4000k |
|---|---|---|---|---|---|
| VOCR3-H15 (default) | 1.399 % | 0.969 % | 0.934 % | 0.902 % | 0.887 % |
| VOCR3-H15 + cascade | 1.032 % | 0.776 % | **0.729 %** | 0.721 % | 0.717 % |
| VOCR3-FL10 (fallback) | 1.407 % | 1.032 % | 0.914 % | 0.875 % | 0.859 % |
| VOCR3-FL10 + cascade | 1.052 % | 0.796 % | 0.757 % | 0.713 % | 0.709 % |
| VOCR4-W20 on every line (reference) | 0.965 % | 0.705 % | 0.709 % | 0.670 % | 0.666 % |

- **What the cascade reads.** A confidence under 0.7 flags 21.5 % of H15's lines (22.3 % of the
  CTC frames) and 74.7 % of the lines it misreads (FL10: 24.2 %, 74.3 %). At 0.8 it would flag
  30 % of the lines and, with the 4 MB LM, read 0.697 %.
- **Time (host).** The decode takes 24-36 µs a line on the host; top-K tracking happens inside the
  recognizer's own run.
- **Time (A9, vita-vn-ocr #82's costs calibrated to #81's H15 time).** The top-8 tracking adds
  6-10 ms a line and the beam 0.4-0.7 ms. A re-read costs about 1.42x the line's own time.
- **Unmeasured on the console:** the first read of the LM (~0.2 s for 1 MB, ~0.7 s for 4 MB) and of
  W20 (~0.3 s), once per game session, from #81's 218-231 ms for H15's 1,155,968 B (estimates),
  and the cascade's real time per line. The next build's logs measure all of these.
- **The CER above assumes no ceiling.** The tools allow 600 s. On the console a 3-line H15 box
  leaves room for about one re-read (estimate), and about 12 % of such boxes have two or more
  doubtful lines (21.5 % per line, assuming independence). So 0.729 % bounds the gain; it is not a
  forecast.

## Finder refinement (API 20)

```ini
ocr_refine_lines = psp   ; psp: PSP games only | on | off
```

The line finder's boxes are taller than their text: its padding takes in the dialogue box's frame,
the edges of the next line, furigana, and sometimes two text rows set close. vita-vn-ocr #83
(`runtime/vocr_lines_refine.c`) refines each box from its own pixels: it cuts the box to its text
band, ignores frame rows, drops a trailing cursor or a portrait edge, and splits a box of several
text rows into one line each. A box never grows and no line is dropped.

- **No reads.** The job refines each box on the pixels it already holds for it, before it reads
  the box: its strip (the box made strip-relative), or a tall line's own block. So the refinement
  adds no strip read (a strip read took 11-26 ms on the console, #81).
- **The same lines as #83's.** A host test (`test_vocr_policy.c`) refines per box on the job's
  pixels and compares with #83's whole-set refinement of the whole region, line for line: 251
  procedural regions of one strip and of several (frames, panels, close rows, cursors, slabs, 23
  tall lines) and 120 crowded ones, in 93 of which the splits would pass 16 lines (436 boxes not
  split for the cap), 0 differences. Each line is read from the region's own pixels.
- **The 16-line cap**, as #83's: a box may become as many lines as keep the job within 16, one kept
  for each box still to come. A split past that is not made, and the box is refined without it: its
  main text band, tightened. No development region below reaches the cap.
- **Memory.** The refinement's arena (`vjo_vocr_refine_bytes`: 16,768 B at 883×112, 72,400 B at
  960×544) is part of the job's workspace when it is on, before the LM extras. Off, nothing
  changes: the lines are the finder's, exactly, and nothing is read or allocated for it.

**What it costs: recognition, mostly.** A refined line is shorter, so the recognizer scales it to a
larger width and reads more CTC frames: on the development regions below, +22.7 % on psp_dev
(10,867 → 13,338 frames, 7.4 more per found box), +8.9 % on vita_dev (16,581 → 18,051, 3.3 more),
and +12.1 % on psp_dev as the overlay captures PSP games (2× bilinear, below: 12,763 → 14,313, 4.4
more). The host job time rises about as much (+22 %, +9 % and +12 % in that run; host times vary
from run to run, frames do not). At the console's measured 4.94-5.22 ms per VOCR3-H15
frame (#81) that is about **37-39 ms more per box** at 480×272, **22-23 ms** at 2× and **16-17 ms**
on Vita text (estimates). The refinement's own work is small beside that: #83 estimates 0.53-1.07
ms per finder line at 444 MHz from qemu-arm instruction counts of the Vita build, an estimate, not a
console figure, and its console calibration (the finder's measured time) puts it near the top of
its 333 MHz range, about 1-1.5 ms per line. #83 counted with the finder's arena; the overlay gives
the refinement its minimum (`vjo_vocr_refine_bytes`), with which a box whose grey pixels do not fit
beside its mask is read twice from memory (it read 38.9 rows per PSP box and 53.9 per Vita box
here): expect the top of that range or a little above (estimate). The host takes 0.06 ms a PSP
region and 0.14 ms a Vita region.

**What was measured where.**

| what | where | screens | result |
|---|---|---|---|
| dialogue CER, whole dialogue (body text) | #83, once, on vita-vn-ocr's protected psp-test | native 480×272 PSP frames | −2.19 to −2.90 points; the VN-cluster 95 % intervals are below 0 for three of six models (VOCR3-H15, VOCR3-W20, VOCR4-W20) and include 0 for the other three |
| | #83, once, on vita-test | native 960×544 Vita frames | level or worse, −0.13 to +0.50 (intervals include 0) |
| | #83, once, on CLANNAD's long run | native 960×544 | worse for five of six models, +0.07 to +0.43 points (four resolved) |
| box fit (IoU of a line with its hand-marked box) | this PR, host, psp_dev | 480×272, as #83 measured | 0.559 → **0.591** |
| | this PR, host, psp_dev | **the overlay's PSP capture**: 480×272 shown 2× through the emulator's bilinear filter | 0.608 → **0.604**, level (2× nearest: 0.611 → 0.604) |
| | this PR, host, vita_dev | 960×544 | 0.594 → **0.594**, level |
| dialogue CER at the overlay's PSP capture | nowhere | 2× bilinear | **untested** |

The display hook gets a PSP game as Adrenaline shows it, its 480×272 frame shown 2× through the
emulator's bilinear filter ([stable text and blank lines](#stable-text-and-blank-lines-api-20)).
That puts its text at about twice PSP's pixel size, like native Vita text, where the protected sets
show no gain, and there the box-fit gain measured at 480×272 is gone. #83 recommends deciding by the
text size the overlay captures and checking it in the test build's A/B logs before the refinement
becomes a default. So `psp` is the next test build's trial, not a measured gain at the overlay's
scale, and that build's logs decide whether it stays. The worker decides by its own process: the PSP
emulator's main module `ScePspemu` is loaded (any Adrenaline bubble, whatever its ID, as AdrBubbleBooter's
`adrbubblebooter.suprx` checks), or its title is a PSP one (`core/games.c` `vjo_game_is_psp`:
`NPXS10028`, a `PSPEMU*` ID, a PSP disc ID). `on` refines in every game, `off` in none.

**Logs, and the A/B.** `gstep job refine` per job: kind the mode, arg the lines the boxes became, rc
0 or the first refusal (that box kept as found), bytes the boxes refined << 16 | those not split for
the cap << 8 | those split, sum the rows read (from memory), us the time. `tools/analyse_selftest.py
--collect` sums them and counts the modes. With them, `game vocr lines:` gives each line's size, CTC
frames, time and confidence beside the text shown. No job reads both forms: the A/B #83 asks for is
a PSP game's jobs with `psp` against the same screens with `off` (a backlog, a reloaded save).

**Measured on the host** (`tools/refine_eval.py`, `docs/benchmarks/refine-20261015.json`): the
linefind development regions, read by the job as shipped (VOCR3-H15, the 1 MB LM, the cascade) with
and without the refinement, and psp_dev's also as the overlay captures PSP games (`--how
x2-bilinear`, `x2-nearest`: the frame shown 2×, the region and the marked boxes with it). The
manifests mark each line's box by hand but hold no text, so this measures the boxes against the
hand-marked ones (IoU); the text accuracy is #83's.

| regions | regions read | lines read (boxes split) | mean IoU of a line with its marked box | marked lines covered (IoU ≥ 0.5) | CTC frames | regions whose text changed | refinement, all regions (host) |
|---|---|---|---|---|---|---|---|
| PSP (psp_dev) | 145 | 332 → 338 (6 split) | 0.559 → **0.591** | 65.5 → 69.3 % | +22.7 % | 66 | 9.0 ms |
| PSP, tight | 59 | 148 → 151 (3 split) | 0.678 → **0.707** | 70.9 → 72.8 % | +24.9 % | 32 | 3.8 ms |
| PSP, loose | 59 | 154 → 157 (3 split) | 0.553 → **0.591** | 60.1 → 65.8 % | +23.4 % | 31 | 4.4 ms |
| PSP shown 2×, bilinear (the overlay's) | 145 | 350 → 356 (6 split) | 0.608 → **0.604** | 73.4 → 74.7 % | +12.1 % | 43 | 24.1 ms |
| PSP shown 2×, nearest | 145 | 362 → 368 (6 split) | 0.611 → **0.604** | 77.8 → 77.8 % | +13.1 % | 44 | 22.9 ms |
| Vita (vita_dev) | 173 | 445 → 450 (5 split) | 0.594 → **0.594** | 78.5 → 79.8 % | +8.9 % | 65 | 24.5 ms |
| Vita, tight | 75 | 206 → 206 (0 split) | 0.716 → **0.721** | 80.8 → 81.8 % | +4.2 % | 28 | 10.4 ms |
| Vita, loose | 68 | 193 → 196 (3 split) | 0.606 → **0.605** | 75.9 → 77.6 % | +8.8 % | 30 | 11.0 ms |

At 480×272 the refined boxes fit the marked lines better (mean IoU +0.031, 3.8 points more of the
marked lines covered); shown 2×, as the overlay captures them, and on Vita regions they are level
(−0.004 to −0.008 and −0.0003; +0 to +1.3 points covered), as #83 found for the text on Vita. The
strips read are the same with and without the refinement.

## Text-presence gate (API 26)

```ini
ocr_text_gate = off   ; on: no lines where the gate sees no text
```

When the selected region holds no text (the game hid its text box, a CG screen, a menu without dialogue) the
line finder still finds "lines" in the picture, and the recognizer reads junk from them. vita-vn-ocr #100 / #101
(`docs/textgate.md` there) is a tiny int8 network that answers one question per region: does it hold text? With
`ocr_text_gate = on`, a region it says holds none returns no line: nothing is recognized and no text is shown (the
weights may still be read ahead by the session's warm-up, as after any job that finds no line:
[first-ocr.md](first-ocr.md)). Otherwise the finder's lines are read exactly as without it.

- **What it reads.** Each row of the region the first time the finder asks for it (`stream_row`), whether a strip
  read brought it or the strip already held it: no strip is read for it, and a region of one strip that the job
  waited on first (its rows all held before the finder starts) is scored like any other. When the finder found a
  line, the gate scores the region from the cells under the finder's boxes (vita-vn-ocr's `boxes` mode: the 4th
  largest text logit, text iff at least -8) and either keeps the lines or drops them all; when it found none,
  there is nothing to stop and the gate does not score.
- **Rubi's finder.** vita-vn-ocr measured the gate on GRAD + refine's boxes; Rubi's finder is the textbox
  recovery, whose boxes differ in some regions, so its score can be lower as well as higher. On vita-vn-ocr's
  development data (host, measured by Rubi's audit of this change), one of the 363 linefind regions scored lower
  with the recovery's boxes (an empty one, -17 to -32: stopped either way) and no decision changed; none of the
  1,515 whole-dialogue cases scored lower. On 13,074 training-side Vita regions they gave a lower score in 49
  (5 dialogue, 44 empty), and at the threshold 2 empty regions went from kept to stopped and 48 (47 dialogue) from
  stopped to kept, so no dialogue region GRAD's boxes kept was stopped (vita-vn-ocr's audit, `docs/textgate.md`
  "Limits").
- **The weights** are a file beside the models: `ocr/textgate_p8_c8_w8.vtd`, 1,540 B, sha256
  `a4e73ad78451c75a19f88a479fc0108daac85beaa88d875b73212ed3c09d85d1`, from
  [v0.3.2-textgate.1](https://github.com/bee-san/vita-vn-ocr/releases/tag/v0.3.2-textgate.1), shipped in the
  package with that release's `ATTRIBUTION.md` as `ocr/GATE-ATTRIBUTION.md` (`tools/prepare_ocr.py` pins both). Its
  licence is that file's: the weights are for non-commercial use only, as the recognizers are; they are data the
  plugin reads, not part of its code. The kernel's broker opens the file only for a request with the gate's bit
  (`VJO_GAME_OCR_OPEN_GATE`), at its pinned size. The worker reads it once, checks its SHA-256 and keeps it in a
  1,540 B static block (`VjoVocrGateResident`); each later job with the gate opens the file to check that it is
  unchanged (its size and time, as for the models) and reads it again only when it changed; `vocr_textgate_load`
  checks its CRC-32 in every job. Without the file (an `ocr` folder from an older package) the gate does not decide:
  the job reads as without it, and the gate's step says why.
- **The request.** `VJO_GAME_OCR_S_GATE`, bit 4 of the request's `speed` word. The flags have one free bit, bit 15,
  which bee-san/Rubi_for_vita#213 (vertical text) takes. The Shell sets it in the user's jobs when
  `ocr_text_gate = on`; the self-test's stages and shots never carry it.
- **Memory.** Its arena (35,529 B at 960×544) takes scratch the finder leaves free, which the recognizer's arena
  holds anyway at every region size (`test_vocr_policy.c` checks every pinned model at every size up to 960×544): no
  job's workspace grows. Module pages, measured on both builds (VitaSDK gcc 15.2.0): the worker gains 3,488 B of text
  and 1,560 B of .bss (the weights' block), on or off. The debug worker keeps its 98,304 B and the normal one takes
  one text page more, 94,208 → 98,304 B, in the game's memory. SceShell and the kernel keep their pages
  ([memory-ledger.md](memory-ledger.md)).
- **Time (estimate, nothing of the gate has run on a console):** about 67-135 ms per 900×150 text box on one core at
  333 MHz, 18-36 ms per 450×80 box, 256-517 ms for a whole 960×544 screen. These come from vita-vn-ocr's cycle model
  on the Vita compiler's `-O2` code, scalar; Rubi builds the gate at `-O2` with the rest of the runtime. It runs
  during the finder's pass, before the 444 MHz boost (which starts with the first recognized line), in every job
  while it is on; on an empty region it saves the recognition. The file's open in each job is not in the estimate.

**What it does, measured in vita-vn-ocr (host, GRAD + refine behind it):**

| data | without the gate | with it |
|---|---|---|
| 66 development text-free regions with no line | 5 | 59 |
| development whole dialogue, VOCR5-S / N / L | 5.21 / 4.78 / 4.46% | 5.28 / 4.84 / 4.53% (not resolved worse) |
| vita-test + psp-test whole dialogue, read once | 9.51 / 9.16 / 8.35% | unchanged |
| CLANNAD dialogue (2,052 lines), read once | 1.43 / 0.92 / 0.63% | unchanged |
| CLANNAD pages without reference text (133) | 162 lines | 114 lines |

**Why it is off by default.** On the text Rubi reads it changes nothing measured, and on empty regions it
stops most junk. But it costs an estimated 0.1 s in every job, at the game's clock, and how often a player's
region is empty (and how much junk that shows) is unmeasured. A console A/B decides: the same screens with the key
on and off (a backlog, a reloaded save, a CG with the text box hidden), in CLANNAD and in a game the measurements
above never read.

**Logs.** `gstep job gate`, once per job (its last try) whose finder found a line, or whose gate could not run:
- kind: its mode (1: boxes);
- arg: the lines it stopped (0: it kept them, the region holds text);
- rc: 0, or why it could not decide (the lines then kept; `VJO_E_OCR_MODEL` when its file is missing or not the
  pinned one);
- bytes: its score + 129, its threshold + 129 from bit 16, and bit 31 (`VJO_GLM_READ`) when the job read its
  weights from the card;
- sum: the rows it took; us: its time.

A job it stopped logs `find` with arg 0 and no `recognize` step. A job whose finder found no line logs no gate step.
`tools/analyse_selftest.py --collect` counts the gate's jobs, those it stopped, their scores and the weights' reads.

## Models

**The default size, Normal, is VOCR5-N (`VOCR5_N_w8.vocr`); Small is VOCR5-S
(`VOCR5_S_w8.vocr`) and Large VOCR5-L (`VOCR5_L_w8.vocr`).** They are vita-vn-ocr's VOCR5
recognizers (`eval/vocr5_protocol.json` and `docs/vocr5.md` there), one shape per size,
published as the [v0.3.0 release](https://github.com/bee-san/vita-vn-ocr/releases/tag/v0.3.0).
VOCR5-S keeps FL10's topology and arena with a 4,987-class head, VOCR5-N H15's with a
5,631-class head, and VOCR5-L is a late-wide shape (channels 64, 128, 256, 480, 576) with
5,631 classes. A first full-screen job of Small takes 163,840 B more than one of VOCR3-FL10
(1,019,904 against 856,064 B: the larger head's weights), Normal 28,672 B more than VOCR3-H15
(1,441,792 against 1,413,120 B) and Large 933,888 B more than VOCR4-W20 (2,805,760 against
1,871,872 B), for which the memory ledger's weights and cascade budgets are raised
(`include/vjo_ledger_budget.h`).

| file | bytes | sha256 | W8I arena | release |
|---|---|---|---|---|
| `VOCR5_N_w8.vocr` (Normal, the default) | 1,185,632 | `20be179def8033171435ba62388657b3d3c932b365093737948c636e025e3bdc` | 182,944 | v0.3.0 |
| `VOCR5_S_w8.vocr` (Small) | 819,792 | `f175bfcb8cec65d69e85dd5f2cf707c9a711ada7203c0d1099cd2d0142978b8d` | 124,832 | v0.3.0 |
| `VOCR5_L_w8.vocr` (Large) | 2,473,696 | `50413b37dbd85b2acb0908967bdeaf7ca67803850e0627a7b7cc3d274edc0ba3` | 261,024 | v0.3.0 |
| `VOCR3_H15_w8.vocr` | 1,155,968 | `cb3c61a8f91f9aeeb3efcc0e79313f664112f25c4bd55865bae9df52522b360c` | 182,944 | v0.2.5-vocr3.1 |
| `VOCR3_FL10_w8.vocr` | 657,664 | `87fb4d0c18416f5f3771cecd274c8db3cf69b8b4a4ee9f204edebb4b82079154` | 124,832 | v0.2.5-vocr3.1 |
| `VOCR4_W20_w8.vocr` | 1,558,016 | `2745e69699ad7b2aba3092f83e5ddc1912f405f54b9df8a2e55be82c385b716e` | 241,056 | v0.2.5-vocr4.1 |
| `H15_w8.vocr` | 1,155,968 | `316ebc377ec34520440991c5085199d8e74031f01a867938be90a60c1e3f03cd` | 182,944 | v0.2.0 |
| `FL10_w8.vocr` | 657,664 | `500e0b842e5f0cb326f55e7d991a52ab66dd24977ccc89bd188228aedc825e84` | 124,832 | v0.2.0 |

VOCR5's scores are in vita-vn-ocr's `docs/vocr5.md` (its locked report). No console has run
a VOCR5 model yet.

**Their attribution and terms.** v0.3.0 ships an `ATTRIBUTION.md` (in its `SHA256SUMS`), which
the package ships beside the weights as `ocr/MODEL-ATTRIBUTION.md` (`tools/prepare_ocr.py`
pins it; the one-VPK install copies it with the models). It names what the recognizers were
trained on. Pretraining used line crops of the Japanese subset of NVIDIA's
OCR-Synthetic-Multilingual-v1 (CC BY 4.0, modified; not redistributed). The vocabularies are
ordered by Jiten's frequency lists (CC BY-SA 4.0). The synthetic dialogue boxes include fonts and
text pools that are free for non-commercial use only, so the file says to **use these weights for
non-commercial purposes only**. VOCR3's and VOCR4's releases had no such file. Before VOCR5 the sizes were VOCR3-FL10, VOCR3-H15 and VOCR4-W20:
vita-vn-ocr's VOCR3 report (host, plain-C W8I runtime, `docs/vocr3.md` there) gives pooled
vita-test + psp-test fixed-line CER 1.73% for VOCR3-H15 against 2.21% for v0.2.0 H15, and 2.13%
for VOCR3-FL10 against 3.82% for v0.2.0 FL10; none of the three VOCR3 models passed that
repository's promotion gate, and VOCR4-W20 (vita-vn-ocr #78, F20's shape) did not qualify as a
default either. On the console VOCR3-H15 has run as the default (v0.8.0-test.1 and test.2),
VOCR3-FL10 in test.2's self-test fallback stage and VOCR4-W20 there as the confidence cascade.
The earlier models are no longer shipped or selectable (no size names them), but the game
worker and the kernel's model broker still accept them by name, for the host tools:
they accept exactly these files, each at its pinned size and SHA-256:<!-- names from kernel/game_ocr.c, written by tools/docs_model_names.py -->
the recognizers `VOCR5_N_w8.vocr`, `VOCR5_S_w8.vocr` and `VOCR5_L_w8.vocr`
(VOCR5, release v0.3.0); `VOCR3_H15_w8.vocr` and `VOCR3_FL10_w8.vocr` (VOCR3,
release v0.2.5-vocr3.1); `FL10_w8.vocr`, `H15_w8.vocr` and `F20_w8.vocr` (VOCR1,
release v0.2.0); `VOCR4_W20_w8.vocr` (VOCR4, release v0.2.5-vocr4.1); and the
character LMs `vocr_lm_o3_500k.vlm`, `vocr_lm_o3_1000k.vlm`,
`vocr_lm_o3_2000k.vlm` and `vocr_lm_o3_4000k.vlm` (release v0.2.6-ctclm.2)<!-- end of the names -->.
`core/vocr_models.c` gives each recognizer its generation (VOCR5, VOCR3, VOCR4, or VOCR1 for
v0.2.0's), the name of the local engine in the Rubi app.

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
pinned commit and point the build at it:

```sh
git clone https://github.com/bee-san/vita-vn-ocr /path/to/vita-vn-ocr
git -C /path/to/vita-vn-ocr checkout ac0d6b1d3da5e96cd771a48da7992b6cf25d6519  # #101's merge (after #85's, #83's and #82's): the text-presence gate, the faster kernels, the refinement, the beam + LM decoder

# Vita plugins and release zip (build/vita/Rubi-VERSION.zip), Release as in CI.
# The zip needs the frozen dictionary's checkout (docs/guide.md, "Build and validation").
cmake -S vita -B build/vita -DCMAKE_BUILD_TYPE=Release -DVJO_WITH_VOCR=ON \
  -DVJO_VOCR_SOURCE_DIR=/path/to/vita-vn-ocr -DVJO_PLAINTEXT_SOURCE=/path/to/plaintext-dicts
cmake --build build/vita --target release

# Host tools and tests; VJO_VOCR_MODEL_DIR adds the real-model checks
cmake -S . -B build/host -G Ninja -DVJO_WITH_VOCR=ON -DVJO_VOCR_SOURCE_DIR=/path/to/vita-vn-ocr \
  -DVJO_VOCR_MODEL_DIR=$PWD/ocr
cmake --build build/host && ctest --test-dir build/host --output-on-failure
```

`cmake/VocrOCR.cmake` checks the twelve runtime files (`runtime/vocr.c`, `vocr.h`,
`vocr_lines.c`, `vocr_lines.h`, `vocr_lines_rows.c`, `vocr_lines_rows.h`, `vocr_lm.c`,
`vocr_lm.h`, `vocr_lines_refine.c`, `vocr_lines_refine.h`, `vocr_textgate.c`, `vocr_textgate.h`) against their
SHA-256 at that commit, and refuses others unless `-DVJO_VOCR_ALLOW_UNPINNED=ON`. For the host tests that run
the text-presence gate, `cmake/VocrGate.cmake` downloads its pinned weights from the release at configure time and
checks them (or takes `-DVJO_VOCR_GATE_BLOB=FILE`; without either, those tests skip); the plugins read the file
from `ocr/` at run time. Copying those files to
`third_party/vita-vn-ocr/runtime/` works too and needs no source directory.
Without `VJO_WITH_VOCR` the Shell has no OCR engine: a job reports that the
backend is missing (install a build made with `VJO_WITH_VOCR`).

## Install

The complete v0.7.5 ZIP already includes both int8 models and the local
configuration; use its `install-vita.sh` as described in that ZIP's README. The
steps below cover a source build or adding another model.

1. Install the vocr build's plugins as in the [guide](guide.md#install).
2. Download and check the weights on a computer:

   ```sh
   python3 tools/prepare_ocr.py --backend vocr-tiers --output ocr    # the 3 sizes' models, MODEL-ATTRIBUTION.md, the gate's 2 files
   python3 tools/prepare_ocr.py --backend vocr-lm --output ocr       # the 4 LMs and ATTRIBUTION.md
   python3 tools/prepare_ocr.py --backend vocr --model VOCR4_W20_w8.vocr --output ocr   # by name: the host tools' cascade
   python3 tools/prepare_ocr.py --backend vocr --model VOCR3_H15_w8.vocr --output ocr   # by name: an earlier one
   ```

   The downloads need no GitHub account: vita-vn-ocr's releases are public, and the
   GitHub CLI (`gh auth login`) is only tried when a public download fails (a draft
   release). `--from FILE` checks a copy downloaded another way.
   `tools/package_release.py --models ocr` needs the first two lines' files: it ships
   the LMs with their attribution as `ocr/LM-ATTRIBUTION.md` (CC BY-SA 4.0), and the
   sizes' models with theirs, v0.3.0's `ATTRIBUTION.md`, as `ocr/MODEL-ATTRIBUTION.md`
   (kept under that name in `ocr/` too, beside the LMs' `ATTRIBUTION.md`), and the text-presence
   gate's `textgate_p8_c8_w8.vtd` with v0.3.2-textgate.1's `ATTRIBUTION.md` as `ocr/GATE-ATTRIBUTION.md`.
3. Copy the files to `ux0:data/VitaJPOverlay/ocr/`, the attributions included, or let the
   FTP installer do it: `tools/install_ftp.sh --vocr-model ocr/VOCR5_N_w8.vocr --set vocr_tier=normal VITA_IP`.
4. In `ux0:data/VitaJPOverlay/config.ini`:

   ```ini
   ocr_model_dir = ux0:data/VitaJPOverlay/ocr
   vocr_tier = normal
   ocr_mode = auto
   log_file = on
   ```

   `auto`, the default (the Rubi app's Read text, Automatically), reads each
   settled dialogue screen in the background, a recognition per screen; `on_press`
   (Only when I press L + R) reads only when the overlay opens
   ([auto-vs-press.md](auto-vs-press.md)).
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
   A job that ran a smaller size logs its model with `(fallback)` and, for each
   size that did not fit, `tier large did not fit in PCSG00415 (need N, free F);
   using normal`. Three more lines follow:
   - `game vocr memory: free USER=U, needs workspace J + weights W + reserve
     R: check passed|refused, headroom H`: the game's free memory at that
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
   - *the game has too little free memory for any OCR right now*: below the
     job's reserve (4, 8 or 16 MiB by size); try later or restart the game;
   - *... for OCR of this region*: select a tighter region;
   - *... for the OCR model's weights*: no size fits, the smallest one asked for
     included; a tighter region or later in the game. Like every message it names
     no size (log.txt has the `tier ... did not fit` lines);
   - *could not be allocated although the memory check passed*: try again;
   - *the Shell has too little memory for the overlay's results*: restart the
     Vita (region and model do not matter).
   A Shell UI memory error after successful recognition preserves text
   while dictionary results are unavailable; restart the Vita and retry. A
   smaller OCR region does not reclaim another Shell plugin's retained heap.
   For an unavailable worker (`OCR is not running in this game`), close the game and start it
   again: the kernel loads the worker as a game that is ON starts, so a game turned ON while it
   ran, or started with L held, has none until then; `kernel.txt`'s `worker for` line says what
   its start decided. A model error with nonzero `io=` identifies game-process file access
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

## Limits

- Horizontal text only; vertical text is not read.
- Adjacent lines 1-2 px apart can merge on PSP-sized text, and decorated
  frames can add a stray line; tight regions read best.
- Recognition runs on low-priority threads in the enabled game process. While
  a job recognizes, the ARM clock is 444 MHz (`ocr_boost_clock = on`, the
  default; a clock already at 444 MHz or more is left as it is) and is put
  back after ([OCR speed](ocr-speed.md#the-arm-clock-ocr_boost_clock-on)); the
  bus and GPU clocks and the memory mode are never changed.
- Only the pinned files listed under [Models](#models) load. A new release needs its sizes, hashes
  and generation added (`core/vocr_ocr.c`, `core/vocr_models.c`, `kernel/game_ocr.c`,
  `tools/prepare_ocr.py`); `python3 tools/docs_model_names.py` then writes the new names into those
  lists and the guide's.
