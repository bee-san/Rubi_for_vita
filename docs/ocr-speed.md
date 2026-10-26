# OCR speed (API 22): faster recognizer, parallel lines, the line cache, the clock

The owner's v0.8.0-test.1 console logs (#81) are the ground truth: VOCR3-H15 ran its lines of 60-100 CTC frames
at a frame-weighted **4,942 µs per frame** (10 lines, 4,726-5,174 µs; the 11 jobs at 4,941-5,238 µs, 5,068
frame-weighted, which in the five-line full-screen job includes strip reads outside the recognizer), 0.10-0.46 s per
line; the finder and the capture strips took 67-137 ms per 883×112 textbox, a 5-line full screen 1.84 s of
recognition. That build decoded greedily. This one makes the recognition about 1.75 times faster on one core and
divides it over the cores the game leaves free, in four steps; every one keeps the text what the serial job gives.

| step | what | effect (estimate unless marked) |
|---|---|---|
| 1 | vita-vn-ocr's faster W8I kernels (vita-vn-ocr #85, its `docs/ocr-speed.md`; the runtime pin) | H15 5.07 → 2.90 ms per frame at 333 MHz with the LM's top 8, 4.95 → 2.81 greedy (1.75x; 1.51x if the A9's prefetch does not help); bit-identical logits, texts and top-K |
| 2 | **parallel lines**: up to 3 lines at once (`ocr_threads = 3`, the default), each on its own thread at the lowest priority on the game's cores | a 3-line box takes about its longest line instead of the sum, when the game leaves cores free |
| 3 | **the line cache**: a line whose pixels are unchanged since a job read it gets that job's text without being recognized | a box read again (auto mode re-runs jobs on the same text: #81's log has the same 742×39 line recognized three times, 382-392 ms each) costs a hash, not a recognition |
| 4 | **`ocr_boost_clock = on`** (the default): 444 MHz while a job recognizes, the clock before restored after; a clock already at 444 MHz or more is left as it is | 1.33x on the recognition |

## A 3-line textbox (3 × 80 frames, VOCR3-H15, the LM's top 8), before → after

