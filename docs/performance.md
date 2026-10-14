# Where an OCR job's time goes: the stage benchmark

The recognizer is not the only cost of a job. Between a settled screen and a word on the
overlay, the game worker waits for a poll, reads the region a strip per display frame, finds
and recognizes the lines, and the Shell waits for another poll, writes 21 log lines and
looks the first word up. `tools/bench_stages.py` measures each of those stages on the real
sources (host time, and Cortex-A9 instruction counts of the Vita compiler's code under qemu-arm)
and combines them with the source's polling periods into a model of one job. It is the baseline
for the speed fixes that follow it; each fix reruns it.

All A9 figures are estimates from vita-vn-ocr's cost model, not console measurements. File
system and kernel-copy costs are not modelled at all; the job model marks them as assumptions.
#81's `--collect` logs replace both (see [console calibration](#console-calibration)).

## The job, end to end (estimate)

One Auto OCR job with VOCR3-H15 resident, at the linefind dev regions' means (7.3 strips, 2.4
lines, 86 CTC frames per job; 372 regions of vita-vn-ocr's `eval/linefind/manifests`
`vita_dev`/`psp_dev`, no protected set), `main` at 4a7ae4d:

| part | 444 MHz, 60 fps | 444 MHz, 30 fps | 333 MHz, 60 fps | 333 MHz, 30 fps | what sets it |
|---|---:|---:|---:|---:|---|
| take (game worker poll) | 0-100 ms | 0-100 ms | 0-100 ms | 0-100 ms | `game_ocr/main.c:763`, 100 ms sleep between `vjoOcrTake` calls (mean 50) |
| capture + finder | 121-159 ms | 243-281 ms | 121-172 ms | 243-293 ms | one display frame per strip, 7.3 strips; the finder's 19-38 ms (A9) inside those frames (low) or after them (high) |
| recognition | 178-323 ms | 178-323 ms | 237-430 ms | 237-430 ms | A9 estimate, 86 frames |
| result (Shell poll) | 0-50 ms | 0-50 ms | 0-50 ms | 0-50 ms | `shell/game_ocr_client.c:43`, 50 ms between `vjoOcrRead` calls (mean 25) |
| log.txt (+ status.txt) | 10-126 ms | 10-126 ms | 10-126 ms | 10-126 ms | 21 lines, each an open/write/close; **assumed** 0.5-3 ms each |
| text post-processing | < 0.1 ms | < 0.1 ms | < 0.1 ms | < 0.1 ms | A9 estimate |
| first-word lookup | 3-16 ms | 3-16 ms | 3-16 ms | 3-16 ms | 0.14-0.48 ms CPU + 1 open + 14.3 reads, **assumed** 0.2-1 ms each |
| **total** | **313-774 ms** | **435-895 ms** | **373-894 ms** | **494-1016 ms** | |
| a game's first job | 392-936 ms | 513-1058 ms | 468-1091 ms | 589-1213 ms | + SHA-256 of the weights, 50-104 ms (A9) + the read (assumed 20-40 MB/s) |

Ranges: low is the A9 model's lower bound with no polling wait and the cheaper assumption, high
its upper bound with both polls at their longest. The polls alone average 75 ms per job. The
game worker runs at the lowest user priority, so a busy game stretches its share; that is not
modelled. The settle detection before a job (a signature every 8th game frame,
`kernel/vjo_kernel.h` `VJO_CHECK_EVERY_FRAMES`, and 300 ms without change, `kernel/scene.h`
`SCENE_STABLE_US`) is policy and not in the table.

## The stages

`host/bench/stage_bench.c` runs each stage with the real code: the display hook's grey rows and
signature (`kernel/capture.c`, `kernel/scene.c`), the game worker's job (`core/vocr_ocr.c` with
vita-vn-ocr's finder and recognizer at the pinned commit), the weights' SHA-256 (BearSSL), the
Shell's text handling, word lookup (`core/client.c`, `core/local_dict.c` with the bundled
`plaintext.vjdict`), config parsing, log formatting, the Anki card record and furigana. Inputs
are committed: the synthetic screen `host/samples/vn_textbox.jpg` (its 896x156 dialogue box, two
lines) and the 50 real VOCR lines of `host/fixtures/word_lookup/vocr_lines.tsv`.

