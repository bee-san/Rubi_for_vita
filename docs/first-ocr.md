# The first OCR: where test.2's 9-15 s went, and what test.3 changes

The owner's v0.8.0-test.2 run (#101, CLANNAD, build `4ac02ac`) felt broken: the first OCR took 10+ s,
later ones were fast. This is the per-job timeline of that run and of test.1's, what each big cost
was, with the code that caused it, and the fixes in this build. Every figure is measured from the
two runs' logs unless it is marked est.; the logs are private (only numbers are quoted here).

    python3 tools/analyse_timeline.py --collect build/device/collect/vjo-collect-STAMP --jobs
    python3 tools/analyse_timeline.py --compare OLD_COLLECT NEW_COLLECT

`--collect` (also written to `timeline.txt` in every collect zip) gives each game start's jobs,
their reads of the weights, the LM and the cascade's model, the self-test's stage runs, and per job
the game worker's stages, its frame pacing (API 22) and the Shell's time beside them, with the
appends that stood between the recognition and the result (`result path:`, est. at the job's own
cost per append; test.3's are in [release-variants.md](release-variants.md#no-log-append-on-the-result-path)).
It reads numbers only, never recognized text.

## Reading a job's step log

The game worker's steps (`gstep job ...`, `game_ocr/main.c` `step_add`) carry two kinds of `us`:

- a duration: `pass` (the first pass's strip waits), `find` (the finder, its strips included),
  `settle` (every stable-text wait of the job), `load` (the weights' block, read, SHA-256 and
  parse), `recognize`, `lm`, `cascade`, `norm`, `refine`, `blank`;
- a time since the job started, for the instant steps (`mem`, `check`, `alloc`, `free`, `sig`,
  `tear`, `clock`): `step_add` called with us 0 (`game_ocr/main.c:148` at `4ac02ac`).

Two of the owner's figures were such times, not durations: the "weights allocation of about
1.38 s" (`alloc ... arg=weights us=1376964`, an allocation made 1.38 s into the job) and the "tear
re-read 2.3 s" (`tear ... us=2298159`, the step written after the job's tries,
`game_ocr/main.c:987`: the job's end).

## test.2's first jobs

The game started at the Shell's t = 14.5 s. Its region first held text at t = 27.3 s (job 3's
first stage read a line there); the first text the overlay could show came at t = 81.8 s, the end
of job 7: 54.5 s later. The self-test was armed, and while it was, every job's OCR was its five
stages (`shell/worker.c:877`), each stage re-reading the screen with its own capture, stable-text
wait and weights, and a stage the screen changed under restarted the five on the next job
(`shell/worker.c:864`). The player read on, so jobs 3-6 were overtaken by the next line and showed
nothing:

| job | ms | what ran | text shown |
|---|---:|---|---|
| 1, 2 | 1,049, 689 | the first stage on screens without a line in the region (waits) | - |
| 3 | 3,822 | fallback stage (FL10 read 132 ms, LM 199, W20 307, cascade 519); textbox stage cancelled | no |
| 4 | 9,076 | fallback stage: the game's job 775 ms, the Shell 6,158 ms beside it (the overlay opened twice meanwhile) | no |
| 5 | 15,048 | fallback 3,927 ms, textbox 4,174 ms (13 rows torn, wait 1,129 ms, H15 read 426 ms), fullscreen cancelled | no |
| 6 | 13,695 | fallback 3,509 ms (wait limit 2,017 ms), textbox 2,299, fullscreen 2,805 (screen changed), probe 728 | no |
| 7 | 12,271 | all five stages: 1,259 + 2,639 + 1,678 + 613 + 934 ms, and their records | yes |

Over the run: 17 stage runs from t = 16.4 to 81.5 s (65.1 s): 38.6 s in the stages' exchanges and
logs (16.1 s of them the game's jobs), 13.5 s writing the stage records, and 13.0 s between stages
(the jobs' ends, the waits for the next settled screen). The user's own recognition on job
7's screen, the `textbox2` stage's game job, took 429 ms.

The second game start (a new CLANNAD process, the self-test done) had no self-test: two jobs
without a line (495, 446 ms), then the first text job in 2,233 ms, of which the game 1,415 ms
(H15 read 223 ms, the LM 201, the cascade's W20 299, recognition 192, a re-read by the cascade
275), the lookup 131 and the Shell 688.

test.1 (`v0.8.0-test.1`, API 19: no LM, no cascade) was the same in kind: its first job was the
self-test (11,168 ms, on the opening notice: nothing in the region), and the first text jobs of
its four game starts took 1,392, 1,242, 1,683 and 1,700 ms.

## Each cost, and its cause

**Capture 1.25 s.** Not a slow capture: 57 strips at 21.9 ms each. Each strip is copied by the
display hook from the next frame the game shows (`kernel/game_ocr.c:434`), so a strip costs a frame,
and a 112-row region is two strips a pass (64 rows a call). `capture_us` adds up every strip the job
reads (`core/vocr_ocr.c:402`), the stable-text wait's passes included: 48 of the 57 were the wait's
24 passes. The capture time and the settle wait are the same time, counted twice.

**The tear re-read, 2.3 s.** The 2.3 s was the job's end (above). The tear itself: 13 rows of the
region read a state their ring of earlier states did not hold, outside a wait, which is a tear
(`core/vocr_ocr.c:373-382`): the first try was dropped, and the retry waited first
(`core/vocr_ocr.c:1298-1299`) until the rows held still, 1,129 ms in 24 passes. The same 13 rows did
the same in every stage on that screen: 195-273 changes to new states per wait (13 × 15-21) and no
return to an earlier one, then stillness. Something in the box animated through about 20 states
for about a second after the kernel had called the region settled with one animated cell masked
(`region settled: ... (1 animated cells masked)` in kernel.txt): the kernel's scene tracker masks
it, the worker's row check does not. Its cost is the first try (its first pass, the finder and the
verify, est. 150-300 ms) and the wait.

**The weights allocation, 1.38 s.** A time, not a duration: the H15 block was allocated 1.377 s
into the job, when the first line was loaded, right after the retry's wait (`core/vocr_ocr.c:945`,
`:950`, the block taken in `resident_weights`, `:689`). It is `USER_RW` (`kind=user`), not phycont,
and the allocation is inside the `load` step, whose other three H15 reads (the same block, read and
hash) took 220-223 ms: no cold allocation or zeroing of note.

**The weights load, 425 ms.** H15's 1,155,968 B read through the kernel broker in 36 pieces of
32 KiB (`core/vocr_ocr.c:110`, `:568-579`; `kernel/game_ocr.c:898`) and SHA-256-checked: 221-223 ms
in every other H15 read of the run, 426 ms here; its recognition ran at 5,583 µs a frame, within the
5,184-5,879 µs a frame of the run's other H15 region jobs (the full-screen stage's 10,142 aside). The logs do not say what held the read up (the card, est.).