| stage | before (test.1) | after, `ocr_boost_clock = off` (333 MHz) | after, the default (444 MHz) | source |
|---|---:|---:|---:|---|
| capture strips + finder | 67-137 ms | same | same | console (#81) |
| weights | resident 1-7 ms (first job 219-232 ms) | same | same | console (#81) |
| recognition, 1 core | 1.22 s | 0.70 s (0.81 s without the prefetch) | 0.52 s | before: test.1's rate with #89's top-8 head (+2.5 %, estimated); after: vita-vn-ocr #85's A9 estimate calibrated to test.1 |
| recognition, 2 cores free | - | 0.46 s | 0.35 s | estimate: lines 1-2 at once, then 3 |
| recognition, 3 cores free | - | 0.23 s | 0.17 s | estimate: the 3 lines at once |
| the beam + LM decode and the cascade | 0.4-0.7 ms a line; doubtful lines read again by VOCR4-W20 (#89) | the same work, in line order on the job's thread | same | vita-vn-ocr #82's estimates |
| the same box again | the whole job | 0 recognitions, about 1-2 ms of hashing (estimate) | same | host: 845 of 845 lines from the cache on the 2nd pass of 363 regions (greedy) |

How many cores a game leaves to the overlay is unmeasured; the lines run at the lowest user priority, so every
game thread preempts them. What test.1 does show: the recognizer's wall time per frame stayed within 4,726-5,174
µs on its ten lines of 60-100 frames (±5 %), where the cost model's high bound for that code with no preemption
gives 4,939 µs at 333 MHz, so CLANNAD left at least one core almost entirely to the worker (an inference from the
log, not a CPU reading). Whether a second and a third core are free is what this build logs, per line and per job
(below).

## What the log shows (API 22)

`log.txt`, per job, from the worker's step log:

- `gstep job par kind=- arg=H rc=0xD bytes=B sum=0xFFCCLLMM ... us=R`: H helpers the job could use, D lines they
  recognized, B bytes their line slots and stacks took, R the recognition in µs; `sum` packs the helpers that did
  not start (FF), the lines from the cache (CC) and the recognized lines' **lowest and mean share of a core** (LL,
  MM: the thread's run time against the wall time of its `vocr_run`, percent; `sceKernelGetThreadInfo` runClocks).
  It was meant to show that game threads had that core, but on the console it is not a measurement: in test.3's log,
  for one model at one clock in one window, the time per frame is flat while the share runs 17-92 % (0 in 3 jobs;
  audit-overlay-analyser-fixes-2-b B3). Why is unmeasured.
- `gstep job clock kind=- arg=BEFORE rc=0x... bytes=DURING sum=AFTER`: the ARM clock (MHz) before, during and after
  the recognition, and the set call's result (1: already at 444 MHz or more, left alone: never lowered). With
  `ocr_boost_clock = on`, the default. A job writes it only when it recognizes a line (the worker's busy hook runs
  at the first line not from the line cache), so a job whose every line came from the cache has none, boost on or
  off. `analyse_selftest.py --collect` therefore reads the boost per game window, from each `game TITLE started`
  line (a start or a resume; the Shell reads `config.ini` again just before it) to the next, from the window's jobs
  that recognized a line: it lists each window's clock before, during and after (or that its jobs logged none: the
  boost off), and compares each recognized line's time per CTC frame with the boost off and raised, by model.
  With the boost off no job reads the clock, so a boost-off window's clock is given as assumed: the game's own, as
  the jobs nearest it read it (after the previous window's last job, before the next one's first), beside any clock
  plugin (PSVshell) in the collect's taiHEN configurations. The ratio is given over all jobs and like for like: not
  the self-test's stages (textbox2 runs serially), between jobs of one kind on both sides (the same game, as many
  recognized lines, the same helpers, and both Auto's with the overlay closed or both the overlay's own), pooled, by
  window and against the off window's neighbours, with job counts (a job's helpers are its `par` step's arg; one with
  no `par` step is serial only when the kernel's `pace` follows, since the worker's FREEs keep their slots in a full
  log). The verdict line, `boost effective: yes|no|unclear (ratio X, n=N)`, reads the like-for-like ratio of the
  wall time: yes at 1.25 to 1.42 (three quarters of 444 / 333's gain to a quarter over it), no at 1.08 or less,
  unclear between them, above 1.42 (more than the clock explains), with under 2 jobs a side, when the game's own
  clock was not one figure around the raised jobs and next to the off windows, or when an off window against its
  neighbours gives another verdict (drift between windows). Whether other threads took more of the core from one
  side's lines is not checked: the share above is not a measurement, so its line time × share ratio is printed as a
  diagnostic only, and a yes says so. With one off window its scene cannot be told from the clock: read the same
  boxes with the boost off and on, or alternate them.
- `gstep job pace kind=- arg=F rc=MAX bytes=SUM sum=SLOW main=SUM0 cdram=MAX0 phycont=SLOW0 us=F0`: added by the
  kernel, the **game's frame pacing**: F frames during the job (from its claim to its completion), their longest gap
  MAX (µs), their sum, SLOW gaps over 25 ms; and the same for the F0 frames since the job before. A job that costs
  the game frames shows a larger MAX and more SLOW than the window before it.
- `game vocr lines:` is unchanged (crop, frames, ms per line: a helper's line is its `vocr_run`, the job's own its
  run and decode); `game vocr speed:` is the recognition's ms per frame (cached lines run no frames and are not in
  it).

## Parallel lines

`core/vocr_ocr.c` (`VjoVocrPool`). The job's thread does everything the serial job does, in line order: each
line's fetch from the strip with every row check, the refinement, the blank-line check, the cache lookup, and, as
each line's turn comes, its LM decode, the cascade's re-read (VOCR4-W20 for a doubtful line), its stats, its text
and the recognizer's raw text (`finish_line`, `place`). Only `vocr_run` with the top-K moves: a helper runs it on
its own arena and context over the shared, read-only weights, into its own line slot (a copy of the line's pixels,
its greedy text, its top-K). The last line, and any line when no helper is free, runs on the job's thread: straight
from the strip into the text when nothing before it is still out (the serial job's path, the serial capacity check
first), else into the job's own slot. `vocr_run` and the decoder are deterministic and share nothing but the
read-only weights and LM, so the text is the serial job's. The one thing that depends on time is the cascade's
ceiling (`ocr_cascade_ms`): the job estimates the time still to recognize from the lines not yet run, so a job whose
lines run sooner may read more doubtful lines again than the serial one would.

