# Stutter: why the game's frames come late while Rubi reads, and what changes

The owner (2026-10-19): "Reduce the stutter. In the test.3 logs, 291 of 1,609 frames (about 18%) came
late during a reading." This is where those late frames come from, what changes (API 29), and how a console
run checks it. The diagnosis and the console test are in [#225](https://github.com/bee-san/Rubi_for_vita/issues/225).
Every figure is measured from the owner's console logs or on the host, or marked est. The logs hold the
owner's reading, so only counts are quoted.

## Where the late frames are

**The data.** The kernel's pace step (the debug build's, API 22 on) gives each job's frames, from its claim to
its completion, and the frames more than 25 ms after the one before ("late"), and the same since the job
before. Two collects have it:
- test.3's (`vjo-collect-20261016-071743Z`, its own run): 28 jobs;
- test.4's (`vjo-collect-20261017-112501Z`): its `log.old.txt` and `log.txt` hold test.3's later play and
  test.4's run, 294 jobs (271 of test.3's build, 23 of test.4's).

Every job is CLANNAD's (`PCSG00415`), in **Automatically**. Each job's own lines add its strips
(`game vocr capture: … N strips`), its stable-text wait (`gstep job settle`: passes, time), its tries and what
started it (`job start (…)`). No test.5 console log exists.

| | test.3's run | test.3 + test.4 logs |
|---|---:|---:|
| late frames during jobs | **291 of 1,609 (18.1%)** | **2,142 of 17,160 (12.5%)** |
| late frames between jobs | 52 of 26,941 (0.2%) | 406 of 218,046 (0.2%) |
| frames during jobs whose display call copied a strip | 1,258 (78%) | 13,372 (78%) |
| late frames a strip | 0.23 | 0.16 |
| the stable-text wait's share of the strips | 78.0% | 78.4% |
| the wait's passes a second of waiting | 16.1 | 17.7 |

**The late frames come with the strip copies.** A job reads its region a strip at a time, at most 64 rows,
and the display hook copies each strip from the next frame the game submits, inside the game's own
`sceDisplaySetFrameBuf` call (`kernel/display.c`, [in-game capture](ingame-capture.md)). Until API 29 that
happened before the frame was set. A least-squares fit over the jobs, without a constant, of each job's late
frames against its frames with and without a strip:

| | per frame with a strip | per frame without | R² |
|---|---:|---:|---:|
| test.3's run (28 jobs) | 0.247 | −0.09 | 0.72 |
| test.3 + test.4 (294 jobs) | 0.156 | −0.03 | 0.28 |

The strips account for all of the excess. What a copy costs in the hook is not logged. Estimates: its grey
conversion takes 0.7-1.9 ms a 52-row strip at 444 MHz ([performance.md](performance.md), est.), plus the
uncached reads of the framebuffer, whose cost is unknown. The nearest console figure is a different routine:
the self-test's probe reads one 64-row strip from user mode in 14.4-16.1 ms (measured).

**Not the recognition.** The worker and its two helpers run at the lowest user priority on cores 0-2
(`game_ocr/main.c` `WORKER_PRIORITY`, `WORKER_AFFINITY`), so every game thread preempts them. Frames during
recognition were late no more often than the job's other frames. The 444 MHz boost (232 jobs) and the 3 jobs
that read weights or the LM from the card show no effect either, once the strips are counted. A user thread
cannot name the fourth core (CPU3) [SDK: Kernel-Reference, 'sceKernelCreateThread'], and CPU3 use is
unmeasured (AGENTS.md rule 8). The game's own threads' priorities are unknown: no CLANNAD dump exists.

