# PP-OCRv6 small: slow, accurate manual OCR

`ocr_backend = ppocrv6` (Settings → OCR & lookup → **PP-OCRv6**) reads the
selected dialogue region with PaddleOCR's PP-OCRv6 small line recognizer. It
is the most accurate local reader measured for this overlay, and much slower
than VOCR. **VOCR stays the default**; PP-OCRv6 is opt-in.

**Status: host- and emulator-tested only. Nothing in this document ran on a
physical PS Vita.** The memory and time figures below come from host builds, from
the Vita build's own machine code under qemu-arm and Vita3K, and from a Cortex-A9
cost model.

## What it is

- PP-OCRv6 small rec as RapidOCR 3.10.0 publishes it (ONNX, Apache-2.0), with
  its CTC head pruned from 18,709 to 6,514 classes: JIS X 0208, printable ASCII,
  full-width forms and `♪♡♥☆★〜～`, plus the final space (vita-vn-ocr eval's
  `ppocr-v6-small-jis` set). Pruning removes output classes of the last layer only.
- Converted to ncnn by pnnx and int8-quantised by the pinned ncnn's own
  `ncnn2table`/`ncnn2int8` (every convolution, Gemm and attention layer is int8).
- ncnn is **linked statically into the existing `*ALL` game worker**,
  `VitaJPOverlay_OCR.suprx`. There is no separate engine module, nothing is
  loaded from `app0:` and no per-game rePatch mount is needed (unlike the removed
  Meiki trial). The graph and class list are compiled into the worker; the
  3.9 MB weights file stays on the memory card.
- The weights reach the game through the existing kernel model broker, like
  VOCR's, so games that cannot open `ux0:` (ENODEV) still work. The broker accepts
  only `ux0:data/VitaJPOverlay/ocr/PP-OCRv6_small_jis_int8.bin` at its pinned length
  (3,893,668 bytes); the worker checks its SHA-256
  (`2555ebd7c7734032ad09a1ea5c63efaa690906af56947ed5203f19b9563b1320`) while
  streaming it into ncnn and refuses any other file.
- Lines come from vita-vn-ocr's polarity-agnostic line finder (the one VOCR uses).
  Each line is converted to the console's grey luma, resized to height 48 (width at
  most 1024) and read as it is on screen: PP-OCRv6 reads light and dark text, so
  dark lines are not inverted as they are for VOCR.

## Use

1. Install the release package (it carries the weights), or add them to an
   existing installation:

   ```sh
   python3 tools/prepare_ppocrv6.py fetch --output ocr      # or: --from FILE
   bash tools/install_ftp.sh --vocr-model ocr/PP-OCRv6_small_jis_int8.bin VITA_IP
   ```

   `fetch` downloads the pinned asset of release `ppocrv6-small-jis-int8.1`
   (through `gh release download` while that release is a draft). Every path
   checks the size and SHA-256.
2. Open **JP Overlay Settings**, press Select, and choose **PP-OCRv6** in the OCR
   engine row. The row shows whether the model file was found.
3. In the game, draw a **tight** region with Square (one or two dialogue lines
   reads fastest) and open the overlay with L+R. The overlay shows
   *Recognizing with PP-OCRv6… This takes a few seconds.* until the text appears.

PP-OCRv6 is **manual only**: it runs when the overlay opens, never in automatic
background mode and never for subtitles (Select+R reports that it is manual only),
whatever `ocr_mode` says. If the dialogue changes while it runs, the overlay
reports *The dialogue changed. Close and reopen the overlay.* instead of starting
another multi-second job by itself. Closing the overlay does not stop a running
job: its result is shown when you reopen the unchanged dialogue (a dialogue
change cancels it). Suspending the
game, a game change or a Shell restart cancels the job between model chunks and
between lines; the job's thread, workspace and model descriptor are released
before another job can start, and a cancelled job never publishes text.

Errors are explicit: a missing or wrong weights file names
`PP-OCRv6_small_jis_int8.bin`; too little game memory says that PP-OCRv6 needs
about 23 MiB and suggests a tighter region or VOCR; if the Shell cannot allocate
dictionary memory after recognition, the recognized text stays on screen.