On the 363 linefind development regions (845 lines, 30,571 frames; the 9 rows of v1267, v4060 and v36, which
vita-vn-ocr reserves for its test sets, are refused) with the real VOCR3-H15 and VOCR3-FL10 weights, greedily and
with the 1 MB LM and the cascade reading every line again, serial, 1 helper, 2 helpers and the cache gave **0
differences** in the shown text, the recognizer's text and every line's frames and flags
(`host/vocr_parallel_bench.c`, `tools/vocr_regions_pack.py`; the cache's second pass compares the texts). The host tests (`host/tests/test_vocr_parallel.c`, a
tiny model, a second one as the cascade and a tiny LM, so CI runs them without release files) compare parallel jobs
with the serial one greedily and with the LM and the cascade, with real threads, slow ones, threads that run at once
and threads that never start; every text capacity against the serial job's result (`VJO_E_TOO_LARGE` exactly where
it gives it); cancellation while helpers run; a helper whose join fails (the job stops, below); fewer helpers when
the workspace holds fewer. 43 mutations of the scheduler, the cache and the worker's helpers and clock each fail
them (ASan/UBSan).

What overlaps: while a helper recognizes line i, the job's thread fetches line i+1 (a strip re-read, one display
frame, when the line is not in the strip it holds), decodes and re-reads the lines before it, and hands lines on.
What does not: the finder (67-137 ms per textbox on test.1, mostly strip waits at one display frame each) still runs
before the first line, because it needs the whole region to separate the lines.

In the game (`game_ocr/main.c`) each helper is a thread created for its line and deleted once the line is placed
(`VjoGameVocrHelper`, 16 KiB stack, the worker's priority and cores), so no thread or stack outlives a job. A thread
that cannot be created runs its line on the worker's thread. A line of at most a strip's bytes may go to a helper,
one taller than a strip too: it is copied out of the tall-line block into the helper's slot first, so no helper reads
that block; a larger line runs on the worker's thread. The job's memory check counts the helpers' slots (the worker's own, 68,160 B, and per helper its
recognizer arena, context and slot: 253,104 B for H15) and stacks, after everything else it chose (the model, the
LM, the cascade), and takes one helper fewer at a time down to none when the game's free memory minus the 4 MiB
reserve is short. A cancelled job stops its helpers at their next 4-frame tile and joins them before it returns. A
helper that cannot be joined (its `sceKernelWaitThreadEnd` times out) may still run in the job's workspace: the job
fails with `VJO_E_OCR_UNAVAILABLE` and is not tried again, the workspace and the weights stay allocated, and the
worker takes no more jobs; when the plugin stops, it waits for the worker first, then for the helper, and only then
gives the memory back.

Host timing (x86, 5 threads of a shared box; only the division of the work, not Vita numbers), H15, ms per region
(2.3 lines on average; a one-line region gains nothing):

| decoding | serial | 1 helper | 2 helpers | the same regions from the cache |
|---|---:|---:|---:|---:|
| greedy | 52.6 | 39.7 | 32.0 | 0.80 |
| 1 MB LM, the cascade reading every line again | 130.2 | 117.2 | 108.1 | 3.7 |