| workload | per | host (x86-64) | A9 insns | A9 at 444 MHz | A9 at 333 MHz | libc share | parity |
|---|---|---:|---:|---:|---:|---:|:-:|
| `capture_abgr` | strip | 66.4 µs | 627,170 | 0.71-1.90 ms | 0.94-2.54 ms | 10% | yes |
| `capture_abgr_sig` | strip | 67.5 µs | 642,163 | 0.72-1.94 ms | 0.96-2.59 ms | 10% | yes |
| `capture_a2b10` | strip | 111.1 µs | 1,187,418 | 1.34-3.17 ms | 1.78-4.22 ms | 6% | yes |
| `capture_a1b5` | strip | 110.8 µs | 1,209,573 | 1.36-3.02 ms | 1.82-4.03 ms | 3% | yes |
| `capture_full_abgr` | strip | 75.0 µs | 703,333 | 0.79-2.35 ms | 1.06-3.09 ms | 11% | yes |
| `scene_sig` | signature | 4.2 µs | 67,766 | 0.08-0.33 ms | 0.10-0.44 ms | 34% | yes |
| `scene_update` | update | 0.5 µs | 9,421 | 11-21 µs | 14-28 µs | 0% | yes |
| `scene_update_typing` | update | 1.1 µs | 14,201 | 16-32 µs | 21-43 µs | 1% | yes |
| `find` | region | 887.3 µs | 16,492,226 | 18.57-37.93 ms | 24.76-50.58 ms | 0% | yes |
| `recognize_h15` | line | 33.13 ms | 63,005,352 | 111.57-202.28 ms | 148.76-269.71 ms | 3% | yes |
| `recognize_fl10` | line | 17.69 ms | 37,145,343 | 63.33-118.58 ms | 84.43-158.10 ms | 3% | yes |
| `job_h15` | job | 67.75 ms | 145,456,441 | 223.44-452.54 ms | 297.92-603.39 ms | 2% | yes |
| `job_h15_first` | job | 71.79 ms | 189,735,120 | 225.92-574.31 ms | 301.23-763.21 ms | 2% | yes |
| `sha256_h15` | model | 4.02 ms | 43,966,006 | 49.76-104.44 ms | 66.34-139.25 ms | 2% | yes |
| `postproc` | text | 0.4 µs | 5,309 | 6-12 µs | 8-16 µs | 17% | yes |
| `lookup_first` | lookup | 45.4 µs | 127,116 | 0.14-0.48 ms | 0.19-0.64 ms | 70% | yes |
| `lookup_every` | lookup | 46.5 µs | 116,150 | 0.13-0.38 ms | 0.18-0.51 ms | 65% | yes |
| `config` | parse | 12.7 µs | 233,124 | 0.26-0.53 ms | 0.35-0.70 ms | 10% | yes |
| `log` | line | 0.3 µs | 3,263 | 4-7 µs | 5-10 µs | 100% | yes |
| `anki` | card | 3.0 µs | 48,042 | 0.05-0.11 ms | 0.07-0.15 ms | 32% | yes |
| `furigana` | word | 0.1 µs | 1,640 | 2-4 µs | 2-5 µs | 49% | yes |

*Parity*: the Vita compiler's build gave the host build's output checksum. *libc share*: of the
counted instructions, the part in glibc's `memcpy`/`memset`/`strlen`/`vsnprintf`..., which stand
in for the firmware's (`sceClib*`, `SceSysclibForDriver`); their real cost is unknown.

| workload | memory it touches (per op) |
|---|---|
| capture (each format) | reads 186,368 B of the game's frame (93,184 B at A1B5G5R5), writes 46,592 B of grey into the worker's block per 64-row strip; 2 x 3,840 B of kernel scratch (static) |
| `scene_sig` | the sampled rows of the region (every 7th row and 4th pixel of the 896x156 box), copied a row at a time |
| `find` | 31,971 B arena; 382 row reads (two passes) |
| `recognize_h15` / `_fl10` | 182,944 / 124,832 B arena and the resident weights; 54 CTC frames per line here |
| `job_h15` | 187,696 B workspace + 57,988 B capture block; 10 strips and 604 row reads for a 156-row region |
| `job_h15_first` | 1,343,664 B (one-shot workspace with the weights) |
| `lookup_first` / `_every` | 1 open, 14.3 / 14.0 reads, 49.6 / 48.8 KiB per lookup; arena peak 53,648 B of the Shell's 64 KiB |
| `config`, `log`, `anki` | 4,702 B of config.ini; 115 B per log line; 843 B per card record |