## Accuracy (host)

vita-vn-ocr `eval/run.py` at `49ff6f4`, hand-checked line crops (box + 4 px on
Vita, + 2 px on PSP). PP-OCRv6 runs through this repository's own C++ path:
`vjo-ocr --backend ppocrv6 --lines`, which runs each crop as one complete job
(weights read and checked, the same ncnn options as the game worker). FL10 and H15
run in vita-vn-ocr's C runtime (`vocr:`, W8I) with the console's polarity rule,
as the overlay runs them.

| recognizer | vita-test CER / exact lines | psp-test CER / exact lines |
|---|---|---|
| FL10 (VOCR default) | 2.38% / 76.7% | 5.30% / 67.3% |
| H15 | 1.56% / 84.0% | 2.87% / 81.2% |
| PP-OCRv6 small, JIS head, fp32 (ncnn, x86) | 1.13% / 90.3% | 1.16% / 91.0% |
| PP-OCRv6 small, JIS head, int8 (shipped), x86 | 1.33% / 88.3% | 1.55% / 88.2% |
| **PP-OCRv6 small, JIS head, int8 (shipped), the Vita build's ARM code** | **1.44% / 87.4%** | **1.59% / 87.8%** |

vita-test: 206 lines (2,564 characters) from 16 games; psp-test: 245 lines
(2,510 characters) from 14 games. On x86, int8 costs 0.20 (Vita) and 0.39 (PSP)
CER points against fp32.

Whole regions, with the console line finder (`--finder console`; a missed line
counts all its characters):

| pipeline | vita-test concat CER | psp-test concat CER |
|---|---|---|
| FL10 | 8.66% | 17.65% |
| H15 | 6.78% | 14.70% |
| PP-OCRv6 int8 | 6.03% | 11.47% |

The finder found 95.2% of the Vita lines and 90.2% of the PSP lines for all three;
most of the remaining error is lines it misses.

**ARM.** The Vita build's own objects (ncnn, adapter, line finder, heap, BearSSL,
all compiled by `arm-vita-eabi-gcc` as linked into the worker) ran every crop under
qemu-arm with a small Linux driver: 37 and 40 character errors (above), against 34
and 39 for the x86 build. ncnn's ARM and x86 int8 kernels round differently; the two
outputs differ on 12 Vita and 21 PSP lines, in both directions. The ARM build's
GEMM and direct int8 kernels gave identical text on every line.

### Calibration

ncnn's KL calibration alone clipped this model's activations hard (thresholds as
low as 6% of the observed maximum, median 44%) and lost 0.7–0.9 points
(1.79% / 2.07%).
Keeping attention, Gemm, the head, the stem or the squeeze-excitation blocks in
float did not fix it. Raising every activation threshold to at least a fraction of
its calibration maximum did. The fraction was chosen on realdata's held-out
**val** split (471 lines, 3-of-4 reader-vote labels, games outside both eval sets
and the calibration), not on the test sets:

| variant (val) | Vita lines CER | PSP lines CER | all |
|---|---|---|---|
| fp32 | 0.24% | 0.53% | 0.43% |
| KL | 0.43% | 0.96% | 0.77% |
| ACIQ | 0.67% | 0.63% | 0.64% |
| KL, floor 0.25 × max | 0.37% | 0.59% | 0.51% |
| KL, floor 0.33 × max | 0.12% | 0.63% | 0.45% |
| KL, floor 0.50 × max | 0.37% | 0.63% | 0.54% |
| **KL, floor 0.67 × max (shipped)** | **0.24%** | **0.53%** | **0.43%** |
| no clipping (max) | 0.18% | 0.82% | 0.60% |

Before that selection three variants had been scored on the test sets
(floor 0.33: 1.25% / 1.47%, floor 0.50: 1.29% / 1.35%, no clipping 1.36% / 1.71%);
those numbers did not choose the shipped one. Differences of ±0.2 points between
these variants are within test-set noise.

