# Read text: automatically, or only when you press L + R

The Rubi app's **OCR & lookup** page has two settings for how the overlay reads the screen. Both are
saved to `ux0:data/VitaJPOverlay/config.ini` (`settings/preferences.c` `vjo_preferences_save_reading`):

| In the app | `config.ini` | Choices |
|---|---|---|
| **Read text** | `ocr_mode` | **Automatically (when it changes)**: `auto`, the default. **Only when I press L + R**: `on_press` |
| **Speed boost while reading (444 MHz)** | `ocr_boost_clock` | ON: `on`, the default. OFF: `off` |

The Shell plugin reads `config.ini` again when a game starts or resumes and each time the overlay opens
(`shell/worker.c` `activate_game` and `open_overlay`, both through `apply_config`), so a choice made in
the app applies from the next resume. The second Read text button names whatever opens the overlay
(`toggle_button`: L + R unless changed, Select, Start, Select + L or R, or a double tap on the back).
An update keeps both choices: `tools/install-vita.sh` no longer sets `ocr_mode = auto` (it did before
this change), and `tools/migrate_config.py` keeps every value. The wait settings below stay in
`config.ini` only: `ocr_settle_ms` (150), `ocr_settle_max_ms` (2000) and `ocr_settle_press_ms` (1000).

## What each mode does

**Automatically.** With the overlay closed, a screen whose text region has settled is read in the
background (`shell/worker.c` `auto_prefetch`, a job `auto: screen settled` or `auto: screen quiet`), and
with Whole word highlighting its first word is looked up too (`want_lookup`). L + R then finds one of:

- the word already looked up for the screen shown (`open: saved local word`) or a finished background
  result (`open: background result`): the popup at once;
- a background job still running (`open: background job still running`): the popup when it ends;
- nothing for this screen (`open: no background result`, `open: screen changed since the background
  result`): a job starts on the press (`job start (overlay opened)`).

While the overlay is open, a newly settled screen is read again in the background and replaces the text
quietly when it reads differently (`background_open`: 2 s apart, 1.5 s after the last lookup).