**The weights read 7 times in one process (FL10 4, H15 3), the LM and W20 twice.** The LM and W20 were read once per game process: test.2
had two (kernel.txt: pid 0x52AA7, then 0xA20E1), and a process's memory goes with it. That is the
design. The 7 weights reads (4 of FL10, 3 of H15, 1,387 ms) in the first process were the self-test
alternating its fallback and textbox stages: never two weight blocks (`core/vocr_ocr.c:671`), and a
job of the other model released them at its start, before it knew whether it would read anything
(`game_ocr/main.c:940`): after job 3's textbox stage, cancelled before its first pass was in, job 4
found no weights resident and read FL10 again. test.1 read H15 four times in one game process (232,
223, 219, 220 ms; kernel.txt: one pid, the game suspended and resumed between): around each suspend
and resume the worker found the game not the active one and released everything at once (the same
code in test.1's build; `game_ocr/main.c:1094-1097` at `4ac02ac`).

**The self-test's share.** In the first game start, everything: the 54.5 s from the first text to
the first text shown, the 9-15 s jobs, 6 of its 7 weights reads. Its stage records were
written line by line, each line opened, written and closed in the report (`shell/selftest.c:67`)
and then again in `log.txt` and `status.txt` through the line-by-line logger of that build
(`shell/log_vita.c:58`, `:101`): 0.4-2.4 s per stage record, 13.5 s in all.

**Not explained by the logs.** In jobs 4 and 18 the Shell took 6.2 and 6.8 s beside a game job of
0.8 and 2.5 s, both with the overlay opened while they ran (in job 11, also opened, 0.5 s). The log
does not say whether the time went to the exchange (the worker taking the job late: it runs at the
lowest priority) or to the log's appends. This build's done line splits it:
`job 0 done rc=0 in T ms, ..., game G ms, log L ms in K appends` (`G`: the exchanges with the game
worker; `L`: every thread's file appends meanwhile).

## What test.3 changes

1. **The self-test never runs in a job the user waits for.** Its stages run in a background job of
   their own, after the first job that read text in the region, on that screen, with the overlay
   closed and the Shell idle 2 s (`shell/worker.c` `selftest_job_due`), local engine only. It shows
   nothing and changes neither the shown text nor its lookup; opening the overlay stops it at once
   (the saved word shows without waiting), a new screen stops it as any job, and it runs again,
   from its first stage, after a later job with text. Its records are written a record at a time,
   one append per file (`shell/selftest.c` `Group`: 21 appends for the 132 lines of a passing run).
2. **Each file read once per game process.** A job that finds no line (title screens, menus, a box
   between two lines) reads the weights of the model size it runs (the configured one when it fits),
   the LM and the cascade's model (the next size up) if they are not resident (`core/vocr_ocr.c`
   `vjo_vocr_warm`, called from `vjo_game_ocr_run_request`), within the memory its check allowed, so
   the first job with text reads none of them; never the self-test's forced smallest size's. A job
   of another size keeps the resident weights until it reads its own, unless it needs their memory
   beside its own size's reserve. The resident blocks stay through a moment's not-active game (a
   resume, a system dialog): they go back after 5 s of the game's running time (`game_ocr/main.c`
   `inactive_long`; process time stops while a process is suspended [SDK: Kernel-Reference,
   sceKernelGetProcessTimeWide]). Nothing new is allocated: the same blocks, the same ledger items,
   the same pages.
