# Stutter: why the game's frames come late while Rubi reads, and what changes

The owner (2026-10-19): "Reduce the stutter. In the test.3 logs, 291 of 1,609 frames (about 18%) came
late during a reading." This is where those late frames come from, what changes (API 29), and how a console
run checks it. The diagnosis and the console test are in [#225](https://github.com/bee-san/Rubi_for_vita/issues/225).
Every figure is measured from the owner's console logs or on the host, or marked est. The logs hold the
owner's reading, so only counts are quoted.

**Waiting for a game with motion.** The owner (2026-10-19 12:45Z) sees no stutter in CLANNAD, a static
slideshow VN: there a late frame shows the same still picture a little longer (inferred). The late frames
below are measured all the same, and every one of them is CLANNAD's. Whether a game with motion (scrolling,
animated sprites, video) shows them, and whether this change helps there, needs a console run in such a game.
Until then the pull request stays a draft.

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

**The late frames rise with the strips.** A job reads its region a strip at a time, at most 64 rows, and the
display hook copies each strip from the next frame the game submits, inside the game's own
`sceDisplaySetFrameBuf` call (`kernel/display.c`, [in-game capture](ingame-capture.md)).
- **By how much of a job's frames carry a strip.** Taking the 294 jobs in quartiles of their strips a frame,
  the late share rises with it while the late frames a strip stay flat:

  | strips a frame | 0.45 | 0.72 | 0.83 | 0.94 |
  |---|---:|---:|---:|---:|
  | late frames during the jobs | 7.5% | 11.2% | 12.8% | 15.9% |
  | late frames a strip | 0.165 | 0.155 | 0.155 | 0.169 |

- **A fit.** A least-squares fit of each job's late frames against its frames with and without a strip (no
  constant) gives 0.247 and −0.09 a frame for test.3's run (R² 0.72), 0.156 and −0.03 for the 294 jobs
  (R² 0.28): the frames without a strip show none. Job length alone fits nearly as well (R² 0.23), as the
  strips are 78% of the frames.
- **Not every strip costs the same.** Fitting the wait's strips apart from the job's other strips (its first
  pass, the finder's second, its lines) gives 0.126 late frames a wait strip (95% interval 0.093-0.163, 2,000
  bootstrap resamples of the jobs) against 0.372 (0.236-0.505) for the others.
- **The open overlay adds its own.** Jobs with the overlay open (presses, refreshes) had about 0.08 late frames
  a frame more than their strips account for.

What a copy costs in the hook is not logged. Estimates: its grey conversion takes 0.7-1.9 ms a 52-row strip at
444 MHz ([performance.md](performance.md), est.), plus the uncached reads of the framebuffer, whose cost is
unknown. The nearest console figure is a different routine: the self-test's probe reads one 64-row strip from
user mode in 14.4-16.1 ms (measured).

**Nothing beyond the strips.** In the fits, recognition, the 444 MHz boost (232 jobs) and the 4 jobs that read
their weights from the card (3 of them the LM too) add no late frames beyond their strips:
- **Priority.** The worker and its two helpers run at the lowest user priority on cores 0-2
  (`game_ocr/main.c` `WORKER_PRIORITY`, `WORKER_AFFINITY`), so every game thread preempts them.
- **Recognition's own reads.** Its lines' strips are among the costlier ones (above).
- **CPU3.** A user thread cannot name the fourth core [SDK: Kernel-Reference, 'sceKernelCreateThread'], and
  CPU3 use is unmeasured (AGENTS.md rule 8).
- **The game's threads.** Their priorities are unknown: no CLANNAD dump exists.

