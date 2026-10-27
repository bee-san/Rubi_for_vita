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
release), each with vita-vn-ocr #117's head gate, and Normal's and Large's with its two-level head, since API 42
(the v0.3.6-head2level.1 pre-release: [Head gate and two-level head](#head-gate-and-two-level-head-api-42)). A later generation's blobs replace them
through the pins, with no change
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
| Small | VOCR5-S | FL10's topology, 4,987 classes | 821,584 B | 124,880 B | 0.51 s | 1,019,904 B | 4 MiB | 5,214,208 B |
| Normal (default) | VOCR5-N | H15's topology, 5,631 classes | 1,189,408 B | 184,448 B | 0.72 s | 1,445,888 B | 8 MiB | 9,834,496 B |
| Large | VOCR5-L | late-wide (channels 64/128/256/480/576), 5,631 classes | 2,477,472 B | 262,528 B | 1.82 s | 2,813,952 B | 16 MiB | 19,591,168 B |

The sizes and arenas are exact (vita-vn-ocr `train/export.py`; `host/tests/test_vocr.c`
checks the arenas against the files). The times are estimates from
`eval/vocr5_protocol.json`'s "estimates" table: `vita/a9_layers.py` on the exact shapes
with vita-vn-ocr #85's runtime (the pin since API 22) and no credit for its weight
prefetch, one core at 333 MHz, 240 CTC frames (3 lines of about 640 px) and 0.1 s for the
line finder, every phase's high bound calibrated so that VOCR3-H15 costs its measured
4,941.7 us per frame on v0.8.0-test.1's console, less the head gate's estimated saving at
the share of frames it skips on vita-vn-ocr's finder-cut development lines (0.12, 0.14 and
0.16 s) and, for Normal and Large, the two-level head's beyond it (0.06 and 0.09 s;
[Head gate and two-level head](#head-gate-and-two-level-head-api-42)). At 444 MHz (`ocr_boost_clock`, the default since
API 22) the same estimates are 0.41, 0.57 and 1.43 s; with `ocr_head_gate = off` 0.63, 0.92
and 2.08 s (0.50, 0.72 and 1.63 s at 444 MHz), and with credit for the prefetch (and
neither) 0.57, 0.81 and 1.47 s at 333 MHz; parallel lines (`ocr_threads`) take less again
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
that memory (`0.7 s, 1.4 MB`).

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
Only `log.txt` says so (`game vocr: VOCR5_N_w8g2.vocr is unusable here
(io=0x80010002), using VOCR5_S_w8g.vocr for this game`). The Rubi app's
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
the recognizers `VOCR5_N_w8g2.vocr`, `VOCR5_S_w8g.vocr` and `VOCR5_L_w8g2.vocr`
(VOCR5, release v0.3.6-head2level.1); `VOCR3_H15_w8.vocr` and
`VOCR3_FL10_w8.vocr` (VOCR3, release v0.2.5-vocr3.1); `FL10_w8.vocr`,
`H15_w8.vocr` and `F20_w8.vocr` (VOCR1, release v0.2.0); `VOCR4_W20_w8.vocr`
(VOCR4, release v0.2.5-vocr4.1); `VOCR5_V_w8.vocr` (VOCR5, release
v0.3.2-vertical.1); and the character LMs `vocr_lm_o3_500k.vlm`,
`vocr_lm6_o3_1000k.vlm`, `vocr_lm_o3_2000k.vlm` and `vocr_lm6_o3_4000k.vlm`
(release v0.3.7-lm6.2)<!-- end of the names -->,
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
  because a text box wraps mid-word and `もう一` / `度` is far more common there than `すごー`;
- counters (vita-vn-ocr #109): `ッ` becomes `ヶ` before `月` or `年`, before `国` or `条` after a numeral
  (`〇一二三四五六七八九十百千万`, `0-9`) or `何数幾`, and before `所` after one of those other than `一`
  (`ルッ月です」` → `ルヶ月です」`; the valid `一ッ所` stays). `ッ月` and `ッ年` occur nowhere in the
  700 million characters of training-side text vita-vn-ocr counted (`docs/counters.md` there); `ッ`
  before `日`, `村` or `町` stays. It runs before the ー/一 rule, so `ハネムーン一ッ月` keeps its `一`;
- a line made only of a box's border or scene art read as text is dropped, with its line break:
  three or more characters, each one of `ー ニ 一 二 ― - ~ ・` (after the forms above), at least
  two different ones and `ー` or `ニ` among them, such as the `ーニーーーーニーニー…` a text box's
  decorative bottom edge reads as. The kanji numerals (`一二三`), a pause (`――――`, `ーーー`), a word
  (`ニート`) and a line with anything else on it (`「ーー」`, `ニーー!`) stay; a line goes whole or
  not at all. It follows `non_japanese_filter` in `config.ini`: `lines`, the default, drops such a
  line, `none` keeps it (API 36: the Shell sets `VJO_GAME_OCR_S_DROP_BORDERS`, bit 16 of the job's
  `speed` word, in the user's jobs; the self-test's stages read as before). A native hook's text
  gets the same rule; a game's own dialogue is not expected to hold such a line.

The text never grows, so the game worker normalises it in place before it returns it, and keeps
the recognizer's own text after it in the same 4 KiB buffer (`vjo_text_normalize_keep`; nothing
is added to SceShell). The Shell writes the recognizer's text to `log.txt`, one line per
recognized line (`game vocr text N: …`, 60 characters at most), and, for a line that changed,
what is shown (`game vocr text N shown: …`); after a dropped line the shown lines are the next
ones, so the log pairs them with the recognizer's lines by position. The job's `gstep job norm`
step counts the changes to OCR's text (`bytes`: full-width forms; `sum`: tildes << 24 | dashes << 16 | ellipses << 8 |
一→ー << 4 | ー→一; API 36, `kind`: the lines of a border dropped; the counter rule's changes count in its total
only; a hook stream's text is not counted), and is logged also when the lines it dropped were all the text.

**The border rule, measured on development data** (postproc-research's R08d, measured again on this
code: `docs/benchmarks/textnorm-border-20261021.json`). The inputs are Rubi's default path on the
VOCR5 development cohorts (VOCR5-S/N/L, the beam with the 1000k LM, the confidence cascade): teacher_val
dev (8,000 dialogue characters), devnative dev (8,332) and 1,161 whole-dialogue units (25,854), with the
whole dialogue also in the Adrenaline 2× view a PSP game has on the console. Each region's text went
through the C code as the worker runs it. The rule changes one unit on every model size in both views,
`v21004_142145`'s +8 px region, whose 35-character line is the box's bottom border, and nothing else:

| whole-dialogue CER | Small | Normal | Large |
|---|---|---|---|
| native view | 5.167 → 5.032 % | 5.055 → 4.920 % | 4.986 → 4.850 % |
| Adrenaline 2× view | 3.361 → 3.226 % | 3.152 → 3.017 % | 3.129 → 2.994 % |

The fixed lines (teacher_val and devnative) do not change. With one unit, the gain's VN-cluster
interval reaches 0 (Normal, native: −0.135 points, 95 % −0.446 to 0). On correct text it dropped none of
the 2,298 development reference lines and none of the 1.6 million lines of vita-vn-ocr's training
corpus (`shared/data/corpus`: dialogue, narration, Wikipedia). Wrapped every 20 or 26 characters, as a
text box wraps, it dropped 1 of 8.8 million pieces: `ニーニ`, the end of `ベルニーニ` on a Wikipedia
line without a final mark. That one case is why `non_japanese_filter = none` turns it off.

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

## Counters: a numeral or a 「 the line lost (vita-vn-ocr #109)

On the owner's console, CLANNAD's `「九ヶ月です」` came back as `ルッ月です」`: 「 lost, 九 read as ル, ヶ as ッ.
The text rule above turns the ッ into ヶ. Before the LM, the job also runs vita-vn-ocr's counter re-decode
(`runtime/vocr_post.c` `vocr_post_redecode`, on the top-K the LM extras hold; `VjoVocrPolicy.counter_tau`
and `counter_open`, `VJO_VOCR_COUNTER_TAU` 6000 and `VJO_VOCR_COUNTER_OPEN` 4000, 1/1000 nat). Where a line
reads a letter that is not a numeral before ッ/ヶ/ヵ and 月/所/年/国/条, it takes the cheapest numeral in
that character's frames, if the frames allow one within 6 nats of the reading. Where the line's first
bracket is a closer with no opener, it puts the 「 or 『 back before the numeral, if a blank frame there
holds it within 4 nats. The LM then decodes the edited top-K. The recognizer's own reading stays the one
logged. Off (0, 0) the job is exactly as before. A job without the LM extras (`ocr_lm = off`, no cascade)
has no top-K and does not run it. The game worker turns it on for every job; the cache keeps its entries
apart (bit 29 of `cache_variant`).

vita-vn-ocr chose the thresholds on development data only (`docs/counters.md` there): VOCR5-S/N/L on
teacher_val dev, devnative dev and whole dialogue changed 0 lines. On a synthetic counter set drawn from
the development text pools in open fonts, with the left edge cut 0-10 px, CER fell from 4.81 / 5.18 /
4.29 % to 3.55 / 3.87 / 3.05 % greedy (S / N / L), and from 3.78 / 4.08 / 3.40 % to 3.45 / 3.60 / 2.98 %
with the LM, with 0 lines worse. On the owner's photo of that page (a local check, not the console's
framebuffer), through `vjo-ocr` with the region's left edge 20-22 px into the line, Normal's `ルヶ月です」`
and Large's `しヶ月です」` became `九ヶ月です」`. The 「 comes back only when the region leaves the line's
left edge in it: vita-vn-ocr #110 is the follow-up (capture a margin around the region).