API 41: in a game whose hook answers without OCR (a Luna site, or a source Rubi has learned), the hook's
new line starts that background read when the game copies it, before the line has finished typing, and
the scene it is read for holds while the line draws, so L + R as the line finishes typing usually finds
the finished result ([native-hooks.md](native-hooks.md#a-new-line-starts-the-read)).

**Only when I press.** Nothing is read while the overlay is closed (`background_wanted` is false). Each
press of a new screen starts a job (`overlay opened`), whose first word is looked up in the same job.
Reopening on the same screen shows the saved word at once, as in Automatically. Once the press's text is
shown, nothing reads the screen again while the overlay stays open (a press job that the screen changed
under is retried, as in Automatically).

Two things read in the background whatever `ocr_mode` says: the subtitle strip (Select + R), whose text
is the background jobs' (`background_wanted`: `subtitles || auto`), and an armed self-test, which forces
`auto` (`apply_config`).

**Speed boost while reading.** With it on, the game worker raises the ARM clock to 444 MHz while a job
recognizes lines and puts back the clock it found after; a clock already at 444 MHz or more (PSVshell's
500) is left as it is, and the bus and GPU clocks are never changed (`game_ocr/main.c` `job_busy`;
[OCR speed](ocr-speed.md#the-arm-clock-ocr_boost_clock-on)). With it off the job runs at the game's
clock and logs no clock step. It changes nothing but recognition: the waits below are frame-bound.

## What "settled" means

Two checks, one in the kernel and one in the game worker, decide when text is read.

**The kernel's scene tracker** (`kernel/scene.h`) hashes a grid of cells over the region on every frame
(32 × 4 on a text box). The region has **settled** when no cell has changed for 300 ms
(`SCENE_STABLE_US`), cells cycling like a blinking "next" icon masked (at most 8), and it is **quiet**
when for 400 ms (`SCENE_QUIET_US`) every change stayed in one spot of 3 × 3 cells that went back to a
recent state (an icon not masked yet). Either event starts Automatically's background job. A press
starts its job at once, whatever the tracker says; the tracker only decides whether that job waits
before its first pass (the region not settled) or checks after it (settled).

**The game worker's stable-text wait** (`core/vocr_ocr.c` `settle_wait`, `settle_verify`) runs in every
job, of both modes. Nothing is recognized until every row of the region has read the same for
`ocr_settle_ms` = 150 ms:

- A row is the same when the checksum (FNV-1a) of its grey pixels is unchanged. A row that goes back to
  one of its recent states (16 a row for a dialogue box, 6 at the full screen) is an animated icon, not
  a change, for at most 64 rows of a job (`VJO_VOCR_CYCLE_ROWS`): text typing out only moves on to new
  states.
- The region is read in passes of strips of up to 64 rows, and each strip costs a frame: the display
  hook copies it from the next frame the game shows ([in-game capture](ingame-capture.md)). CLANNAD's
  852 × 135 region is three strips a pass; in test.3 a wait of several passes took 56-73 ms a pass
  (each wait's time over its passes, measured).
- A job whose screen the kernel already called settled reads its first pass and finds its lines, then
  reads the region again until a whole pass that started at least 150 ms after the rows were first
  read the same has matched (`settle_verify`). Otherwise it waits first (`settle_wait`): passes without
  finding lines until a pass with no change starts 150 ms or more after the start of the last pass that
  saw one. With test.3's passes of 56-73 ms the rows have to hold still for three passes, 0.17-0.29 s
  (est.), after the last change.
- A job waits at most `ocr_settle_max_ms` = 2000 ms in all; a job the open overlay waits for (a press;
  not Automatically's own job, a refresh or the self-test's) at most `ocr_settle_press_ms` = 1000 ms
  (`shell/worker.c` `vocr_run_one`). When that runs out the job reads the region anyway.
- A row that changes after the wait, while the job finds or reads its lines, is a tear: that capture is
  dropped and the job tries again, waiting first, up to three captures (`VJO_VOCR_TRIES`), then fails
  with `VJO_E_SCREEN_CHANGED` and is retried quietly later.

**Reading before the line has finished.** A typewriter pause longer than the window (150 ms and about
one pass) passes the wait, and a pause longer than the whole job leaves its text with the line so far:
a row that changes while the job still reads the region is a tear, read again. A press's job waits from
the press, so the window is all it has; Automatically's background jobs start only once the kernel has
seen no change for 300 ms, so a pause has to be longer still for them. In Automatically, the kernel then
sees the typing resume, the screen settles again and the next job reads the whole line; the press shows
that one, or the open overlay's refresh replaces the text. With Only when I press, a press during such a
pause shows the part read, and nothing reads it again until the overlay is closed and opened again. The
host settle bench (`vjo-settle-bench 150`, a simulated 917 × 125 box at 60 fps, est.: the console's frame
pacing differs) shows both sides: text typing one character every 3 frames is read whole (25 of 25
characters, 1.6 s), and with 12 of 25 characters shown, a pause of 8 frames (133 ms) is waited out
(25 of 25) while a pause of 24 frames (400 ms) is read as settled (12 of 25).

`tools/analyse_selftest.py --collect` now reports such reads: **read before finished** counts, in each
game window, a job whose text the next job's began with and went on from, with both jobs' file and line
(`log.old.txt:N` or `log.txt:N`) and lengths and the first job's wait, never the text. It can also flag a
short line that begins the next screen's (an ellipsis before a line that starts with one), and it misses
a difference past a line's 60th character, the most a line's log entry keeps. It needs the recognized
text and the job lines in `log.txt`, which the debug build (the test builds') logs; the normal build's
short log has neither ([release-variants.md](release-variants.md)), and for its boots the analysis says
so instead. In test.3 (Automatically, CLANNAD) it found none: 0 of 18 pairs, and 2 jobs that read the
same text again. Over all of test.2's, test.3's and test.4's logs it is 1 of 299 pairs, a settings menu
and no dialogue: [text-stability.md](text-stability.md) has the counts and why no fix or model is needed.

## From the press to the popup

Measured in test.3 (v0.8.0-test.3, `9cfa920`, CLANNAD `PCSG00415`; #138's collect,
`vjo-collect-20261016-071743Z.zip`, this boot's part of `log.txt` from line 10630, read with this
change's `tools/analyse_selftest.py --collect`). Every press of that run was in Automatically; Only when
I press was not run. Times are the Shell's own, from its log lines: a job's text comes with its
`subtitle text ready in N ms` line and its first word with its `done` line, both counted from the job's
start and taken before that line's own log appends. Two gaps are not in them:

- **Before:** a press's job starts after the press's own handling (`shell/worker.c` `open_overlay`):
  its input block, `open:` and `job start` log lines, 3 to 6 file appends at 12.1-46.0 ms each
  ([performance.md](performance.md), #81's self-test timestamps), and `config.ini`'s re-read, est.
  0.04-0.3 s.
- **After:** `net_job` logs a job's `done` line, one append to `log.txt` and one to `status.txt`
  while that is under 64 KiB, before it tells the control thread, which notices within its 50 ms
  poll (`POLL_TIMEOUT_US`) and then shows the word.

A press that waited for a running job has no exact time, as the log has no time for a job's start. The
analysis gives that job's whole time plus 142 ms, the done line's two appends at their most measured
46 ms and the poll. For an OCR-only job (Highlight = Character, the subtitles), it also adds the lookup
of the word after it and 142 ms more. The job began before the press was handled, so this is an upper
bound (est., as the appends' times are) on when the Shell has the word from that moment; the overlay
draws it at its next frame. Two things can stretch it, neither seen in test.3's 7 presses:
- the press itself can come before the job's clock by up to the job start line's own appends and the
  kernel's 16 ms input poll: the log shows when the Shell handled a press, not when it was made;
- another thread's log line holding the log's lock when the `done` line is written can add its own
  appends, two while `status.txt` is open.

A job whose `done` line comes before the press's `open:` line ended during the press's own handling and
is not counted. A running job that failed or read no line, the self-test's job and a refresh are never a
press's wait. A job that read text and then failed its word's lookup is counted on its own line with
its rc: the overlay showed its text and why the word is missing. The rc can be -113 or -114 (the
dictionary file), -103 or -104 (the lookup's memory or its entry), or -103 when the Shell had no
result memory. A stale job is counted as no text. It was cancelled while the overlay was open (`job
cancelled: the screen changed since its capture`, a line that can also follow its `done` line), or
`on_job_text` passed over its text (cancelled, or another game's: no `subtitle text ready` line).
One cancelled after it posted its text showed that text only until its retry's "Recognizing…", or
with no retry until the close, and never its word. So a stale job is never a press's text or wait:
a later job of the press's own brings the text, or none does. A cancel after the press's close
changed nothing it showed and is not counted. A job cancelled by a change of `config.ini`'s pipeline
(the OCR, dictionary and selection settings: `apply_config`, `same_pipeline`) logs no line, so the
analysis cannot see it. A press closed before its text came, or whose overlay could not open
(no Paf memory for its text, or the page itself refused: `overlay page failed to open`), is not
timed; one closed after its text but before its word is still timed, its word by its job's end,
which the user did not wait for.

| What L + R found (19 presses) | Presses | Text | Word |
|---|---:|---|---|
| the saved word (at once) | 1 | a frame (not timed) | a frame (not timed) |
| a background job still running | 7 | - | at most 0.8-3.3 s after the press, median at most 2.8 s (est.) |
| nothing for this screen: a job on the press | 7 | 1.0-1.7 s after the job's start, median 1.3 s | 1.1-1.7 s after the job's start, median 1.4 s |
| a running job whose result was for an earlier screen, then a job | 1 | at least 1.4 s | at least 1.4 s |
| a job on the press without text | 2 | - | - |
| a job on the press, the overlay closed before its text (after 0.43 s) | 1 | not timed | not timed |

The two presses without text were in PCSG00940, which had no OCR worker registered (`rc=-117`): in
test.3's build the worker ran only in games with a taiHEN line; since #148 the kernel loads it into
every game that is ON ([worker-loading.md](worker-loading.md)).

In the seven timed presses that started a job, waiting for the rows to hold still took 0.6-1.0 s
(median 0.8 s; one reached the 1 s cap), with 17-29 row changes a pass: rows of the box were still
changing at the press. The log does not say whether that was text typing out or an animation of the box (test.2's
CLANNAD box animated for about a second after the kernel had called it settled:
[first-ocr.md](first-ocr.md#each-cost-and-its-cause)). Finding and recognizing the lines and looking up
the word took the rest.

Automatically's own 20 background jobs (15 with text) had their text 0.45-2.69 s (median 1.24 s) and
their word 0.51-3.18 s (median 1.33 s) after they started, which is 300 ms or more after the region last
changed (the kernel's settle); their stable-text waits took 0.08-2.06 s (median 0.78 s). So only 1 of
the 19 presses found its word ready: 8 found a background job still running (in one of them that job's
result was for an earlier screen), 4 the screen changed since the last background result and 6 no
background result. The log does not say what changed, or why there was no result.

**Each mode, from these figures:**

| | Automatically | Only when I press |
|---|---|---|
| Press after the background word is ready, same screen | at once (1 of test.3's 19 presses) | first press: a job, est. at most 1.1-1.7 s from its start and the press's handling before it (Automatically's press jobs, on screens still settling; a still screen's job waits less); reopening: at once |
| Press while the text is still settling | the rest of the background job, at most 0.8-3.3 s (est.; 7 of 19) | a job: 1.1-1.7 s to the word from its start, after the press's handling (est. 0.04-0.3 s; measured on Automatically's press jobs, the same path) |
| First press of a game process | as above: jobs that found no line read the weights before | est. 0.3-0.8 s more: the job reads the model (125-295 ms in test.3), the LM (184 ms) and, for a doubtful line, the next size's model (185-331 ms) itself |
| Work while you read | a job for every settled screen, at the lowest priority on the game's cores | none (subtitles off) |

The background jobs' cost to the game: during test.3's 28 jobs that logged the game's frame pacing,
291 of 1,609 frames came more than 25 ms after the last one, against 52 of 26,941 frames in the time
before each job (measured; `game frames during OCR jobs` in the analysis, press jobs included). Battery
and CPU3 use are unmeasured (AGENTS.md rule 8).

**The speed boost's share.** test.3 raised the clock in 22 jobs (333 → 444 → 333 MHz, measured); the
optional run with the boost off was not made, so what it saves is unmeasured. Of the 9 press jobs with
text, 6 recognized a line, and their recognition and the larger model's re-reads took 52-283 ms at
444 MHz (their `recognize` and `cascade` steps and clock steps, measured); the other 3 took every line
from the line cache, recognized nothing and raised no clock. At 333 MHz the 6 would take up to a third
longer if they scale with the clock (444 / 333 = 1.33, est.): 0.02-0.1 s more a press (est.). The stable-text wait and the strip reads,
most of a press's time, are frame-bound and do not change.

## What test.4 should measure

With a test build, which is the debug build (its `log.txt` has the job lines and the recognized text
these figures come from; the release's normal zip logs neither):
- One session each way: Read text **Automatically**, then **Only when I press** (the app's OCR & lookup
  page, then resume the game), on the same kind of screens, presses made right after the text appears
  and again a few seconds later. The analysis' `presses (the overlay opened) with ocr_mode ...` lines
  give each mode's figures as above, and `read before finished` says whether 150 ms read lines too early.
- The Speed boost OFF for a stretch of the Automatically session (test.3's step 6): `boost off against
  on` in the analysis compares the time per CTC frame.

Until then, every figure above for Only when I press is Automatically's press path or an estimate.