Calibration uses 400 real dialogue line crops (200 Vita, 200 PSP; 269 VNs,
355 VNDB screenshots, 238 light-on-dark and 162 dark-on-light) from realdata's
**training** split, which excludes every eval test game, related VN and capture
session (`tools/ppocrv6_calibration.jsonl` lists URL, SHA-256 and box of each;
the screenshots are not redistributed). Lines are fed as the worker feeds them:
grey luma, height 48, cut or edge-padded to 640 px.

## Memory

All of a job's memory is in the enabled game's process, allocated only while the
job runs:

| | bytes |
|---|---|
| one private USER_RW memblock: grey region (w × h), ncnn heap (18 MiB), the job's request copy and text (4,408 B), page-rounded | 19,402,752 (full 960×544), 18,923,520 (one 960×44 line) |
| job thread stack, created for the job and deleted after it | 65,536 |
| game reserve the worker leaves free (unchanged, as VOCR) | 4,194,304 |
| **free game USER needed before the job starts** | **23,662,592 (22.6 MiB)** for the full screen |

ncnn's tensors, layers and weights all live in its private heap; nothing is
allocated from the game's heap, Paf or Shell. Peaks inside the heap:

| | Vita build's code (qemu-arm, Vita3K) | host x86 |
|---|---|---|
| highest heap offset reached, one line at the 1024-px clamp | 15,331,520 | 17,172,800 |
| same, 2, 3 and 11 lines at the clamp in one job | 15,331,520 | 17,172,800 |
| one 640-px line | 11,159,488 | 14,350,912 (798 px) |
| bytes in use at the peak (1024-px clamp) | 15,288,192 | 15,582,208 |
| stack high-water mark of the job | 14,260 | not measured |

Every line is clamped to 1024 px, and activations are freed between lines, so the
peak does not grow with the number of lines. The 18 MiB heap leaves 23% spare over
the Vita code's peak. Shell memory and the kernel's 8 KiB staging buffer are
unchanged.

**Games that never select PP-OCRv6** pay only the larger worker module, which the
`*ALL` plugin loads into every application:

| `VitaJPOverlay_OCR` | `origin/main` (1a118ef) | this branch |
|---|---|---|
| code + read-only data (`.text`) | 33,224 B (9 pages) | 825,672 B (202 pages) |
| `.data` + `.bss` | 4,176 B | 4,320 B (still 2 pages) |
| `.suprx` file | 32,312 B | 554,175 B |
| idle worker thread stack | 32 KiB | 32 KiB (unchanged) |

That is **+790,528 bytes (772 KiB) of module pages per application process**, and
no heap, stack, thread or file I/O until the engine is chosen and a job starts.
The worker has one static constructor from ncnn (for `CpuSet`). Nothing calls it:
the worker is linked with `-nostartfiles`, so no C runtime start-up code runs
`.init_array`. Nothing depends on it either: it only stores zero to a `.bss` word.
The Shell grows by 1,128 bytes of code and 8 bytes of `.bss`, the kernel by 52 bytes
of code.

## Vita3K

Vita3K build 4154 ran a scratch homebrew test app (not part of this change):

- It loaded the built `VitaJPOverlay_OCR.suprx` with `sceKernelLoadStartModule`:
  both segments mapped (0xC9C60 and 0x10E0 bytes), every relocation linked and
  `module_start` returned 0. With no kernel plugin, the worker's `vjoGetVersion`
  import is missing, so it stays idle as it should. `module_stop` and the unload
  returned 0.
- The same app ran the Vita build's PP-OCRv6 libraries (the ones the worker links,
  line finder included) on a 64 KiB thread stack, with the workspace in a USER_RW
  memblock and the weights read with `sceIo`: a 586×44 line, a 960×44 line and a
  three-line 880×144 box all returned rc=0 with the same text as the host build;
  the 1024-px cases reached the same 15,331,520-byte heap extent as under qemu-arm.

Its times (15, 20 and 62 s) are the emulator's x86 JIT, not a Vita.

## Speed

Host x86 (one thread, SSE2 only, the C++ path): 102–118 ms per line on the test
sets, 194 ms for a 1024-px line, about 2.1 s for an 11-line full screen. That says
nothing about the Vita.

