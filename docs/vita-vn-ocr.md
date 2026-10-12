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

Select **VOCR2** on the Settings app's OCR & lookup page, or set
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

1. The kernel captures the selected region (single pass). The game worker reads each
   row once, converting it to grey (`(77 R + 150 G + 29 B) >> 8`, the luma both
   the finder and the recognizer use), so the kernel frees its capture buffer
   before recognition starts.
2. The polarity-agnostic line finder (`vocr_find_lines`, gradient/contrast
   projection) finds up to 16 lines. Unlike the PP-OCRv5 backend's bright-pixel
   finder it reads dark text on light boxes, outlined text and coloured names,
   and never rejects a region. A region with no line ends here: the weights
   file is not read.
3. The existing kernel worker opens the pinned model from
   `ux0:data/VitaJPOverlay/ocr` and reports its length and modification
   time. The first job, and any job after the file changed, copies it in
   32 KiB reads into its own weights block and checks size and SHA-256
   against the pinned release asset. Later jobs of the same game reuse those
   verified weights without reading the card again. The runtime validates the
   model either way.
4. Each line is recognized (int8 weights, int32 sums, NEON); lines of dark
   text are inverted first, as the model was trained.
5. The job workspace is freed before the dictionary lookup and before anything
   is shown; the verified weights stay for the next job (see below). A failed
   or cancelled job shows no partial text.

A job's memory is two blocks: the weights, kept between jobs, and a
workspace sized exactly for its region and freed after the job:

```
weights (kept)
max(line finder arena, recognizer arena) + region width × height
+ the runtime's two structs (3,760 bytes on the Vita)          (per job)
```

The table's totals are the two together, the peak of a job; between jobs
only the weights remain, page-rounded.

| model (`vocr_model`) | weights | recognizer arena | full 960×544 region | 880×144 dialogue box |
|---|---|---|---|---|
| `H15_w8.vocr` (default, more accurate) | 1,155,968 B | 182,944 B | 1,864,912 B (1.78 MiB) | 1,469,392 B (1.40 MiB) |
| `FL10_w8.vocr` (smallest; automatic fallback) | 657,664 B | 124,832 B | 1,308,496 B (1.25 MiB) | 912,976 B (0.87 MiB) |
| `F20_w8.vocr` (larger) | 1,558,016 B | 241,056 B | 2,325,072 B (2.22 MiB) | 1,929,552 B (1.84 MiB) |

The line finder's arena (83,323 B at 960×544) is smaller than every
recognizer arena, so it costs nothing extra.

**Default and fallback.** `vocr_model` (default `H15_w8.vocr`) is the model a
job uses when the game has room for it. The game worker checks free `USER`
memory before each job; if the reserve, H15's workspace and (unless already
resident) H15's weights do not fit but the smaller `vocr_fallback_model`
(default `FL10_w8.vocr`) does, that job runs FL10. Resident FL10 weights stay
while H15 still does not fit, and are released for H15 once it does. A
missing, wrong or corrupt `vocr_model` file also makes the game's later jobs
use the fallback (until the game restarts). The kernel opens only the two
pinned names the Shell submitted; the log names the model that ran and, after
a memory fallback, what H15 would have needed. Every pinned model is inside
the console budget ceiling (weights ≤ 1,600,000 B, recognizer arena ≤
262,144 B; bee-san/vita-vn-ocr `docs/vita-memory-budget.md`). In v0.7.1 inference runs in
`VitaJPOverlay_OCR.suprx`, registered under `*ALL`. The `*main` plugin handles
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
network jobs retain their two arenas. Local OCR-only/subtitle jobs use a static
4 KiB filtered-text buffer and allocate no dictionary arena. The UI owns a
separate 4 KiB copy for character navigation. Left/right moves one Unicode
character and requests zero or one best entry at that position; it does not
recapture or recognize text. Scene, region and foreground changes invalidate
saved text, while rapid cursor moves supersede earlier lookup results. If Shell refuses
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

## Models

The weights are the int8 assets of the vita-vn-ocr
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
git -C /path/to/vita-vn-ocr checkout 49ff6f4d1c55804c600d098810574a49d382a009

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
   python3 tools/prepare_ocr.py --backend vocr --output ocr          # H15_w8.vocr
   python3 tools/prepare_ocr.py --backend vocr --model FL10_w8.vocr --output ocr
   ```

   While vita-vn-ocr is private this uses the GitHub CLI (`gh auth login`
   with access to it). `--from FILE` checks a copy downloaded another way.
3. Copy the file to `ux0:data/VitaJPOverlay/ocr/`, or let the FTP installer do
   it: `tools/install_ftp.sh --vocr-model ocr/H15_w8.vocr --set ocr_backend=vocr VITA_IP`.
4. In `ux0:data/VitaJPOverlay/config.ini`:

   ```ini
   ocr_backend = vocr
   ocr_model_dir = ux0:data/VitaJPOverlay/ocr
   vocr_model = H15_w8.vocr
   vocr_fallback_model = FL10_w8.vocr
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
4. **Log.** Fetch `ux0:data/VitaJPOverlay/log.txt` with
   `tools/install_ftp.sh --status VITA_IP`. A completed job logs
   `game vocr rc=... H15_w8.vocr: WxH, lines=N USER workspace=B
   weights=read|resident resident=R, find=X ms load=Y ms recognize=Z ms
   frames=F io=0x... stat=0x... cleanup=...`. Success requires `rc=0` and
   `cleanup=0`. The first text-bearing job of a game logs `weights=read`;
   later ones log `weights=resident` with a much smaller `load`.
   A job that fell back logs `FL10_w8.vocr (fallback)` and a `game vocr
   fallback:` line with H15's needs. Three more lines follow:
   - `game vocr memory: free USER=U, needs workspace J + weights W + reserve
     4194304: check passed|refused`: the game's free memory at that moment,
     also for a refused job (`rc=-103`), i.e. this title's real OCR budget.
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
   For an unavailable worker, check its `*ALL` registration and restart the
   game. A model error with nonzero `io=` identifies game-process file access
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