**The stable-text wait reads most strips.** 78% of the strips are its passes, read back to back: about 17
passes a second, which at 3 strips a pass is nearly every frame. The count includes the check after the
finder (`settle_verify`), which the pace leaves as it is. Most of the wait's passes come while CLANNAD's box
animates for about a second after a new line ([text-stability.md](text-stability.md#what-holds-the-reads-back)).
The 103 of 294 jobs that were torn and read again, which wait again, cost 56% of the late frames.

**Background jobs cause most of them; presses suffer most:**

| what started the job | jobs | late frames during it |
|---|---:|---:|
| Automatically (background) | 272 | 1,947 of 16,185 (12.0%), 91% of all |
| a press (the overlay's job) | 16 | 167 of 739 (22.6%) |
| the open overlay's refresh | 4 | 25 of 113 (22.1%) |
| the self-test | 2 | 3 of 123 (2.4%) |

A press's job waits first on a screen still changing, so nearly all its frames carry a strip. That explains
only part of its share: at the background jobs' late frames a strip its 637 strips would give about 99 late
frames, against 167. The rest comes with the open overlay.

## What changes (API 29)

**1. The stable-text wait is paced** (`ocr_settle_pace_ms`, 150 by default, `config.ini` only). Each pass of
the wait is ceil(height / strip rows) strips, and each strip is a game frame whose display call copies rows.
The rule is the same as before: two whole reads `ocr_settle_ms` apart that agree, every row, and the wait's
budgets are unchanged. Only how often it looks changes:
- **Which passes it reads.** Of the passes it would read back to back, the paced wait reads the first one at
  least 150 ms after the one before, or the first one that could end the wait, if that comes sooner. It
  sleeps in between (`sceKernelDelayThread`). No gap is longer than about `ocr_settle_ms`, so a pace above it
  counts as `ocr_settle_ms`.
- **Never sooner for typing.** A pass ends the wait when it starts `ocr_settle_ms` after the last pass that saw
  a change and sees none. A change is seen by any later pass, as the rows keep their checksums, so rows that
  only move on to new states (text typing out, a box animating to its end) end the wait no sooner than back to
  back (`core/vocr_ocr.c` `settle_wait`). A typewriter's pause gets the same window. The grid comes from the
  last pass's time, so on a console, whose passes vary, a paced pass lands near back to back's, not exactly on
  it.
- **Icons faster than the pace.** A row that changes and goes back between two paced reads is not seen
  changing. An icon that toggles 8 or more times a second can therefore end the paced wait sooner. The finder's
  pass may then see it as a tear and the job reads again: more strips, the same lines. On the host's screen it
  happened in 3 of 160 icon cases (a blink every 5 frames, 3 states every 6 or 7 frames). The console's icons
  returned a median 2.2 times a row a second, at most 9.9 (156 waits with icon rows in test.3 + test.4's logs).
- **Near the budget.** In the budget's last `ocr_settle_ms` + the pace + one pass, the wait reads back to back
  again. A change it saw up to a pace late can then still hold before the budget runs out, wherever back to
  back would hold.
- **Which jobs.** Only the jobs nobody waits for: Automatically's, the subtitle strip's and the open overlay's
  refreshes (`shell/worker.c` `vocr_run_one`). A press's job waits back to back as before: its time and its
  window against a typewriter's pause stay what they were, and so do the self-test's stages. A press that
  finds a background job running waits for its end, as before, which may now come up to a pace later. A
  cancelled job stops after its current sleep.
- **On the wire.** The request carries the pace in `VjoGameOcrRequest.speed` bits 8-15, in 10 ms
  (`VJO_GAME_OCR_S_PACE_*`); `0` reads back to back, as before. The settle step's `kind` counts the wait's
  sleeps, so a log shows which jobs were paced.

**2. The wait's strips share the region's rows evenly.** A pass of the wait takes the same number of strips,
each with the same rows: CLANNAD's 852 × 135 box is 3 strips of 45 rows, where it read 3 of 64 (the last moved
up to row 71). That is 30% fewer bytes a pass and at most 45 rows a display call (`vjo_vocr_pass_rows`). The
first pass, the finder's passes and the check after them read as before.

**3. The analysis.**
- **`tools/analyse_selftest.py --collect`** prints `late frames during OCR jobs`: the late frames a strip, the
  wait's share of the strips and its passes a second, the jobs paced, and the late frames by what started the
  job.
- **`tools/stutter_model.py --collect DIR`** replays a collect's jobs on the host through the core's own wait
  (`vjo-settle-bench --replay`), back to back and paced, and prints the strips and the late frames each would
  cost.

Nothing new is allocated, no thread is added and no page changes: the same blocks, the same ledger items.

**Not in this change: the frame set before the copy.** Copying a strip after the hook passes the call on,
instead of before, would keep the copy from coming between the game and its frame. But other plugins draw into
the same frame in the same hook, before they pass it on: PSVshell's FPS counter and OSD and reVita's menu and
popups [source: PSVshell 9a47b0e `src/main.c`, reVita f35adc5 `reVita/src/main.c`]. taiHEN runs the hook added
first and its continuation runs the others [source: taiHEN `patches.c` `hooks_add_hook`], and a one-VPK install
puts Rubi's kernel line first (`core/taicfg.c`); the owner's test.4 configuration lists it before PSVshell. So
Rubi now reads the game's own pixels, and read after the call its strips, its region signature and a card's
screenshot would hold those drawings (audit-stutter B1). `host/tests/test_kernel_display.c` pins the order.
Moving the copy would need its own change with a console measurement, one that leaves other hooks' drawings
out.

## The owner's three ideas

**A cheap check before the OCR** (a hash or downsampled difference of the box; read once it has held for N
frames). The kernel already does this before every Automatically job:
- **The kernel's check.** A 25 × 5 cell hash of CLANNAD's box every 8th frame; a job starts only after 300 ms
  without a change (settled) or 400 ms of changes in one 3 × 3 spot (quiet).
- **What is left.** The worker's own full-resolution passes during its stable-text wait, back to back: the
  measured cause. Pacing them reads less often by the same rule, without a second, coarser hash.
- **#211.** It is answered already: 0 of 238 dialogue pairs were read before they had finished
  ([text-stability.md](text-stability.md)), and the paced wait's window against a typewriter's pause is
  main's (below).

**Chunked recognition, or the worker on the core the game uses least.** Not done: recognition adds no late
frame beyond its strips (it yields to every game thread already), and user threads cannot run on CPU3.

**"Only when I press" for games that stutter, switched automatically.** Over 20 jobs in a row, CLANNAD's late
share during jobs ranged 5.5-23.1% before this change (median 11.6%); the replay estimates 17.2% at the worst
after it. A switch with a threshold above that would leave the one measured game in Automatically and catch a
game that suffers more. It needs the frame pacing in the normal build too, where the kernel compiles it out
today ([release-variants.md](release-variants.md)), so it is a change of its own.

## Measured on the host

**The wait on a simulated box** (`vjo-settle-bench --pace`, CLANNAD's 852 × 135 at 60 fps, a strip a frame:
est. for the console). "Strips" are the frames with a copy until the lines would be recognized. All but the
first row waited. A pace of 200 ms gives exactly what 150 does (above):

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
- **Never sooner on typing.** Typing that stops at 16 points, with and without resuming, with and without an
  icon (64 cases): the paced job's first pass starts no sooner, shows the same characters, and reads no more
  strips. The earlier, simpler pacing (a pass at most every 150 ms) fails it.
- **The budget.** Typing until 1.25 s against budgets of 1.30-1.70 s, on 2 strips a pass, and of 1.45-1.70 s in
  1 ms steps on 4 strips a pass: wherever back to back holds within its budget, the paced wait holds too.
  Without the budget rule it fails on the first. With a rule of `ocr_settle_ms` + the pace alone, without a
  pass's time, it fails on the second at 34 budgets (audit-stutter S2).
- **Icons.** 160 cases, blinking or of 3 states, 1-40 frames a state, asked settled or not: the same lines and
  characters in every case; the paced wait ends sooner in at most 3, all faster than 8 frames a state.
- **Back to back.** A pace of 0 reads exactly as no pace.
- **No sleep.** Without a sleep callback the passes follow each other.
- **The limit.** A screen that never holds still reaches the budget as before.
- **Cancelled asleep.** A cancellation during a sleep ends the job.
- **The strips.** The wait's strips share the rows; every other read is as before.
- **The hook.** `test_kernel_display.c`: the region is sampled and the rows copied before the call is passed
  on, so a later hook's drawing is never read. The copy-after order fails 3 of its 4 cases.

**The replay of the console's jobs** (`tools/stutter_model.py`, est.):
- **The method.** Each background job that waited is replayed as a box whose rows change every frame until
  its wait's last change, then hold still, at the job's own frame rate. Presses and the self-test are kept as
  measured. The replay has no icons, though 156 of the 294 waits had icon rows.
- **The check.** Replayed back to back, the jobs read 3,240 passes, against 3,337 measured (a median error of
  3% a job). This mostly re-derives the passes from each wait's measured time: a weak check.
- **The late frames.** Two estimates. At each job's own late frames a strip, which takes every strip of a job
  to cost the same: 7.2%. With each wait strip left out costing the fit's 0.126 late frames (0.093-0.163):
  7.8% (6.6-9.0%) (`--wait-strip-cost`). So about 7-9%, from 12.5%.

| | test.3's run | test.3 + test.4 logs |
|---|---:|---:|
| background jobs replayed | 12 | 220 |
| strips | 1,258 → 862 (−31.5%) | 13,372 → 7,393 (−44.7%) |
| late frames during jobs, est. | 18.1% → 12.1% (14.7% at the fit's cost) | 12.5% → 7.2% (7.8%; 6.6-9.0%) |
| Automatically's jobs, est. | 18.6% → 9.4% | 12.0% → 6.5% |
| presses (not paced) | 21.1% | 22.6% |
| the worst 20 jobs in a row, est. | 19.6% → 12.9% | 23.1% → 17.2% |
| a replayed job's wait, longer by | median 59 ms (0 to 128) | median 51 ms (0 to 150) |

No replayed job ends sooner, reaches its limit where it held, tries more often or publishes other characters.
The balanced strips (2) change what each strip costs, which only the console measures: they are not in these
estimates.

What this costs: the background result can come up to a pace later (median est. +51 ms; a press that finds
a background job still running waits that much longer). The worker sleeps more and reads less. A press's own
job is unchanged.

## Console test (the next test build: hardware testing is paused)

With the debug build, **a game with motion** (scrolling, animated sprites or a video behind the text: CLANNAD
shows none), **Read text: Automatically**:
1. **Play.** Ten minutes or more as usual (new lines, a few presses), then
   `bash tools/install_ftp.sh --collect VITA_IP`.
2. **Read the analysis.** Run `python3 tools/analyse_selftest.py --collect build/device/collect/vjo-collect-STAMP`
   and read `late frames during OCR jobs`:

   | | before (test.3 + test.4, CLANNAD) | after, expected (est.) |
   |---|---:|---:|
   | late frames during jobs | 12.5% | about 7-9% on the same kind of jobs |
   | the wait's passes a second | 17.7 | about 7 |
   | the wait's share of the strips | 78.4% | well under 78% |
   | jobs paced | none | most of Automatically's |

3. **The pace alone.** In the same game, set `ocr_settle_pace_ms = 0` in `config.ini` for a second stretch
   (it applies at the next resume): its wait reads back to back again, and the two stretches compare the pace
   alone, with the balanced strips in both.
4. **Whether it shows.** Late frames are what the log counts. Whether they are what stutters on the screen,
   in a game with motion, is the owner's to say.
5. **The replay.** `python3 tools/stutter_model.py --collect … --wait-strip-cost 0.126` on the new collect.
   Its `replay at pace 0` line checks the replay against that run.

Unmeasured until then: what a strip's copy costs in the hook, a game with motion, and CPU3.