Cortex-A9 estimate from the Vita build's machine code: qemu-arm counted every
executed instruction with vita-vn-ocr's `insn_mix` and cache plugins, and
`vita/a9_cycles.py`'s cost model gives a range (low: perfect overlap; high: no
dual issue and full miss latencies):

| one job, including the model's SHA-256 check and parse | 444 MHz | 333 MHz (default clock) |
|---|---|---|
| one 640-px line | 1.8–5.9 s | 2.4–7.7 s |
| one 1024-px line | 2.9–9.9 s | 3.9–12.7 s |
| two 1024-px lines | 5.8–19.2 s | 7.8–24.7 s |

Add the weights' transfer through the kernel broker: 476 reads of 8 KiB. On a
PCH-1000 in CLANNAD, Meiki's broker-fed load (two passes over a 5.4 MB model, about
1,300 reads, plus parsing) took 2.15 s, which suggests well under 1 s here. So a
one-line region is an estimated 3–10 s at the default clock, and each further
full-width line adds 4–12 s: a three-line dialogue box is an estimated 12–40 s.
Select one line for the shortest wait. These are estimates, not console
measurements. The job thread has the same priority as VOCR's worker. ncnn's int8 GEMM convolution path is used (about 30% fewer
instructions than its direct kernels, identical output on all 451 test lines,
+1 MB heap). The exact-erf GELU costs about 11% of the instructions; a faster
approximation would need a new accuracy measurement.

## Not yet verified

- No physical Vita run: memory availability in real games, the actual speed and
  clock, worker stability during long jobs, and suspend/resume during a job all
  need a PCH-1000 check (below).
- Vita3K cannot run the overlay's path: the job transport and model broker are
  syscalls of the kernel plugin (`VitaJPOverlayForUser`), and Vita3K emulates user
  mode only, without taiHEN (vita-vn-ocr's earlier Vita3K work substituted a
  private bridge module for the kernel API). See *Vita3K* above for what did run.
- Accuracy is host crop and region CER on vita-vn-ocr's hand-checked sets. The
  ARM run checks that the Vita code reads the same text; it is not a retail-game
  measurement.

### PCH-1000 checklist

1. Install the package, reboot, choose PP-OCRv6 in Settings, set `log_file = on`.
2. In CLANNAD (PCSG00415), select one dialogue line with Square and open the
   overlay. `log.txt` should show `game vocr rc=0 PP-OCRv6_small_jis_int8.bin: ...
   USER workspace=... find=... load=... recognize=... io=0x00000000 cleanup=0`.
   Note the times and the shown text.
3. Repeat with the whole dialogue box, then with a full-screen region.
4. Press PS during a job, resume, and reopen: expect no crash and a fresh job.
5. Choose VOCR again: VOCR must behave exactly as before.

## Rebuild the weights

```sh
pip install -r tools/requirements-ppocrv6.txt   # numpy, onnx, protobuf, Pillow (pinned)
python3 tools/prepare_ppocrv6.py build --work /tmp/ppocrv6 --output ocr \
    [--realdata /path/to/realdata]               # its shots/ folder; else downloaded from VNDB
```

It downloads the pinned ONNX, pnnx 20260704 and ncnn `3798be64` (each checked by
SHA-256), builds ncnn's quantisation tools (x86 SSE2 only, so every host computes
the same statistics), prunes the head, converts, calibrates and quantises, then
compares the weights, param and class list with the pins and fails on any
difference. On the build host a second run reproduced every pinned file byte for
byte (another compiler could change the calibration statistics; the pin check
would report it). One run takes about 80 s on 5 cores. The build header (`vjo_ppocrv6_data.h`) is generated from the committed
param and class list by CMake.

## Licences

The PP-OCRv6 model, its dictionary and the converted weights are PaddleOCR's,
Apache-2.0 ([licence](../third_party/licenses/PaddleOCR.txt)); the ONNX export is
RapidOCR's (Apache-2.0). ncnn is BSD-3-Clause with bundled notices
([licence](../third_party/licenses/ncnn.txt)); pnnx is only a build tool. See
[third_party/README.md](../third_party/README.md).