With the LM and the cascade, the decode and the re-read stay on the job's thread (they decide the text in line
order), so the helpers take only the first recognition off it; with the default 0.7 confidence the cascade reads
about one line in five again (vita-vn-ocr #82), not every line as here.

## The line cache

`VjoVocrCache`, 8 lines in the worker (6,160 B of .bss): the key is a 64-bit FNV-1a of the line's grey rows, its
size, its polarity and the job's `cache_variant` (the LM tier, the cascade and its ceiling, the refinement); entries
of another model never match, and a job that reads the weights from the file again empties it. A hit gives the
line's shown text, its recognizer text and its `VjoVocrLineStat.flags` (its confidence, LM, cascade). A line is kept
only when its text is the one an unhurried serial job shows anywhere: read with the LM the job asked for, read again
by the cascade when its confidence asks for it (not skipped for the time ceiling), shown as read (not a shorter
fallback near the text's capacity), not read as empty by the cascade. A hit whose longest reading would not fit
where it falls makes the job read the region again without the cache, as the serial job would. A job adds its
lines only once it has succeeded. On the development regions with the LM and the cascade, 825 of 845 lines came
from the cache on the second pass; the other 20 were lines the cascade read as empty.

## The request (API 22)

The helpers and the clock ride in a new word, `VjoGameOcrRequest.speed` (`VJO_GAME_OCR_S_*`: bits 0-1 the helpers,
bit 2 the clock), not in `flags`, whose bits API 20's decoder, cascade and refinement use; the kernel and the worker
refuse unknown bits of either word before anything is allocated.

## The ARM clock (`ocr_boost_clock`, on)

On by default: the owner's decision of 2026-10-15, on the advice of shoui, a PS Vita developer, that
`scePowerSetArmClockFrequency` is safe to call, that games call it themselves, and that 500, 222 and 166 MHz are
settings people run all the time. `scePowerSetArmClockFrequency(444)` just before the first line is recognized and
back to the clock read before once the last one is, unless the game changed it meanwhile; nothing for a job whose
lines all come from the cache. A clock already at 444 MHz or more (PSVshell's 500, say) is left as it is and logged
(`rc=0x1`): the worker never lowers a clock. Only the ARM clock changes; the bus and GPU clocks are never touched.
`ocr_boost_clock = off` keeps the game's clock; the installer's migration adds `on` to a config that never set the
key and keeps an explicit `off`.
ScePower keeps the clock each process asked for and applies it while that process is in front, so the request is
the game's own and ends with it (PSVshell reads the same per-process value, Electry/PSVshell 9a47b0e
`src/oc.c:119-147`, and replaces a game's request in its kernel hook, `src/main.c:106-123`: an inference from those
two sources, not a console measurement). 444 MHz is the highest the API accepts; 500 needs PSVshell's kernel patch
(`main.c:113-117`), which this does not make: 1.13x more is not worth a kernel patch.

## Memory (ledger)

| item | before (main `8f1ec8f`) | after | note |
|---|---:|---:|---|
| OCR worker pages | 73,728 | 90,112 | +2 text pages (text 62,012 → 70,124 B: the scheduler, the cache, the helper threads and the clock, 67,452 B on main's runtime, and vita-vn-ocr #85's kernels, +2,672 B) and +2 data pages (5,840 → 14,200 B: the cache and the line queue); the game's partition [measured] |
| VjoGameVocr workspace, worst | 618,496 | 1,191,936 | the line slots of two helpers after the LM and cascade extras, at 960×544; only when the job's check passes with them |
| VjoGameVocrHelper stacks+TLS | 0 | 40,960 at most | per line, never between jobs |
| game job limit | 8,105,984 | 8,720,384 | 9.7 % of the 90,177,536 B free USER #81's CLANNAD session had at the least |
| kernel pages | 114,688 | 114,688 | the frame-pacing step: 36,336 → 36,608 B of text, 77,720 → 77,768 B of data/bss [measured] |
| Shell pages | 294,912 | 294,912 | text 216,448 → 216,856 B, data/bss 77,624 → 77,648 B (the two config keys, the request's speed word, the self-test's step names and stage words; the new config fields last in `VjoConfig`, so no other field moves): the same pages, 232 B of text and 176 B of data to spare; no Shell allocation added |

Each block still goes through the ledger wrappers (`tools/check_memory_ledger.py`: pass). CPU3: unmeasured.

## What these speeds allow for the next models (VOCR5)

**The VOCR5 sizes** (the conductor, 2026-10-15): the owner's weight caps bind over the time ceilings below (Small
< 1 MB, Normal ≤ 1.6 MB, Large ≤ 2.5 MB), and Normal, the default tier, is sized for about 1 s per 3-line box on one
core at 333 MHz: **Small < 1 MB, Normal about 1.26 MB, Large ≤ 2.5 MB**, provisional until test.3's logs confirm the
speed. The table gives the time ceilings those sizes stay inside. 333 MHz is the safe clock for the sizing: the
default raises the clock to 444 MHz while a job recognizes, but `ocr_boost_clock = off` and a refused set call keep
the game's. VOCR5's shapes inside those sizes (vita-vn-ocr `eval/vocr5_protocol.json`, the model sizes since this
package): Small 819,792 B, Normal 1,185,632 B, Large 2,473,696 B, an estimated 0.63, 0.92 and 2.08 s per 3-line box
on one core at 333 MHz without prefetch credit (0.50, 0.72 and 1.63 s at 444 MHz). Since API 42 each carries its
1,792 B head gate (vita-vn-ocr #120), which skips the output layer on about three frames in five, and Normal's and
Large's their two-level head too (#122: 821,584, 1,189,408 and 2,477,472 B), whose rare output rows run only on the
frames whose escape fires; with both on (`ocr_head_gate = on`, the default) an estimated 0.51, 0.72 and 1.82 s at
333 MHz (0.41, 0.57 and 1.43 s at 444 MHz; the gate alone 0.51, 0.78 and 1.91 s;
[head gate and two-level head](vita-vn-ocr.md#head-gate-and-two-level-head-api-42)).

The largest int8 weights whose 3-line box (3 x 640-px lines = 240 CTC frames, plus 0.1 s for the finder and the
strips) stays under each tier's time, with the LM's top 8: random-init blobs of vita-vn-ocr's `vita/model_tiers.py`
width family (5,631 classes, bottleneck 96, 1.6-6.2 MB) and the released FL10/H15, each profiled with
`vita/a9_layers.py` on 24 development lines of 60-100 frames, on `main`'s runtime and on #85's, calibrated to
test.1's per-line rate (H15 on its own code). Each cell: without credit for #85's weight prefetch (the model's cache
without the `PLD` fills: the bound if `PLD` does nothing), then with it. Two cores: two lines on each. Estimates.

| tier (box) | before (`main`), 1 core, 333 MHz | after, 1 core, 333 MHz (`ocr_boost_clock = off`) | after, 1 core, 444 MHz (the default) | after, 2 cores free, 333 MHz | after, 2 cores free, 444 MHz |
|---|---:|---:|---:|---:|---:|
| Small (< 1 s) | 0.87 MB | 1.26-1.45 MB: **the < 1 MB weight cap binds** | 1.61-1.87 MB: the cap binds | 1.73-2.08 MB | 2.07-2.57 MB |
| Normal (< 2 s) | 1.74 MB | **2.15-2.71 MB** (3.5-4.6 M MACs/frame) | 2.42-3.40 MB (4.0-5.9 M) | 2.63-3.79 MB | 3.08-4.89 MB |
| Large (< 3 s) | 2.32 MB | **2.65-3.84 MB** (4.4-6.8 M MACs/frame) | 3.12-4.96 MB (5.4-9.0 M) | 3.62-5.60 MB | 4.47 MB to over 6.2 MB |

The "before" column is the same method on `main`'s runtime; it is close to the owner's 18:09Z ceilings (Small < 1 MB,
Normal ≤ 1.6 MB, Large ≤ 2.5 MB), which were set from the same console figure. FL10's box with the LM: 0.82 s → 0.51-0.57 s; H15:
1.32 → 0.80-0.91 s (1 core, 333 MHz).

The safe time ceilings are the low ends of the 1-core, clock-off column: a 1 s box allows 1.26 MB (Normal's sizing),
a 2 s box 2.15 MB, a 3 s box 2.65 MB; the caps bind for Small (< 1 MB) and Large (2.5 MB). They hold whatever the game
does with its cores, even with `ocr_boost_clock = off` or a game whose clock stays at 333 MHz. Above the 2 MiB L2 (shared with the
game) the weight stream's DRAM misses decide: there the low ends are the measured cost model without `PLD`, and the
calibration on H15 (which fits) is least certain. The high ends need the prefetch to work as modelled; the 444 MHz
column is the default configuration's, the 2-core columns need free cores, all unmeasured on the console until
test.3's `speed`, `par`, `clock` and `pace` lines are read.

## Not measured

- The console has run none of this: the times are the A9 cost model calibrated to test.1's per-line rate. The next
  test build's `par`, `pace` and `speed` lines measure it.
- How many cores a game leaves free, and whether two or three NEON threads on the shared L2 slow the game's frames:
  the `pace` step's job window against the window before it answers it per job.
- Whether a new thread starts with the worker's FPSCR (the SDK says only that threads can use VFP/NEON by default;
  nothing here sets FPSCR). The self-test settles it, and measures the helpers' gain, on the console: its `textbox`
  stage runs with the configured helpers and `textbox2` the same region again serially, neither with the line cache,
  and `tools/analyse_selftest.py`'s `parallel` row compares their texts (a checksum in each stage's record) and
  recognition times ([self-test](self-test.md)).
- CPU3 (unmeasured until PSVshell readings with matched clocks, plugin set and workload exist).