**The stable-text wait reads most strips.** 78% of the strips are its passes, read back to back: about 17
passes a second, which at 3 strips a pass is nearly every frame. Most of them come while CLANNAD's box
animates for about a second after a new line ([text-stability.md](text-stability.md#what-holds-the-reads-back)).
The 103 of 294 jobs that were torn and read again, which wait again, cost 56% of the late frames.

**Background jobs cause most of them; presses suffer most:**

| what started the job | jobs | late frames during it |
|---|---:|---:|
| Automatically (background) | 272 | 1,947 of 16,185 (12.0%), 91% of all |
| a press (the overlay's job) | 16 | 167 of 739 (22.6%) |
| the open overlay's refresh | 4 | 25 of 113 (22.1%) |
| the self-test | 2 | 3 of 123 (2.4%) |

A press's job waits first on a screen still changing, so nearly all its frames carry a strip.

## What changes (API 29)

**1. The stable-text wait is paced** (`ocr_settle_pace_ms`, 150 by default, `config.ini` only). Each pass of
the wait is ceil(height / strip rows) strips, and each strip is a game frame whose display call copies rows.
The rule is the same as before: two whole reads `ocr_settle_ms` apart that agree, every row, and the wait's
budgets are unchanged. Only how often it looks changes:
- **Which passes it reads.** Of the passes it would read back to back, the paced wait reads the first one at
  least 150 ms after the one before, or the first one that could end the wait, if that comes sooner. It
  sleeps in between (`sceKernelDelayThread`).
- **Never sooner.** The passes it leaves out could only have seen a change, never ended the wait, so it never
  ends sooner than back to back (`core/vocr_ocr.c` `settle_wait`). A typewriter's pause gets the same window.
- **Near the budget.** In the budget's last `ocr_settle_ms` + 150 ms the wait reads back to back again, so a
  change it saw up to a pace late can still hold before the budget runs out.
- **Which jobs.** Only the jobs nobody waits for: Automatically's, the subtitle strip's and the open overlay's
  refreshes (`shell/worker.c` `vocr_run_one`). A press's job waits back to back as before. Its time and its
  window against a typewriter's pause stay what they were, and so do the self-test's stages.
- **On the wire.** The request carries the pace in `VjoGameOcrRequest.speed` bits 8-15, in 10 ms
  (`VJO_GAME_OCR_S_PACE_*`). `0` reads back to back, as before.

**2. The wait's strips share the region's rows evenly.** A pass of the wait takes the same number of strips,
each with the same rows: CLANNAD's 852 × 135 box is 3 strips of 45 rows, where it read 3 of 64 (the last moved
up to row 71). That is 30% fewer bytes a pass and at most 45 rows a display call (`vjo_vocr_pass_rows`). The
first pass, the finder's passes and the check after them read as before.

**3. The game's frame is set before the copy.** The display hook calls the game's `sceDisplaySetFrameBuf`
first, then samples the region's signature and copies an armed strip (`kernel/display.c`). The pixels are the
same: they are only read, and the game draws into another buffer while this one is shown. A frame the call
refuses is not read. So a frame the game set in time is no longer held past its vblank by the copy (est.:
whether the firmware's call waits for the vblank itself, and what this saves, need the console).

**4. The analysis.**
- **`tools/analyse_selftest.py --collect`** prints `late frames during OCR jobs`: the late frames a strip, the
  wait's share of the strips and its passes a second, and the late frames by what started the job.
- **`tools/stutter_model.py --collect DIR`** replays a collect's jobs on the host through the core's own wait
  (`vjo-settle-bench --replay`), back to back and paced, and prints the strips and the late frames each
  would cost.

Nothing new is allocated, no thread is added and no page changes: the same blocks, the same ledger items.

## The owner's three ideas

**A cheap check before the OCR** (a hash or downsampled difference of the box; read once it has held for N
frames). The kernel already does this before every Automatically job:
- **The kernel's check.** A 25 × 5 cell hash of CLANNAD's box every 8th frame; a job starts only after 300 ms
  without a change (settled) or 400 ms of changes in one 3 × 3 spot (quiet).
- **What is left.** The worker's own full-resolution passes during its stable-text wait, back to back: the
  measured cause. Pacing them does what a second, coarser hash would do, with one rule.
- **#211.** It is answered already: 0 of 238 dialogue pairs were read before they had finished
  ([text-stability.md](text-stability.md)), and the paced wait's window against a typewriter's pause is
  main's (below).

**Chunked recognition, or the worker on the core the game uses least.** Not done: recognition costs no
measured frame (it yields to every game thread already), and user threads cannot run on CPU3.

**"Only when I press" for games that stutter, switched automatically.** Over 20 jobs in a row, CLANNAD's late
share during jobs ranged 5.5-23.1% before this change (median 11.6%). The replay estimates 17.1% at the
worst after it. A switch with a threshold above that would leave the one measured game in Automatically and
catch a game that suffers more. It needs the frame pacing in the normal build too, where the kernel compiles
it out today ([release-variants.md](release-variants.md)), so it is a change of its own.

## Measured on the host

**The wait on a simulated box** (`vjo-settle-bench --pace`, CLANNAD's 852 × 135 at 60 fps, a strip a frame:
est. for the console). "Strips" are the frames with a copy until the lines would be recognized. All but the
first row waited:

| screen | back to back | strips | paced at 150 ms | strips | read |
|---|---:|---:|---:|---:|---:|
| still, asked settled | 199 ms | 10 | 199 ms (+0) | 10 (+0%) | 25/25 |
| still, not settled when asked | 366 ms | 22 | 366 ms (+0) | 13 (−41%) | 25/25 |
| the box animates 1 s after its line, asked settled | 1,349 ms | 81 | 1,449 ms (+100) | 33 (−59%) | 25/25 |
| the same, not settled when asked | 1,316 ms | 79 | 1,366 ms (+50) | 28 (−65%) | 25/25 |
| typing, 1 character / 3 frames | 1,616 ms | 97 | 1,766 ms (+150) | 34 (−65%) | 25/25 |
| a blinking icon, 8 frames | 466 ms | 28 | 566 ms (+100) | 16 (−43%) | 25/25 |

**A typewriter's pause** (`vjo-settle-bench --pauses 150`, text-stability.md's replay: 12 of 25 characters
shown, the typing resuming after P frames). On 852 × 135 and 917 × 125, at 60 and 30 fps, asked settled or
not, and with frames of 16,667 and 16,683 µs (59.94 Hz):
- the longest pause still read whole and the shortest read as the 12 shown are main's in all 16 cases;
- at 16,667 µs a frame they are text-stability.md's table.

**The tests** (`host/tests/test_vocr_policy.c`, the core's own code on the simulated screen):
- **Never sooner.** Typing that stops at 16 points, with and without resuming, with and without an icon (64
  cases): the paced job's first pass starts no sooner, shows the same characters, and reads no more strips.
  The earlier, simpler pacing (a pass at most every 150 ms) fails it.
- **The budget.** Typing until 1.25 s against budgets of 1.30-1.70 s: wherever back to back holds still
  within its budget, the paced wait holds too. Without the budget rule it does not.
- **Back to back.** A pace of 0 reads exactly as no pace.
- **No sleep.** Without a sleep callback the passes follow each other.
- **The limit.** A screen that never holds still reaches the budget as before.
- **Cancelled asleep.** A cancellation during a sleep ends the job.
- **The strips.** The wait's strips share the rows; every other read is as before.
- **The hook.** `test_kernel_display.c` pins the hook's order: the frame is set, then the signature is
  sampled, then the rows are copied; a refused frame is not read. On main's order 3 of its 5 cases fail.

**The replay of the console's jobs** (`tools/stutter_model.py`, est.):
- **The method.** Each background job that waited is replayed as a box whose rows change every frame until
  its wait's last change, then hold still, at the job's own frame rate. Its late frames are estimated at its
  own measured late frames a strip. Presses and the self-test are kept as measured.
- **The check.** Replayed back to back, the jobs read 3,240 passes, against 3,337 measured (a median error
  of 3% a job).

| | test.3's run | test.3 + test.4 logs |
|---|---:|---:|
| background jobs replayed | 12 | 220 |
| strips | 1,258 → 862 (−31.5%) | 13,372 → 7,378 (−44.8%) |
| late frames during jobs | 18.1% → est. 12.1% | 12.5% → est. 7.2% |
| Automatically's jobs | 18.6% → est. 9.4% | 12.0% → est. 6.5% |
| presses (not paced) | 21.1% | 22.6% |
| the worst 20 jobs in a row | 19.6% → est. 12.9% | 23.1% → est. 17.1% |
| a replayed job's wait, longer by | median 59 ms (0 to 128) | median 51 ms (0 to 150) |

No replayed job ends sooner, reaches its limit where it held, or tries more often. The balanced strips (2)
and setting the frame first (3) are not in these estimates: they change what each strip costs, which only
the console measures.

What this costs: the background result can come up to a pace later (median est. +51 ms; a press that finds
a background job still running waits that much longer), and the worker sleeps more and reads less. A press's
own job is unchanged.

## Console test (the next test build: hardware testing is paused)

With the debug build, CLANNAD or any game, **Read text: Automatically**:
1. **Play.** Ten minutes or more as usual (new lines, a few presses), then
   `bash tools/install_ftp.sh --collect VITA_IP`.
2. **Read the analysis.** Run `python3 tools/analyse_selftest.py --collect build/device/collect/vjo-collect-STAMP`
   and read `late frames during OCR jobs`:

   | | before (test.3 + test.4) | after, expected |
   |---|---:|---:|
   | late frames during jobs | 12.5% | about 7% (est.) |
   | the wait's passes a second | 17.7 | at most about 7 |
   | the wait's share of the strips | 78.4% | well under 78% |
   | late frames a strip | 0.16 | under 0.16 if (2) and (3) help |

3. **The pace alone.** For a like-for-like check of (1), set `ocr_settle_pace_ms = 0` in `config.ini` for a
   second stretch (it applies at the next resume): its wait reads back to back again, and only (2) and (3)
   remain.
4. **The replay.** `python3 tools/stutter_model.py --collect …` on the new collect: its `replay at pace 0`
   line checks the replay against that run.

Unmeasured until then: what a strip's copy costs in the hook, whether setting the frame first saves any of
it, and CPU3.