3. **A press waits at most 1 s for the text to hold still** (`ocr_settle_press_ms = 1000`;
   `ocr_settle_max_ms`, 2 s, for every other job). In test.2 the two press jobs waited 1,265 and
   1,490 ms for text typing out (365 and 400 changes). There a 1 s cap would not have helped: the job
   reads at 1 s, a row still typing tears that read, the job tries again without waiting (each try
   about 0.15-0.25 s), and if all three tries tear, a new job follows at once with a fresh wait: est.
   from 0.1 s faster to 0.4 s slower on those two presses. It saves up to 1 s only on a region that
   keeps changing for longer than 1 s. It is test.3's experiment: the press jobs' settle steps
   (`limit` against `held`), their tears and tries (`tools/analyse_timeline.py --jobs`) decide whether
   it stays. In test.3, 1 of the 9 jobs that waited under the press cap reached it (1,041 ms; the
   others held after 0.4-1.0 s); how each mode's presses fared is in
   [auto-vs-press.md](auto-vs-press.md#from-the-press-to-the-popup).
4. **Measured, not guessed, next time.** The TEAR step's `bytes` is when the last try began (us since
   the job's start), the warm-up's LOAD step has `arg=1`, and the done line has the exchange and log
   times.

Not changed: a strip still costs a frame (a pass of a region of more than 64 rows, two frames; one
strip a pass needs a capture block above the 64 KiB budget and a kernel API change), and a box whose
animation the worker has not seen still tears the first try (the kernel knows the masked cell; the
worker would need it through the API). #93 (parallel lines, the line cache, 444 MHz while reading)
and #102 (the hand-off without polls) come on top of this.

## Estimates for test.3 (est., from test.2's measured parts)

| | test.2 (measured) | test.3 (est.) |
|---|---:|---:|
| first game start: first text shown after the region first held text | 54.5 s | 1.0-1.8 s |
| first game start: the first job with text | 12,271 ms | 1,000-1,800 ms |
| second game start: the first job with text | 2,233 ms | about 1,510 ms |

The first game start's first text job, on job 3's screen without the self-test: find 82 + wait 110
+ recognition about 371 (H15 at 5.3 ms a frame, 70 frames) + a re-read by the cascade if H15 doubts
the line as FL10 did (0-519) + decoding and the rest about 40 ms, the reads done by jobs 1 and 2,
which found no line: 0.6-1.1 s in the game, and the Shell's 400-700 ms of test.2's other jobs. The second game start's: 2,233 ms less its
723 ms of reads, which jobs 14 and 15 (no line) make instead. A press while the self-test's job runs
waits for it to stop: the worker ends the stage within a strip or a line's tile, then the stage's log
batch, its record and the done line (4-7 file appends at 12-46 ms) and two 50 ms polls: est. about
0.2 s typically, at most about 0.5 s. Unmeasured until
test.3: all of these, CPU3, and what the 6.2-6.8 s Shell time was.