Over the 372 dev regions (`bench_stages.py dev`): 7.3 strips per job (median 8, p90 12, max 18),
2.4 lines (median 2, p90 4), 86 CTC frames (median 78, p90 158); every region row is read 3.99
times (the finder's two passes, the line fetches, the recheck of a region inside one strip); 94
regions fit in one strip; 41 lines are taller than a strip.

## Hot spots

1. **Polled hand-offs: 75 ms per job on average, 150 ms at worst.** The game worker sleeps
   100 ms between `vjoOcrRegister` + `vjoOcrTake` (`game_ocr/main.c:763`), so a submitted job
   waits 0-100 ms before the game claims it, and every candidate game makes those two syscalls
   ten times a second. The Shell asks for the result every 50 ms
   (`shell/game_ocr_client.c:43`). The kernel already waits on an event flag for model I/O
   instead of polling (`kernel/game_ocr.c:651`, `MEV_DONE`), the pattern of ref00d's RSA engine
   thread (blocks on a request bit, signals a done bit: ref00d f3271bc
   `src/ref00d_rsa_engine.c:31-57, 94`); the SDK advises against polling loops
   ([vita-memory.md](vita-memory.md), "Don't busy-wait"). Fix: Submit and Complete signal event
   bits that Take and Read wait on, bounded as the model syscalls' waits are.
2. **Strip waits: 7.3 display frames per job,** 122 ms at 60 fps and 243 ms at 30 fps. Each strip
   is one rows call answered by the game's next frame. An estimated 24% of them (1.7 per job on
   the dev regions) re-read rows for the finder's second pass while the recognizer's part of the
   job's workspace is idle (the scratch is the larger of the finder's 32 KB and the recognizer's
   183 KB arena): kept there, the first pass could serve the second. That is `core/vocr_ocr.c`'s
   strip stream, which the dialogue-quality work (stable-text settle) is changing; the figures
   went to it.
3. **Logging: 21 file opens per job, 42 while `status.txt` is under 64 KiB.** Every
   `vjo_log` line is an open/append/close of `log.txt` and of `status.txt`
   (`shell/log_vita.c:46-66, 115`), so a crash keeps every line. 16 of a job's 21 lines come
   from one result, written back to back between the game's result and the first word's lookup
   (`shell/worker.c:733-734`). Formatting is 4-7 µs a line (A9); the file system's cost is
   unmeasured (assumed 0.5-3 ms per line: 10-126 ms per job). ds4vita (aad83c9 `log.c:8-49`) and
   VitaGrafix (65b6ee3 `src/log.c:10-61`) keep a buffer and write it with one open/write/close.
   The kernel's `kernel.txt` is flushed with its own open/close on every 16 ms tick that has new
   lines (`kernel/main.c:154, 217`), about 3 per job. Fix: one result's lines written with one
   write, same bytes in the same order.
4. **The display hook's grey conversion: 12 instructions a pixel** (`core/fbgrey.h:62`,
   `kernel/capture.c:181`), 0.7-1.9 ms per 64-row strip at 444 MHz (0.9-2.5 ms at 333) inside the
   game's `sceDisplaySetFrameBuf`, and twice that for A2B10G10R10 and A1B5G5R5 (an expansion pass
   to A8B8G8R8, then the grey pass, `core/fbgrey.h:38`). It adds no OCR latency (each strip waits
   for a frame anyway) but it is frame time the game loses during a capture, 7-10 frames a job.
   Fix: two pixels per 32-bit word in 16-bit lanes (77 R + 150 G + 29 B <= 65,280: no carry
   between lanes), and the expansions folded into the grey pass. Integer only: the kernel has
   no FPU (`tools/check_kernel_instructions.py`).
5. **The weights' SHA-256 on a game's first job: 50-104 ms** at 444 MHz (BearSSL's compact
   `br_sha2small_round`, 35 instructions a byte). Once per game session; resident afterwards.
   Not changed: third-party crypto, and a one-time cost.
6. **Lookups are I/O, not CPU.** 0.14-0.48 ms of CPU per lookup (70% of it in libc: `strlen`
   over the preferred-readings table for every candidate, `core/local_dict.c:492`, and `memset`
   zeroing the 45 KB workspace, `:588`/`:718`), against 1 open and 14 reads of 4 KiB at random
   offsets of a 134 MiB file. Each
   read is an `sceIoLseek` and an `sceIoRead` (`shell/platform_vita.c:341`). A page cache kept
   across the lookups of one text would save little: replaying the every-letter trace, 0.7% of
   reads with the current 4 pages, 9.5% with 32 pages (128 KiB of SceShell memory). So no cache;
   `sceIoPread` halves the syscalls per read (nonpdrm 8372225 `main.c:228` reads with
   `ksceIoPread`; VitaTweaks 4233c54 `DownloadEnabler/main.c:50` with `sceIoPread`).