`vjo-ocr --counters on` runs it on the host as the worker does (it needs `--lm` or `--cascade`).

## Beam + LM decoding (API 20)

```ini
ocr_lm = 1000k   ; 4000k | 2000k | 1000k | 500k | off: the largest language model a job may use
```

Each line is decoded with a CTC prefix beam search and a character n-gram language model instead
of the recognizer's best class per frame. Both come from bee-san/vita-vn-ocr #82
(`runtime/vocr_lm.c`, `vocr.c`'s top-K extras). The four LM tiers come from the
[v0.3.7-lm6.2 pre-release](https://github.com/bee-san/vita-vn-ocr/releases/tag/v0.3.7-lm6.2)
(CC BY-SA 4.0, attribution shipped as `ocr/LM-ATTRIBUTION.md`):
- the 1 MB and 4 MB tiers are VOCR6's LMs, built with the text of the owner's hooked console captures
  besides the open corpora (vita-vn-ocr #127, `docs/vocr6-lm.md`);
- the 0.5 MB and 2 MB tiers are v0.2.6-ctclm.2's, byte for byte (#82).

While the recognizer runs, it keeps each CTC frame's 8 best classes and the blank's score. The
decoder then searches 8 beams over them, scoring each prefix with the LM (`core/vocr_ocr.c`
`lm_opts()`, with each tier's settings from `lms[]`: the 1 MB and 4 MB tiers weight the LM 0.6,
give 2.5 nats a character and start each line from the start-of-line context without an
end-of-line term; the 0.5 MB and 2 MB tiers keep #82's 0.5, 3 nats and end-of-line term; every tier
has a gate of 8 and the bracket weight). With the LM's weights at 0 it gives the greedy text bit for
bit. A line the recognizer reads as empty is never decoded.

The default is the 1 MB tier. On vita-vn-ocr's development data the VOCR6 settings and LMs read the
hooked titles' pages better than ctclm.2's at Rubi's old settings (VOCR5-N with its cascade, page
path 4.12 % → 3.39 %, cross-validated), and are level on unseen games (#127; no console run yet). They
ship under VOCR6's adoption rule as the conductor ruled it (2026-10-26 16:38Z): better on the hooked
titles' held-out pages and not worse on the unseen-game development sets (game-bootstrap 95 % interval).
These LM figures, and the counter set's below, were measured with VOCR5 v0.3.0's blobs and no head gate. Rubi runs the
gated blobs with `ocr_head_gate = on` by default (API 42, [below](#head-gate-and-two-level-head-api-42)), which was
validated with ctclm.2's 1 MB LM at its old settings; the gate skips about 0.2 % of the frames that are not blank, so
the difference is probably small, but the default combination of the gate and the new settings has not been
measured. Larger is not always better here: the 2 MB tier is ctclm.2's at its old settings, and on the hooked titles'
pages it reads worse than the new 1 MB default (vita-vn-ocr: 3.96 % against 3.39 %).

**Counters.** On #109's synthetic counter set ([counters](https://github.com/bee-san/vita-vn-ocr/blob/main/docs/counters.md):
189 lines of public text with a numeral before ヶ月, ヶ所 …, in three forms and seven left-edge cuts, 3,969
crops; VOCR5-N's cached top-8, Rubi's counter re-decode, beam + LM and counter rule) the 1 MB game-text LM
keeps the small ke in 3,015 crops against ctclm.2's 3,104: it loses 121 crops that ctclm.2 kept and gains 32
(dialogue CER 3.60 % → 3.78 %). The difference is mostly in crops whose left edge cuts into the line: with a
3 px margin before the line's first ink (the uncut form), 27 against 36 errors in 7,320 characters [measured on
the host, 2026-10-26; the same run's real development lines, which hold no counter: teacher_val 108 → 110,
devnative 112 → 104, whole dialogue 2,841 → 2,835 errors]. On a crafted counter line
(`host/tests/test_vocr_policy.c`) the 1 MB tier reads 「九月です」 where ctclm.2's 2 MB tier reads 「九ヶ月です」. The loss
follows the tier's new decoder settings, not its blob: the VOCR6 blob at ctclm.2's settings keeps the ヶ, and
ctclm.2's 1 MB blob at the new settings drops it (audit-vocr6-lm-rubi); vita-vn-ocr's retune of the settings for VOCR6
(#127) checks counters.

The measurements below are VOCR3's with ctclm.2's LMs.

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
| `vocr_lm6_o3_1000k.vlm` (default) | 982,320 (983,040) |
| `vocr_lm_o3_2000k.vlm` | 1,971,568 (1,974,272) |
| `vocr_lm6_o3_4000k.vlm` | 3,603,552 (3,604,480) |

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
  most the Large size's 2,478,080 B since API 42's head gate and two-level head, 2,473,984 B before; VOCR4-W20's was
  1,560,576 B),
  read and SHA-256 checked at the first doubtful line, or (test.3 on) by the first job that finds no
  line when its memory check takes the cascade, before any line is doubted: from then on the block is
  held whether or not a line is ever doubted (299-307 ms through the broker in test.2, once a game
  process), and kept like the job's weights. The job's workspace grows by
  the cascade's arena and decoder (VOCR5-L's 316,288 B beside VOCR5-N, VOCR5-N's 238,208 B beside
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
ocr_text_gate = off   ; on (the Rubi app's Skip OCR on screens without text): no lines where the gate sees no text
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
- **The request.** `VJO_GAME_OCR_S_GATE`, bit 4 of the request's `speed` word. The flags have one free bit, bit 15
  (API 27's vertical text had it; API 47's is in the `speed` word, bits 28-29). The Shell sets it in the user's jobs when
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

**Why it is still off by default (measured, API 43).** The owner's rule (2026-10-26): no OCR when there is no text,
with a default that loses no real line. Rubi's own job on the hooked page pool measured what the gate costs in
lines, and it does lose some:

`tools/gate_eval.py` runs `host/gate_eval.c`, the worker's job policy on the host (`core/vocr_ocr.c` with Rubi's finder,
the gate at its pinned point, VOCR5 Normal with the 1 MB LM and the cascade, PSP regions refined, a PSP page scaled
2x as Adrenaline shows it; measured before #307's head gate, on vita-vn-ocr `817d203` and VOCR5 v0.3.0's files: the
gate scores the finder's boxes, which #307 does not change), on every page of vita-vn-ocr's hooked page pool (`vocr6-ingest` `pagepool/v1`: 46 games,
30 Vita and 16 PSP, whose hook text was verified against the drawn glyphs; PCSG00007, PCSG00866 and PCSG00807 are left
out, as they share releases with CLANNAD and vita-test, or are the pool's test title), in three regions a page: its
own dialogue region, the one region a player would draw for the game (around the dialogue of almost every page), and
the whole screen (Rubi's region until one is drawn). A region the gate stops loses its verified lines; each stopped
region is read again without the gate, and a line counts as one OCR would have read when that reading matches the
region's hook text (similarity 700 or more) [measured, host; no console claim]:

| Region | Lines | Lines in regions the gate stops | of which OCR would have read |
| --- | ---: | ---: | ---: |
| Vita, the page's dialogue region | 52,909 | 14 (0.026 %) | 2 (0.004 %) |
| Vita, the game's one region | 54,767 | 363 (0.66 %) | 19 (0.035 %) |
| Vita, the whole screen | 59,702 | 624 (1.05 %) | 4 (0.007 %) |
| PSP, the page's dialogue region | 31,952 | 113 (0.35 %) | 55 (0.17 %) |
| PSP, the game's one region | 32,226 | 314 (0.97 %) | 44 (0.14 %) |
| PSP, the whole screen | 36,242 | 577 (1.59 %) | 46 (0.13 %) |

Most of the stopped lines are `……` (no letter: 189 of the 363 in the Vita games' one region) or one or two
characters (「え？」: 94 more). A lower threshold does not separate them: at -96, where 39 of those 363 lines are
still stopped, the gate stops only 5 of the 90 text-free regions (7 are left without a line, counting the 2 where
the finder finds none). On vita-vn-ocr's text-free development and holdout regions
(eval/linefind's 92, 90 with their screenshot here) it leaves 81 without a line, against 2 without it. Lines OCR would have read are lost in every kind of region, so
the gate is not the default: the owner's rule for a default is that it loses no real line, which every kind of
region fails on both platforms. (The rule written down before the run, the user region's raw rate of stopped lines
and at least 0.85 of the text-free regions stopped, fails too: 0.66 % on Vita, 0.97 % on PSP, and 38 of 45 Vita
text-free regions, 0.84. The lines-OCR-would-have-read column is a measure chosen after it: on Rubi's own
similarity, `core/hook_select.c`'s, the user region's are 0.005 % on Vita and 0.093 % on PSP [audit-rubi-ocr-skip's
recount].) **Skip OCR on screens without text** on the Rubi app's OCR & lookup page turns it on.
A better gate (vita-vn-ocr's VOCR6 spotter, whose text-presence score was not ready) would be measured the same way.

The cheaper check, an unchanged screen, is already in place and needs no setting: the kernel starts no background
read for a scene it has read (the result stands for the screen until its pixels change:
[auto-vs-press.md](auto-vs-press.md)), and a line whose pixels a job has recognized before is taken from the line
cache, not recognized again (`VjoVocrCache`, keyed by the line's pixels). What neither catches is a new screen
without text (a hidden box, a CG), whose junk "lines" the finder still finds: that is the gate's work.

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

## Vertical text (vita-vn-ocr #99)

```ini
ocr_vertical = auto   ; on: always read columns; off: always rows (Rubi 0.9.7 and older)
```

**What it reads.** Japanese set in columns, read right to left: a vertical dialogue box or NVL page (Yunohana
SpRING! on Vita, the Harukanaru Toki no Naka de titles on PSP, among the consoles' VNs vita-vn-ocr #99 counted) and
a manga's speech balloons in [NOBORU](https://github.com/Creckeryop/NOBORU) (a PS Vita manga reader). The Rubi app's
OCR & lookup page has the choice (**Vertical text**: OFF, Automatic, ON); it applies when the overlay next opens.

- **auto** (the default): a job of rows, as before, whose line finder's rows are checked first. Where they look like
  vertical text (below), nothing is read from them and the same job reads the region's columns instead, as `on`
  does. Elsewhere it is `off`'s job, bit for bit; with `ocr_line_fix` on (API 50), in a region at least as tall as
  wide, it reads the same text after finding the rows twice, first with the frozen finder for the check.
- **on**: every job of every game reads columns. Horizontal text inside the region is then read as columns of one
  character each: turn it back for horizontal games.
- **off**: every job reads rows, as Rubi did before.

Draw the text box (□) around the vertical text: the check and the columns are the region's. On a whole screen the
column finder takes a picture's edges and the menu for columns too, and auto keeps rows on the measured dialogue
screens (below); a screen or box of short rows lined up one under another can still look like vertical text to
it (limits, below).

**How a job reads columns** (`core/vocr_ocr.c`, a model whose `finder_mode` has `VJO_VOCR_FINDER_COLUMNS`; closed
draft #223's job, on today's main):

- **The columns.** vita-vn-ocr's column finder (`runtime/vocr_vertical.c`, `vocr_find_columns_rows`: GRAD run on the
  region's transpose, the same two passes over its rows; boxes wider than 5/4 of their height dropped, as
  horizontal text), in reading order: right to left, columns sharing a right edge top to bottom. Its variant P1
  (`VOCR_COLUMNS_VALLEYS`), which vita-vn-ocr #99 selected under `eval/vertical_protocol.json`'s finder rule, with
  `VOCR_COLUMNS_NO_DARK`: the finder reads no column's rows for its dark flag (a column spans every strip of the
  region), and the job computes the same flag (`vocr_line_dark`) on the pixels it fetches for the column. The finder's
  arena is the larger of the two finders': 83,619 B at 960×544 (GRAD's 83,323 B), under the model's 124,832 B.
- **Each column is a line.** It is fetched as a line is (from the strip, or, taller than a strip, as most columns
  are, through the tall-line block), turned 90° anticlockwise (`vocr_column_rotate`, its top to the left) into the
  job's rotation buffer, and from there it is the line: the blank-line check, the line cache, the helpers' copies,
  `vocr_run` (grey, inverted for dark text), the counter re-decode and the LM decoder take the turned strip. The
  column's box on the screen stays the line's geometry (`game vocr lines:` in the log), and its line flags carry
  `VJO_GAME_OCR_LINE_COLUMN`, from which the Shell names `VOCR5_V_w8.vocr` as the model that read the text (Anki
  cards, `history.csv`, the log).
- **The text** is the columns' readings right to left, one per line, as rows are top to bottom. The popup shows a
  column a line, and the word lookup, the cross-line join (`core/textjoin.c`: a full column joins the next one as a
  full row does), the arrows and the touch lookup work on it unchanged: they read the text, never the geometry.
- **Not for columns:** the refinement (`ocr_refine_lines`), the short-line probe (#194), the confidence cascade (no
  larger model reads columns) and the text-presence gate (`ocr_text_gate`: it scores row boxes). The LM (`ocr_lm`),
  the threads (`ocr_threads`), the line cache and the stable-text wait apply as they do to rows.

**auto's rule** (`core/vocr_ocr.c` `vjo_vocr_rows_vertical`, on the row finder's boxes, before any weights are
read). The row finder reads vertical text as rows one character tall, each a character of every column side by side,
stacked down the columns; horizontal dialogue gives a few lines, each many characters wide. The rows are vertical
text when either holds:

- the region is at least as tall as wide (a box drawn around columns), with 2 rows or more, 60 % of them at most 4
  times as wide as tall;
- 5 rows or more, 70 % of them at most 3.5 times as wide as tall and each with at least 80 % of its width inside
  the median span of those rows (their median left and right edges; of an even count, the upper of the two
  middle ones).

It is integer arithmetic on at most 16 boxes: no memory, no strip, a few hundred instructions. Then the job returns
`VJO_E_VERTICAL` to the game worker, which frees the try's blocks and runs the request again with the vertical model
(`game_ocr/main.c`, `again:`): its own memory check, a request by name's reserve, the column job. A vertical model
it cannot read (absent, short, failing its hash) or fit has the job read the rows instead; after a file it could not
read, the game's later jobs read rows unchecked. The tries are one job: they share its stable-text wait (at most
`ocr_settle_max_ms`, a press's `ocr_settle_press_ms`, over all of them, as the core's own tries do), so the
column try reads the region at once and recognizes once its rows have held still within what is left of the
wait; and the step log keeps each try's memory check, first pass and `find` step, but only the last try's
blocks (`alloc`, `free`), which keeps room in its 20 steps for the last try's own (audit of #335). The rule
was chosen on the hooked pages below, before it was checked on any other page; on the pages it was then
checked on it was not changed.

**The request (API 47).** `VjoGameOcrRequest.speed` bits 28-29 (`VJO_GAME_OCR_S_VERTICAL`): 0 rows, 1 columns, 2
auto; 3 is refused, as unknown bits are, by the kernel and by the game worker. The Shell sends the setting as it is
in the user's jobs (`ocr_vertical`'s values are the field's: `core/config.h` `VJO_VERTICAL_*`); the self-test's
stages and the Anki screenshot never carry it. A job of columns runs the vertical model alone, whatever its tiers,
with a request by name's 4 MiB reserve (the Small size's) and no smaller model to step down to, and opens it as
`VJO_GAME_OCR_OPEN_VERTICAL`, which the kernel's broker allows only for a request with a vertical mode, never by the
name a request gives as its model or fallback. With `on`, a vertical model that fails to open fails the job; no size
is left out of the game's ladder for it. API 27 (#213, reverted by #222) and draft #223's API 29 had this in the flags'
bit 15, which stays undefined.

**The model.** VOCR5-V (`VOCR5_V_w8.vocr`, vita-vn-ocr #99, pre-release
[v0.3.2-vertical.1](https://github.com/bee-san/vita-vn-ocr/releases/tag/v0.3.2-vertical.1)): VOCR5-S's shape
(819,792 B of int8 weights, W8I arena 124,832 B, 4,987 classes, 967,392 MACs per CTC frame), fine-tuned from VOCR5-S on
columns turned 90° anticlockwise. The package ships it beside the size models with its release's `ATTRIBUTION.md` as
`ocr/VERTICAL-ATTRIBUTION.md` (VOCR5-S's training data and terms, non-commercial use only, and its vertical data).
vita-vn-ocr #103 read its held-out vertical test set once (host, C runtime, greedy): 7.17 % CER on fixed VN columns and
11.29 % end to end on VN regions, 0.86 % and 2.45 % on manga.

**Measured through Rubi's job** (host, x86-64: `vjo-page-ocr`, the game worker's job in `core/vocr_ocr.c` on each
region with the weights kept, on config.ini's defaults: Normal size, the 1 MB LM (ctclm.2's `vocr_lm_o3_1000k.vlm`, the
default until #306: its VOCR6 LM was built with the hooked train titles' text, Yunohana SpRING!'s train pages among
them, so it would flatter these pages), the cascade within 2 s, the head
gate and two-level head (each where a blob has it: VOCR5-V's has neither), the counter re-decode, the text normalised
and filtered as the Shell shows it; `tools/vertical_eval.py`, on main `93647d3` and this change; #328's re-read
fixes since change no job of columns, as VOCR5-V has no head gate). Dialogue CER: every line
the job read but those over another text box of the page (a speaker's name tag), each normalised with vita-vn-ocr's
`eval/metrics.norm`, joined in the job's order, against the page's dialogue columns right to left, with 95 % intervals
from 10,000 page bootstrap resamples. Column recall and precision: found columns against the labelled ones at IoU ≥
0.5, one to one.

On **Yunohana SpRING!** (`PCSG00653`, VNDB v17343, PS Vita): 863 pages of the owner's hooked corpus (vita-vn-ocr #125,
vocr6-ingest `pagepool/v1`), with 1,611 dialogue columns and 20,791 characters whose labels are the game's own text
read by its hook (the corpus audit read 20 of its vertical lines: 0 wrong). VOCR5-V never trained on this VN: it is in
vita-vn-ocr #103's held-out vertical test cluster (VOCR5-S, its starting point, saw 2 horizontal lines of it).

| region (how the box is drawn) | off: rows, as today | on | auto (columns read) |
|---|---:|---:|---:|
| tight: the columns plus 12 px | 82.62 % | **4.51 %** [4.09, 4.95]; recall 0.972, precision 0.995 | 5.08 % (789 of 863) |
| drawn once: one box for every page whose text box is in the same place (2 places) | 83.63 % | **14.03 %** [12.58, 15.58]; recall 0.942, precision 0.687 | 13.58 % (741 of 764) |
| the text box and its speaker's name tag | 84.30 % | 23.37 % [22.06, 24.73]; recall 0.960, precision 0.727 | 23.64 % (837 of 863) |
| the whole screen | 96.04 % | 168.32 % | 97.55 % (41 of 863) |

A box drawn once has the box's empty edge and the name tag beside the columns: the finder takes some of their marks
for short columns (precision 0.69), whose readings add errors. On vita-vn-ocr #103's development regions (181: its
regions are a page's dialogue columns plus 12 px; PC pages scaled to 960×544, PSP frames shown 2× as Adrenaline shows
them, manga in NOBORU's fit-to-width view; scored the same way): `on` 12.29 % [8.80, 16.08] (PSP VN 1.37 %, PC VN
22.81 %, manga 4.59 %), `off` 88.16 %; auto read columns in 153 of the 181 regions (all 28 PSP, 47 of 56 PC, 78 of 97
manga), 29.32 % [20.01, 38.74] (PSP 1.37 %, PC 43.97 %, manga 23.60 %).

**On horizontal text, auto is off.** Its rule took no region for vertical text on 50,945 drawn boxes and 50,706 whole
screens of the hooked corpus's 44 other titles with pages, PS Vita and PSP (all of vocr6-ingest's pages but Yunohana
SpRING!'s and the three titles related to protected test titles), and took 1 of 2,837 PS Vita and PSP screenshots
from VNDB (vita-vn-ocr #99's census, no test VN or session), a whole PSP screen whose row finder had found a
portrait's blocks and no dialogue line. On a sample of 3,080 of the hooked regions (60 drawn boxes and 10 whole screens per title, seeded),
`auto`'s and `off`'s jobs gave main `cbaa4e3`'s text, lines, CTC frames, strips and workspace, every one of them
(and main `93647d3`'s, before #328; `off`'s gave main `213fd60`'s too, before the head gate). The rule took 310 ns a region on the host (median; 910 ns
at the 99th percentile), against a job's median of 55 ms there; on 600 of those regions, each read by main's job,
`off`'s and `auto`'s in turn and timed three times (`vertical_eval.py abtime`), the host's job times were within its
noise: `off` / main 1.0026 [0.9983, 1.0069] and `auto` / main 1.0039 [0.9998, 1.0080] (geometric means, 95 %
intervals). On the console the rule is a few hundred instructions a job (an estimate): nothing next to the job.

**Memory.** The worker's code, in every game that is ON whatever the setting: 4,096 B more module pages in each build
(184,320 B normal, 192,512 B debug: [memory-ledger.md](memory-ledger.md#vertical-text-api-47)). SceShell's and the
kernel's pages are as before. A job of columns takes the vertical model's weights (823,296 B in pages, a Small job's)
and a workspace of VOCR5-S's shape plus the rotation buffer (`VJO_VOCR_ROTATE_BYTES`, 57,344 B): 124,832 + 3,872
(the console's runtime structs, `VJO_VOCR_VITA_STRUCTS`) + 57,344 B, 188,416 B in pages at
960×544; with the LM extras, the greedy text and both helpers' slots 706,320 B on the host build (708,608 B in pages),
within `VJO_BUDGET_GAME_WORKSPACE` (1,220,608 B; `host/tests/test_ledger.c`). A
column whose crop holds more than the rotation buffer (wider than 105 px at the full 544-px height) is left out and
counted (`VjoVocrStats.rotate_skipped`, the bytes of the log's `find` step). auto's job that reads columns holds the
row job's blocks first, for the finder's pass only, and frees them before the column job allocates its own.

**Time (estimates; nothing ran on a console).** Per CTC frame VOCR5-V costs what VOCR5-S does: about 2.2 ms at 333
MHz and 1.7 ms at 444 MHz (VOCR5-S's box estimate, calibrated to VOCR3-H15's 4.94 ms a frame in v0.8.0-test.1's logs;
the console also measured up to 5.22 ms, about 6 % more). The capture costs more than for rows, as a column spans
every strip of the region and each strip read waits for the game's next frame. On the box drawn once (114 or 126 × 476
px, its two places, 2-3 columns), `on`'s job read a median of 27 strips (33 at the 90th percentile) and recognized 73 CTC frames:
about 0.45 s of capture at 60 fps (0.9 s at 30) and 0.16 s of recognition at 333 MHz. auto's reads the region twice
more first, for the row finder: 49 strips (56), about 0.8 s at 60 fps. Horizontal dialogue boxes read 5 strips
(median, the same with auto). On a whole screen of columns `on` read 64 strips. Draw the text box around the text.

**Off and auto are today's job** (`host/tests/test_vocr_vertical.c`): jobs of rows on a 5-line region and on #194's
box, greedy, with the LM, with two helpers and refined, give the same text, greedy text, frames, line stats, workspace
and strips as main `cbaa4e3` (before vertical text) gives, hash for hash, with auto's check and without it; the stage
benchmark's recorded job checksums are unchanged too. `test_game_vocr.c` runs the worker's auto retry, its fallback
to rows, and the request's checks; `test_shell_worker.c` the Shell's field and model name; `test_settings_app.c` the
app's card.

**Limits.**

- Nothing has run on a console: the accuracy, the capture time and the memory above are the host's and estimates.
- NOBORU's portrait mode, which turns the whole page on the screen (the Vita held upright), is not read: its columns
  reach the frame as rows of turned characters. Read in NOBORU's landscape view (fit to width).
- With `on`, horizontal text in the region (a speaker's name box, a menu) is read as columns of one character.
- Furigana beside a column is dropped when the finder takes it for a narrow column (under 55 % of the median
  column's width), and read with the column when it merges into it.
- auto misses vertical text whose finder rows do not look like columns (9 % of the tight boxes above, 16 % of
  vita-vn-ocr's development regions; then rows are read), and a whole screen is mostly read as rows. Set `on` for a
  vertical game.
- auto takes for vertical text some horizontal layouts its measured pages do not have: 5 or more short rows lined
  up one under another (a game's menu of short items at 24 px, a list of names, a status panel; on a whole screen
  or in a box) and a tall box around 2 or more short lines. On such a screen it reads columns, which gives no
  sensible text, and reads the vertical model's weights in place of the size's. Found on constructed screens
  (generic words in a free font, the row finder and the rule of this build; audit of #335): the rows of vertical
  text it fired on in Yunohana SpRING! overlap (the median region's median gap between rows: -0.07 to -0.13 of a
  row's height), those constructed rows have gaps of 0.33-0.6, a difference a later rule could use (its cost in
  recall unmeasured). Set `off`
  for a game of such screens that never sets text in columns.
- The self-test's stages (the debug build, [self-test](self-test.md)) read rows whatever the setting.
- With `auto`, a region that holds vertical text in some screens and rows in others reads the other model's weights
  (0.8 or 1.2 MB, from the card) at each change, and a job of columns gives back the confidence cascade's
  resident model (Normal's is VOCR5-L, 2.4 MB), which the next job of rows reads again at its first doubtful
  line.
## Head gate and two-level head (API 42)

```ini
ocr_head_gate = on    ; on (both, the default) | gate | two_level | off (the full head on every frame, as before)
```

Two speed-only changes from vita-vn-ocr #117, each adopted there under its pre-registered rule: the **head gate**
(vita-vn-ocr #120, `docs/headgate.md` there; all three sizes) and the **two-level head** on top of it (#122,
`docs/head2level.md`; Normal and Large). `ocr_head_gate` sets them: `on` both, `gate` or `two_level` one, `off`
neither. Its value is two bits, `VJO_HEAD_GATE` and `VJO_HEAD_TWO_LEVEL` (`core/config.h`), which the Shell sends as
they are in the request's speed word, bits 18 and 19 (`VJO_GAME_OCR_S_HEAD_GATE`, `VJO_GAME_OCR_S_TWO_LEVEL`), on
each of the user's OCR jobs. One key, not two: the normal Shell's code had 100 B of its last page left on main, and a
second key cost more than that (`docs/memory-ledger.md`, "Head gate and two-level head"). The self-test's stages leave
both off, as they do the text-presence gate, so their reports compare with older ones; `same_pipeline` compares the
setting, so a changed value voids the scene's text. The Rubi app has no switch for it, as it has none for the other
OCR pipeline settings (`ocr_text_gate`, `ocr_refine_lines`).

The game worker plans every recognizer of a job with what the request asks for **and its blob carries**
(`core/vocr_ocr.c` `run_opts`: `vocr_has_gate`, `vocr_has_two_level`): the job's own lines, its helpers' and the
confidence cascade's re-reads. A blob without a gate section, or without a two-level trailer, reads as before, so a
model asked for by name (VOCR3, VOCR4, v0.2.0's) is unchanged, and Small, whose blob has no trailer, runs the gate
with `on` or `gate` and the full head with `two_level` or `off`. Nothing in the code names a model for either path:
each blob's own sections (below) drive them. The line cache keeps the entries of each setting apart (bits 30 and 31 of `cache_variant`).

- **The head gate.** The recognizer reads a line as CTC frames, about three in five of them blank, and its output
  head (96 inputs to 4,987 or 5,631 classes) is 15-40 % of a frame's time. A tiny gate on the head's input, 96 → 16 →
  1, says "blank" before the head runs; the runtime then skips the head on that frame and gives it only the blank (its
  blank logit is the head's own, so the confidence and the top-K's blank stay exact). The other frames read exactly
  as without it. Each size's blob carries its own gate, fit on that model's own frames of training-side lines
  (nothing was retrained): a 1,792 B `VOCG` section after the version-1 blob, which a runtime without the gate
  ignores. On vita-vn-ocr's development data it skips 61.4 / 61.0 / 57.9 % of the frames of finder-cut dialogue lines
  (Small / Normal / Large) and 0.16-0.19 % of the frames that are not blank; no development score was resolved worse
  greedy, with this overlay's beam + 1 MB LM and counter re-decode, or with its cascade (every 95 % upper bound at most
  +0.050 points, the rule's margin; Normal and Large with room, Small at the rule's edges). vita-test, psp-test and
  CLANNAD, read once afterwards: greedy dialogue text unchanged on the test sets, dialogue CER −0.043 to +0.006
  points, 0-3 lines changed per size.
- **The two-level head.** Most CTC frames that are not blank read one of a few hundred common characters, yet the
  head computes all 5,631 rows on every frame the gate keeps. Normal's and Large's blobs have their head rows and
  vocabulary in frequency order (every class's logit the same) and a 1,984 B `VOC2` trailer: level 1, the blank and
  the 503 most frequent classes (504 rows), runs on every kept frame, with a tiny escape (98 → 16 → 1, on the head's
  input and level 1's two largest logits) that says whether the frame may need a rarer character; only those frames
  run the other 5,128 rows. A frame it picks reads exactly as the full head; one it does not reads over level 1's
  classes. The escape fires on 21 % (Normal) and 13 % (Large) of the frames the gate keeps, 8 % and 5 % of all
  frames. Against the gate alone the development score is +0.000 [+0.000, +0.000] (Normal) and +0.006 [−0.006,
  +0.022] points (Large) with this overlay's beam + LM and counter re-decode, and rare-character recall 0.06 / 0.09
  points lower than today's (95 % upper bounds 0.15 / 0.24). vita-test and psp-test, read once afterwards: no line changed beyond the gate's;
  CLANNAD's dialogue: Normal one more line on the cascade (+0.003 points), Large 3-5 lines where the gate alone
  changes 0-1 (+0.003 to +0.009 points). Small's was not adopted (vita-vn-ocr `docs/head2level.md`, "Why not Small").
- **Time (estimates).** The gate: −122 / −137 / −163 ms per nominal 3-line box at 333 MHz on one core without
  prefetch credit (−19.5 / −14.9 / −7.9 %; −92 / −103 / −122 ms at 444 MHz), −45 / −51 / −60 ms on a real 89-frame Vita
  box. The two-level head, beyond the gate's: −62 / −94 ms (Normal / Large) at the development share of frames that
  escape (−32 / −66 ms as measured on 24 development lines, which escape 3-5 times as often; −47 / −71 ms at 444 MHz
  by the clock ratio, −24 / −50 ms on the measured basis, vita-vn-ocr `docs/head2level.md`'s 444 MHz column). The
  gate's 444 MHz figures are by the clock ratio too.
  Both from qemu instruction counts of the Vita build calibrated to VOCR3-H15's measured console rate. The sizes' times
  in the Rubi app (`tools/model_tiers.json`) subtract them: 0.51 / 0.72 / 1.82 s at 333 MHz (0.63 / 0.92 / 2.08 s with
  `ocr_head_gate = off`). No console has run either.
- **The counter re-read.** [Counters](#counters-a-numeral-or-a--the-line-lost-vita-vn-ocr-109) look at the top-K of
  blank frames beside a numeral they suspect and before a line's first bracket, and a skipped frame offers only the
  blank there; and the escape can miss a small ヵ or ヶ, a rare row, which then reads as the full-size カ or ケ. So a
  line whose run skipped something (frames the gate skipped, or with the two-level head frames that ran level 1 only)
  is read again with the full head (`vocr_extra.ungated`, `core/vocr_ocr.c` `read_again`) when its counter re-decode
  finds a counter context or, with the two-level head, its recognizer's reading holds a full-size カ or ケ before 月,
  所, 年, 国 or 条; that reading is the line's: its re-decode, its LM decode, its text and its confidence. This is the
  rule vita-vn-ocr's `eval/head2level_dev.py counter` scores for the two-level head (for the gate alone, the counter
  context alone, as `docs/headgate.md` has it); it applies to the job's lines, the cascade's re-reads and, without
  the LM's top-K (no LM and no cascade: no counter re-decode), to the two-level head's kana lines still. A re-read
  whose text does not fit the room left in the job's text keeps the gated reading: the line is read gated once more
  (the same bits). On real development lines the counter-context trigger read nothing again (0 in 8,233 reads per
  size, `docs/headgate.md`; vita-vn-ocr does not report the kana trigger on development lines, and this overlay's
  1,074 development jobs with `on`, below, read none again); on vita-vn-ocr's synthetic counter set the re-read
  leaves 76 (Normal) and 18 (Large) of 3,969 lines worse with the gate (96 and 32 without it; Small 14), nearly all
  of them lines cut 2-10 px into their first character, which the finder's 3 px margin avoids (Large: 2 of its 18 at
  the ink edge), and the two-level head 3 and 1 lines more.
- **Off is today's output.** With `ocr_head_gate = off` every recognizer reads with the full head on every frame.
  Small's blob is v0.3.0's bytes and its section, so its output is v0.3.0's. Normal's and Large's give every class
  v0.3.0's logit bit for bit, the same class text at another class number, and the greedy text, the top-K, the LM
  decode and the counter re-decode map classes by text, so their text is v0.3.0's **except on exact ties**, which
  every decoder breaks by class number: of two classes' logits (the first maximum, the top-K's order and its last
  place, which the beam follows) or of the counter re-decode's integer numeral costs (vita-vn-ocr
  `runtime/vocr_post.c`: two numerals within 1/1024 nat of each other cost the same, and the frequency order puts 361
  of the 666 pairs of its 37 numerals the other way round: audit-vocr6-rubi-gate F2 built such a line). None occurred
  on real lines: on the 759 line boxes of the development regions below (27,556 frames a size) every top-8 logit, its
  classes' texts and order, the confidence, the counter re-decode and the beam + 1000k LM's text were v0.3.0's at
  every size, with no exact tie and no counter context [measured by audit-vocr6-rubi-gate on the host].
  `host/tests/test_vocr.c` `the_off_path_reads_v030s_logits_bit_for_bit` checks every logit of synthetic lines against
  v0.3.0's blobs (316 frames a size, 0 tied maxima).
  The game worker's whole job (`vjo-ocr` in its worker form: blank lines skipped, the beam + 1000k LM, the counter
  re-decode, the cascade at confidence 700, the refinement on PSP regions), run on the 358 regions of vita-vn-ocr's
  linefind development manifests whose screenshots are on the build host (192 Vita, 166 PSP; no protected set), gave
  main's text, CTC frames and every count at every size with `off` [measured on the host: main `4bc2604` with
  v0.3.0's blobs against this change; only the cascade's file name differs]. With `on` the text of 43 / 24 / 21 of
  those regions changes (Small / Normal / Large) and the gate skips 63.2 / 62.6 / 60.8 % of the jobs' frames; the
  regions carry no reference text, so this says nothing of accuracy (vita-vn-ocr's scores above do).
- **Memory** (measured: both Vita builds, [memory-ledger.md](memory-ledger.md#head-gate-and-two-level-head-api-42-vita-vn-ocr-120-and-122)).
  The game worker takes two text pages more in each game that is ON (180,224 / 188,416 B in the normal / debug
  build, 8,192 B more each): the runtime's two paths (`vocr.o` +5,037 B) and the job's re-read. The Shell's pages
  are unchanged (the key's name and values live in its data segment). Normal's and Large's blobs are 3,776 B larger
  than v0.3.0's, a weights page more each (1,191,936 / 2,478,080 B, the ledger's weights and cascade budgets); Small's
  1,792 B stay in its block. The arenas are 48 B (Small) and 1,504 B (Normal, Large) larger, the largest job's
  workspace 6,400 B (1,218,048 B on the host build, two pages more: `VJO_BUDGET_GAME_WORKSPACE` 1,220,608 B), and a
  first Normal or Large job takes 4,096 B more; `VJO_LIMIT_GAME_JOB` is 10,604,544 B. The helper threads' deepest
  stack chain is 2,528 B of 16 KiB (2,392 B before; the two-level path's 2,440 B). No new block, thread or
  allocation site.
- **Logs.** The job's `recognize` step logs the share of its CTC frames the gate skipped as its kind, in percent
  (`gstep job recognize kind=61 arg=3 ... bytes=240`: 61 % of 240 frames); `kind=-` without the gate or when it
  skipped none. `tools/analyse_selftest.py --collect` sums it (`head gate: N of M job(s) skipped frames: at least F of
  T CTC frames`). `VjoVocrStats.head_gated`, `head_escaped` and `head_rereads` count the frames the gate skipped, the
  frames that ran the rare rows and the lines read again; `vjo-ocr --head-gate on` and `--two-level on` run them on the
  host as the worker does and print them.

### A new pin set (VOCR6)

The paths are the blobs': a generation whose S/N/L blobs bring their own `VOCG` gate and `VOC2` trailer (each fit on
that blob's own outputs: vita-vn-ocr `train/headgate.py`, `train/head2level.py`, under #120's and #122's rules)
changes no code here, only pins. A size whose new blob has no gate or no trailer simply runs without it. A new set
updates:

1. `include/vjo_vocr_sizes.h`: `VJO_VOCR5_{S,N,L}_NAME` and `_BYTES` (or the new generation's names, with
   `VJO_VOCR_TIERn_*` pointing at them); `VJO_VOCR_MAX_BYTES` follows the largest; `VJO_VOCR_VITA_STRUCTS` when the
   runtime's structs change (the Vita build checks it). `core/vocr_ocr.h` `VJO_VOCR_DEFAULT_MODEL`: the Normal file.
2. `core/vocr_ocr.c` `models[]`: each blob's SHA-256 and its W8I arena with both paths on (the planning
   `vocr_arena_bytes` gives with `VOCR_HEAD_GATE_ON` and `two_level = 1` where the blob has them;
   `host/tests/test_vocr.c` `pinned_table_matches_release_files` checks it against the files).
3. `kernel/game_ocr.c` `model_size()`: the broker's names, each by its macro (a new generation's macros are an edit
   here, and the kernel plugin is rebuilt in any case). `python3 tools/docs_model_names.py` then rewrites the names'
   lists in this file and the guide from the kernel's (`tools/tests/test_docs_model_names.py`, in ctest, fails until
   it has).
4. `core/vocr_models.c`: the generation, the box times (estimates with the paths on) and the job bytes;
   `tools/model_tiers.json` the same values (`tools/tests/test_model_tiers.py` checks they agree) and
   `host/tests/test_settings_app.c` `DETAILS`, the size buttons' strings made of them.
5. `tools/prepare_ocr.py`: the release tag, each asset's URL, size and SHA-256, the attribution's pin and
   `VOCR_GENERATION`; the host tests' pins (`host/tests/test_tier_pins.h`, `tools/tests/test_vocr_defaults.py`
   `TIER_PINS`, the release tag in `tools/tests/test_package_models.py`) and the self-test fixtures
   (`vjo-selftest-writer DIR NAME`). `host/tests/test_vocr.c` `the_off_path_reads_v030s_logits_bit_for_bit` compares
   the pins with v0.3.0's blobs: point it at the blobs the new ones are derived from, or retire it.
6. `cmake/VocrOCR.cmake` and the three checkouts in `.github/workflows/release.yml` (the host, arm and vita jobs):
   the runtime commit and each runtime file's SHA-256, when the runtime changes.
7. The memory ledger (`include/vjo_ledger_budget.h`): the weights and cascade budgets (the largest blob,
   page-rounded), the workspace (`host/tests/test_ledger.c`'s largest job, page-rounded), the worker's pages if the
   runtime grows, and `VJO_LIMIT_GAME_JOB`, each raise justified ([memory-ledger.md](memory-ledger.md)). The normal
   Shell's code had 28 B of its last page left after API 42: a pin set adds no Shell code.
8. The docs that name the files, their sizes or blocks: this file's size table, models and attribution, [OCR
   speed](ocr-speed.md), the guide's sizes and its names' list, [memory-ledger.md](memory-ledger.md),
   [vita-memory.md](vita-memory.md), [offline-testing.md](offline-testing.md),
   [settings-readability.md](settings-readability.md), [anki.md](anki.md), `README.md`'s credits and the CHANGELOG.

## Line finder fixes (API 50)

```ini
ocr_line_fix = on   ; off: the frozen finder; junk: also drop frame strips (regions with a margin); all: also split touching rows
```

The finder cuts a region into lines before anything is read, and most of the whole-dialogue errors come from that
cut, not from reading (vita-vn-ocr #119). bee-san/vita-vn-ocr#129 measured it on the owner's hooked corpus (#125),
46,805 native PS Vita and PSP pages whose line boxes and text come from the game itself. It found three common
failures and fixed each behind a flag in `runtime/vocr_lines_fix.c` (vita-vn-ocr `docs/finder-fix.md`):

| flag | what | `ocr_line_fix` |
|---|---|---|
| `VOCR_FIX_COLGAP` | a line's parts across a full-width space (after ！ or ？) stay one line; the frozen finder kept only the heavier part and read the rest as a line of its own, or not at all | on, junk, all |
| `VOCR_FIX_JUNK` | a line on the region's top or bottom edge under 70% of the tallest other line's height (a frame strip, a cut neighbour), and a line spanning the region that repeats itself (an ornament), are dropped | junk, all |
| `VOCR_FIX_VALLEY` | a band of two touching rows (heavy outlines, tight leading) is split at its valley | all |

`on` is `VOCR_FIX_DEFAULT` (COLGAP), the step #129's pre-registered rule adopted and that holds whether a region is
drawn with a margin or tight. `off` is the frozen finder, bit for bit (vita-vn-ocr's `make check-lines-fix`). The
flags reach the finder in `VjoVocrPolicy.line_fix`.

**What it does, measured in vita-vn-ocr.** These are host numbers through Rubi main's path (`eval/pagepath.py`, #131),
with VOCR5-N W8I, the 1 MB LM, the cascade, `core/textnorm.c` and the line filter, in the console view (PSP frames
2×). Every figure is whole-dialogue CER with a 95% cluster-bootstrap interval.

| regions | `off` (Rubi before) | `on` | `junk` |
|---|---:|---:|---:|
| hval: other pages of 46 hooked train titles, drawn with a 12-20 px margin (6,327) | 3.875% | **3.292%** (−0.583 [−0.926, −0.309]) | 1.898% |
| development: 93 other VNs (1,161) | 3.036% | **2.340%** (−0.696 [−1.262, −0.249]) | 2.325% |
| hval, cut tight at the lines' union (2,109) | 2.570% | 1.933% (−0.637 [−1.022, −0.340]) | 2.391% |
| development, cut 1 px around the ink (387) | 3.110% | 2.123% (−0.986 [−1.852, −0.362]) | 2.727% |
| the hooked test title, read for report only | 1.241% | 0.978% | 0.978% |

VOCR5-S and VOCR5-L move the same way with `on`: hval 3.22% → 2.64% and 3.92% → 3.34%.

`junk` is not the default.
- **Where it helps.** On regions drawn with a margin it removes frame strips and ornaments: −1.395 points on hval.
  Most of that is one game whose dotted ornament reads as text.
- **Where it hurts.** On regions drawn tight, as this guide recommends, it drops real dialogue lines: tight hval
  +0.458 [−0.007, +1.451] and tight development +0.603 [0.000, +1.938] against `on`. The cases are a textured box,
  an edge line shorter than a furigana line, and an inline name.

Use `junk` for a game whose box frame or ornament is read as text, with a region drawn a few pixels outside the
text. `all` is `junk` plus a split of two touching rows. On regions drawn with a margin it is the best of the four
(hval 1.505%, development 1.497%), and it fixes Narcissu (13.65% → 2.76% on hval). But it makes two development
games worse than `on` (v10972 38 → 70 errors, v6985 6 → 7), and it carries `junk`'s loss on regions drawn tight
(not measured for `all` itself), so it is opt-in too.

- **The request.** `VJO_GAME_OCR_S_LINE_FIX` (speed bit 21), `VJO_GAME_OCR_S_LINE_JUNK` (bit 22) and
  `VJO_GAME_OCR_S_LINE_VALLEY` (bit 23). Bit 17 is closed draft #271's; bits 18 and 19 are the head gate's and the
  two-level head's (API 42), and bit 20 is trusted hooks' (API 43). The Shell sets them in
  the user's jobs; the self-test's stages and shots never carry them, so the self-test reads with the frozen finder
  as before.
- **Memory.** There is no new allocation. The fixes use the finder's own arena (`vocr_lines_arena_bytes` is
  unchanged), with no heap and no recursion; the deepest call chain is 8 B deeper (vita-vn-ocr's audit). The worker
  links `vocr_find_lines_fix_rows` in place of `vocr_find_lines_rows`: +1,256 B of code at -O2 (vita-vn-ocr's
  measurement, VitaSDK gcc 15.2.0, Thumb-2). The module pages are given in the PR and checked by the memory ledger.
- **Time.** On the host the finder takes the same time with `on` (426 / 424 µs per hval region, best of 7). `on`
  reads 0.6-0.7% more CTC frames on hval and development (host, measured), because a line's parts stay together.
  What that costs on the console is an estimate; nothing of it has run on a console.
- **The host tool.** `vjo-ocr --line-fix on|junk|all` reads a region as the worker does with the setting.

**To test on console.**
- A line with a full-width space after ！: one line with `on`, two parts with `off`.
- A box whose frame or ornament touches the region, with `junk` and a margin.
- A Narcissu page with `all`.
- The job time with the setting on and off.

## Models

**The default size, Normal, is VOCR5-N (`VOCR5_N_w8g2.vocr`); Small is VOCR5-S
(`VOCR5_S_w8g.vocr`) and Large VOCR5-L (`VOCR5_L_w8g2.vocr`).** They are vita-vn-ocr's VOCR5
recognizers (`eval/vocr5_protocol.json` and `docs/vocr5.md` there), one shape per size,
published as the [v0.3.0 release](https://github.com/bee-san/vita-vn-ocr/releases/tag/v0.3.0),
each with its head gate appended since API 42: v0.3.0's bytes unchanged, then a 1,792 B
section (the [v0.3.5-headgate.1 pre-release](https://github.com/bee-san/vita-vn-ocr/releases/tag/v0.3.5-headgate.1)),
and Normal's and Large's with their two-level head too: their head rows and vocabulary in frequency order (every
other byte v0.3.0's but the CRC), the same gate and a 1,984 B trailer (the
[v0.3.6-head2level.1 pre-release](https://github.com/bee-san/vita-vn-ocr/releases/tag/v0.3.6-head2level.1), which
carries Small's gated file too, byte for byte; [Head gate and two-level head](#head-gate-and-two-level-head-api-42)).
VOCR5-S keeps FL10's topology and arena with a 4,987-class head, VOCR5-N H15's with a
5,631-class head, and VOCR5-L is a late-wide shape (channels 64, 128, 256, 480, 576) with
5,631 classes. A first full-screen job of Small takes 163,840 B more than one of VOCR3-FL10
(1,019,904 against 856,064 B: the larger head's weights), Normal 32,768 B more than VOCR3-H15
(1,445,888 against 1,413,120 B; 1,441,792 B with v0.3.0's blob, before API 42's two-level blob took its weights
block one page more) and Large 942,080 B more than VOCR4-W20 (2,813,952 against 1,871,872 B; 2,805,760 B with
v0.3.0's blob: its weights block and its workspace each take one page more since API 42), for
which the memory ledger's weights and cascade budgets are raised (`include/vjo_ledger_budget.h`).

| file | bytes | sha256 | W8I arena | release |
|---|---|---|---|---|
| `VOCR5_N_w8g2.vocr` (Normal, the default) | 1,189,408 | `f0c8e52c9c1d9f2a714d5ca3ecfba9c0b031ee7ac51a522a2c29fd558d51761b` | 184,448 | v0.3.6-head2level.1 |
| `VOCR5_S_w8g.vocr` (Small) | 821,584 | `76a1bc5de5cd0419a7edad31bc198af8016bda5592a7e595de75697671d3e928` | 124,880 | v0.3.6-head2level.1 (v0.3.5-headgate.1's file) |
| `VOCR5_L_w8g2.vocr` (Large) | 2,477,472 | `41c3543ff8dddd45dc953a6a5f5d579ef9ca779f0e43196406be59c1d985c5b5` | 262,528 | v0.3.6-head2level.1 |
| `VOCR3_H15_w8.vocr` | 1,155,968 | `cb3c61a8f91f9aeeb3efcc0e79313f664112f25c4bd55865bae9df52522b360c` | 182,944 | v0.2.5-vocr3.1 |
| `VOCR3_FL10_w8.vocr` | 657,664 | `87fb4d0c18416f5f3771cecd274c8db3cf69b8b4a4ee9f204edebb4b82079154` | 124,832 | v0.2.5-vocr3.1 |
| `VOCR4_W20_w8.vocr` | 1,558,016 | `2745e69699ad7b2aba3092f83e5ddc1912f405f54b9df8a2e55be82c385b716e` | 241,056 | v0.2.5-vocr4.1 |
| `VOCR5_V_w8.vocr` (vertical text, `ocr_vertical`) | 819,792 | `ccd20c63bb2620f054538b456904bdf19b74d483d3da8de865009e282bb04172` | 124,832 | v0.3.2-vertical.1 |
| `H15_w8.vocr` | 1,155,968 | `316ebc377ec34520440991c5085199d8e74031f01a867938be90a60c1e3f03cd` | 182,944 | v0.2.0 |
| `FL10_w8.vocr` | 657,664 | `500e0b842e5f0cb326f55e7d991a52ab66dd24977ccc89bd188228aedc825e84` | 124,832 | v0.2.0 |

The arenas are those with `ocr_head_gate = on`: the head gate's 48 B more than v0.3.0's, and for Normal and Large
the two-level head's 1,456 B more (with `off` the blobs plan v0.3.0's arenas). VOCR5's scores are in vita-vn-ocr's
`docs/vocr5.md` (its locked report), the gate's and the two-level head's in its `docs/headgate.md` and
`docs/head2level.md`. No console has run a VOCR5 model yet.

**Their attribution and terms.** v0.3.0, v0.3.5-headgate.1 and v0.3.6-head2level.1 ship an `ATTRIBUTION.md` (in their `SHA256SUMS`), which
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
the recognizers `VOCR5_N_w8g2.vocr`, `VOCR5_S_w8g.vocr` and `VOCR5_L_w8g2.vocr`
(VOCR5, release v0.3.6-head2level.1); `VOCR3_H15_w8.vocr` and
`VOCR3_FL10_w8.vocr` (VOCR3, release v0.2.5-vocr3.1); `FL10_w8.vocr`,
`H15_w8.vocr` and `F20_w8.vocr` (VOCR1, release v0.2.0); `VOCR4_W20_w8.vocr`
(VOCR4, release v0.2.5-vocr4.1); `VOCR5_V_w8.vocr` (VOCR5, release
v0.3.2-vertical.1); and the character LMs `vocr_lm_o3_500k.vlm`,
`vocr_lm6_o3_1000k.vlm`, `vocr_lm_o3_2000k.vlm` and `vocr_lm6_o3_4000k.vlm`
(release v0.3.7-lm6.2)<!-- end of the names -->.
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
git -C /path/to/vita-vn-ocr checkout f8a1856c1b21bf41af744c159b54ce8203408a89  # #135's merge (after #130's, #123's, #122's, #120's, #112's, #111's, #101's, #85's, #83's and #82's): the line finder fixes, the attention op (unused by the pinned blobs), the two-level head, the head gate, the counter re-decode, the text-presence gate, the faster kernels, the refinement, the beam + LM decoder

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

`cmake/VocrOCR.cmake` checks the sixteen runtime files (`runtime/vocr.c`, `vocr.h`,
`vocr_lines.c`, `vocr_lines.h`, `vocr_lines_rows.c`, `vocr_lines_rows.h`, `vocr_lm.c`,
`vocr_lm.h`, `vocr_lines_refine.c`, `vocr_lines_refine.h`, `vocr_textgate.c`, `vocr_textgate.h`, `vocr_post.c`,
`vocr_post.h`, `vocr_vertical.c`, `vocr_vertical.h`: vita-vn-ocr #103's column finder, the same file at the pin) against their
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
   python3 tools/prepare_ocr.py --backend vocr-tiers --output ocr    # the 3 sizes' models, MODEL-ATTRIBUTION.md,
                                                                     # the gate's 2 files, VOCR5_V_w8.vocr, VERTICAL-ATTRIBUTION.md
   python3 tools/prepare_ocr.py --backend vocr-lm --output ocr       # the 4 LMs and ATTRIBUTION.md
   python3 tools/prepare_ocr.py --backend vocr --model VOCR4_W20_w8.vocr --output ocr   # by name: the host tools' cascade
   python3 tools/prepare_ocr.py --backend vocr --model VOCR3_H15_w8.vocr --output ocr   # by name: an earlier one
   ```

   The downloads need no GitHub account: vita-vn-ocr's releases are public, and the
   GitHub CLI (`gh auth login`) is only tried when a public download fails (a draft
   release). `--from FILE` checks a copy downloaded another way.
   `tools/package_release.py --models ocr` needs the first two lines' files: it ships
   the LMs with their attribution as `ocr/LM-ATTRIBUTION.md` (CC BY-SA 4.0), the
   sizes' models with theirs, v0.3.6-head2level.1's `ATTRIBUTION.md`, as `ocr/MODEL-ATTRIBUTION.md`
   (kept under that name in `ocr/` too, beside the LMs' `ATTRIBUTION.md`), the text-presence
   gate's `textgate_p8_c8_w8.vtd` with v0.3.2-textgate.1's `ATTRIBUTION.md` as `ocr/GATE-ATTRIBUTION.md`,
   and the vertical model `VOCR5_V_w8.vocr` with v0.3.2-vertical.1's `ATTRIBUTION.md` as
   `ocr/VERTICAL-ATTRIBUTION.md`.
3. Copy the files to `ux0:data/VitaJPOverlay/ocr/`, the attributions included, or let the
   FTP installer do it: `tools/install_ftp.sh --vocr-model ocr/VOCR5_N_w8g2.vocr --set vocr_tier=normal VITA_IP`.
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
9. **Head gate and two-level head (API 42).** With `ocr_head_gate = on` (the default), read ten dialogue lines
   at each size and note the `speed` line's microseconds per frame and each job's `gstep job recognize kind=`
   (the share of frames the gate skipped; vita-vn-ocr's development share is 57-61 %). Set `ocr_head_gate = off`,
   reload the settings, read the same lines and compare the text shown and the time per frame: the estimate is
   about 21 / 16 / 11 % less per frame with the gate (Small / Normal / Large, on lines where it skips 49-57 % of
   the frames; vita-vn-ocr `docs/headgate.md`) and, for Normal and Large, an estimated 3.5-6.8 % and 3.2-4.5 % of a
   box's time less again with the two-level head (`docs/head2level.md`); `gate` and `two_level` turn on one each. Any line whose text differs between `on` and
   `off` goes in the report with its photo. A line such as CLANNAD's 「九ヶ月です」 should read the same every way
   (the counter re-read), and no job should be refused for memory at your usual free memory (a first Normal or
   Large job takes 4 KiB more).
10. **Line finder fixes (API 50).** With `ocr_line_fix = on` (the default):
    - **Start and memory.** A game that is ON still starts; the worker holds 8 KiB more of it than before API 50. No
      job is refused for memory at your usual free memory (`game vocr memory:` shows `check passed`).
    - **A line with a full-width space** after ！ or ？: it reads as one line. `game vocr lines:` lists one crop
      for it, and the shown text has the whole line. With `off` (reload the settings) the same line gives two
      crops, or loses its second part. Photograph the shown text both ways.
    - **`off` is the frozen finder.** On five other lines, the `game vocr lines:` crops with `off` are those of a
      build before API 50 (test.19, API 47).
    - **`junk`.** A box whose frame or ornament touches the region, with the region drawn a few pixels outside
      the text: with `on`, the strip shows up as a crop of its own in `game vocr lines:` (and maybe as stray
      text); with `junk` that crop is gone and the dialogue lines are unchanged.
    - **`all`.** On a Narcissu (PSP) page, the two touching rows give two crops in `game vocr lines:` and the
      shown text both lines.
    - **Vertical text.** In a vertical game with `ocr_vertical = auto`, a box drawn around the columns and the
      speaker's name still reads as columns (the log's find step of kind 2, the lines marked as columns).
    - **Time.** Note `find=` and `recognize=` in the `game vocr rc=` line with `on` and with `off` on the same
      five lines: the finder's time should not change, and recognition only by the frames of the joined parts.

## Limits

- Vertical text is read only as [vertical text](#vertical-text-vita-vn-ocr-99) says: in a region drawn around
  the columns, by `ocr_vertical = auto` or `on`; no console has run it.
- Adjacent lines 1-2 px apart can merge on PSP-sized text, and decorated
  frames can add a stray line; tight regions read best. vita-vn-ocr's learned
  text detector (its #97) aims at these and at text-free regions; Rubi does not
  use it for now ([why, and what would change that](text-detector.md)).
- Recognition runs on low-priority threads in the enabled game process. While
  a job recognizes, the ARM clock is 444 MHz (`ocr_boost_clock = on`, the
  default; a clock already at 444 MHz or more is left as it is) and is put
  back after ([OCR speed](ocr-speed.md#the-arm-clock-ocr_boost_clock-on)); the
  bus and GPU clocks and the memory mode are never changed.
- Only the pinned files listed under [Models](#models) load. A new release needs its sizes, hashes
  and generation added (`core/vocr_ocr.c`, `core/vocr_models.c`, `kernel/game_ocr.c`,
  `tools/prepare_ocr.py`); `python3 tools/docs_model_names.py` then writes the new names into those
  lists and the guide's.