7. **Recognition is the bulk of a job** (112-202 ms per line for VOCR3-H15 at 444 MHz on these
   54-frame lines, 63-119 ms for VOCR3-FL10), and it is vita-vn-ocr's runtime, pinned here.
   Its own speed work happens there.
8. **Wakeups.** SceShell's control thread wakes every 50 ms (`shell/worker.c:44`), with or
   without a game; the kernel worker every 16 ms (`kernel/main.c:21`); the game worker every
   100 ms in every candidate game. CPU3's share is unmeasured (AGENTS.md rule 8).

Not hot: config parsing (0.26-0.53 ms per overlay open), text post-processing (< 12 µs), the scene
tracker (< 31 µs per 64 ms), card records (< 0.11 ms), furigana (< 4 µs a word). The overlay's
Paf layout and font rendering run in ScePaf and cannot run off the console.

## Plugin size budget

The Shell module is at 299,008 B of its 299,008 B budget: 40 B left in its code page and 688 B in
its data page ([memory-ledger.md](memory-ledger.md)). A Shell-side speed change that adds code has
to save as much elsewhere in the Shell, or it adds a page to SceShell, which AGENTS.md rule 1 does
not allow. The kernel module has 156 B (code) and 112 B (data) left, the game worker 92 B and
2,460 B (Vita build of `main` 4a7ae4d).

## Reproduce

```sh
. .refs/env.sh   # VITASDK, ARM_CC, QEMU_PLUGINS_DIR (vita-vn-ocr's plugin qemu), VJO_VOCR_SOURCE_DIR
cmake --build build/host --target vjo-stage-bench
python3 tools/bench_stages.py smoke --binary build/host/vjo-stage-bench --models $VJO_VOCR_MODEL_DIR
python3 tools/bench_stages.py host --binary build/host/vjo-stage-bench --models $VJO_VOCR_MODEL_DIR \
    --dict build/host/plaintext -o host.json
python3 tools/bench_stages.py a9 --vita-build build/vita --models $VJO_VOCR_MODEL_DIR \
    --dict build/host/plaintext -o a9.json
python3 tools/bench_stages.py dev --binary build/host/vjo-stage-bench --models $VJO_VOCR_MODEL_DIR \
    --manifests $VJO_VOCR_SOURCE_DIR/eval/linefind/manifests --shots DIR [--shots DIR] -o dev.json
python3 tools/bench_stages.py report --host host.json --a9 a9.json --dev dev.json \
    -o docs/benchmarks/pipeline-stages-YYYYMMDD.json --md table.md
```

`smoke` (ctest `stage_bench_smoke`) runs every workload once and compares each output checksum
with `tools/bench_stages_expected.json`: a speed change that alters a stage's output fails it.
The `a9` lane links the Vita build's own libraries (`libvjo_core_vita.a`, `libvjo_vocr*.a`,
`libvjo_bearssl.a`) and the kernel's `capture.c.obj`/`scene.c.obj`, so the counted instructions
are the plugins' (Thumb-2, the modules' own flags), runs each workload with 1 and 3 iterations
and keeps the difference (one steady-state iteration), and costs it with vita-vn-ocr's
`vita/a9_cycles.py` at the pinned commit (NEON/VFP issue, dual issue, load/store pipe, the 32 KiB
L1 and 2 MiB L2 simulated by qemu's cache plugin). The bench's own code is not counted; glibc's
string and formatting functions are, as stand-ins, reported as *libc share*. The baseline's raw
data: [benchmarks/pipeline-stages-20261014.json](benchmarks/pipeline-stages-20261014.json).

## Console calibration

Only the console has the file system, the firmware's copies and SceShell's scheduling. #81's
`--collect` zip answers the model's assumptions directly:

- each `lookup ...: N reads B, rc, T ms` line gives the time of N dictionary reads and an open;
- each `job N done rc=0 in T ms` beside the game's `find=/load=/recognize=` gives the Shell-side
  overhead (polls, logs, lookup) of that job;
- `game vocr capture: ... S strips in T ms` gives the strip waits at that game's frame rate.

These are not measured yet; every console number above is marked as an estimate or an assumption.
