# Reading time: the AFK rule and how it was chosen

The overlay's reading statistics (Triangle, [ocr-history.md](ocr-history.md#total-characters-read))
credit each recorded line with the time to the next recorded line of its game. What else that gap
holds, Rubi cannot see: a lookup, a slow line, or an AFK, a suspend, standby, the next day. The rule
that decides how much of a gap counts is `reading_time` in `config.ini`
([ocr-history.md](ocr-history.md#reading-time)). Rubi 0.9.7 counted a gap only if it was 15 s or less
(`15s`); since this change the default is `adaptive`. This page says how the rules were compared, and
how GameSentenceMiner, whose numbers Rubi's users compare theirs with, came to its own rule.

## How the rules were measured

No `history.csv` from a console was available (no collect fetches it), so the rules were measured on
simulated timelines: `tools/reading_time_sim.py`. It is a test harness, not a feature.

- **Readers**: 3,000, 6,000, 10,000 and 18,000 characters an hour, word lookups included (Poisson,
  0.06 to 0.002 a character, 4-20 s each), a line's time log-normal around the reader's pace (spread
  0.4), 0.6 s to press on. A text box has the characters of one of the 50 real VOCR regions of
  `host/fixtures/word_lookup` (6-44, mean 26.6), or 0-4 (6 %).
- **What happens besides reading**, per hour of reading: AFK breaks (4 of 20-120 s, 0.5 of 2-60 min;
  the Vita's own standby after 5 minutes), suspends (0.5-1 of 1-15 min), standby (0.25-0.5 of 5 min
  to 8 h), menus (3 of 5-40 s, their text recorded half the time), the backlog (1 of 1-4 old lines,
  each recorded 60 % of the time), skip mode (bursts of 20-200 lines at 0.1 s), the game's auto mode
  (10-50 % of lines, at about the reader's pace), a typewriter pause recorded as a part line (0.5 %),
  a refresh that reads a line differently (1 %).
- **What Rubi records** (shell/history.c, Read text: Automatically): one record per settled screen whose
  text is not the record before's (a blank read lets the same text be written again), timed at the
  read's start: 300 ms after the line finished typing (30 a second) for OCR, at once for a hook
  (API 41); a line the player pressed past before the read is not recorded; skipped lines are not
  recorded by OCR or an untrusted hook (they never settle, and such a hook's quick lines wake nobody), but
  a trusted hook (API 49) records one at each job ([Skip mode](#skip-mode)).
- **The truth**: the time on each read line, lookups and re-reading in the backlog included; no AFK,
  suspend, standby, menu or skipped line. Its characters: each read line once.

Eight scenarios (clean reading, AFK breaks, suspend and standby, menus and backlog, skip mode, auto mode,
and an everyday mix of all of them, by OCR and by hook), four readers, 12 runs of 4 hours each (skip mode with a
trusted hook has its own report: [Skip mode](#skip-mode)). Every
parameter is an assumption, so `--sensitivity` reruns the comparison with each changed. To reproduce:

```sh
python3 tools/reading_time_sim.py --seeds 12 --sensitivity
python3 tools/reading_time_sim.py --fixtures host/fixtures/reading_time   # the tests' timelines
python3 tools/reading_time_sim.py --skip                                  # skip mode's filter
```

`host/fixtures/reading_time/*.tsv` are eleven 1-hour timelines from it, with what each rule credits;
`host/tests/test_shell_history.c` appends them through the real writer and checks the Shell's counters
and a recount, and `tools/tests/test_reading_stats.py` checks `tools/reading_stats.py` and the error
against the truth.

## Results

Reading-time error (estimated / true − 1) over all 32 scenario-reader pairs, mean of the absolute
error, and the worst pair [estimates: simulated timelines]:

| rule | reading time: mean abs | worst | characters/hour: mean abs | worst |
|---|---:|---:|---:|---:|
| Rubi 0.9.7 overlay: a gap over 15 s is a break | 52.7 % | −93 % | 63.1 % | +149 % |
| Rubi 0.9.7 `reading_stats.py`: over 5 minutes | 4.2 % | +13 % | 3.5 % | −11 % |
| GameSentenceMiner 2025-09: every gap capped at 120 s | 5.1 % | +15 % | 4.2 % | −12 % |
| GameSentenceMiner 2025-12: capped at 60 s | 3.3 % | +10 % | 3.0 % | −8 % |
| GameSentenceMiner 2026-03: max(15 s, 3 s a character) + IQR | 4.1 % | +12 % | 3.4 % | −10 % |
| GameSentenceMiner 2026-06: 2.5 × the median pace | 4.2 % | −9 % | 4.6 % | +10 % |
| GameSentenceMiner 2026-07: 2.5 × half the median | 2.8 % | +11 % | 2.3 % | −9 % |
| GameSentenceMiner 2026-10 (now): 2.5 × the session median | 4.4 % | −9 % | 4.8 % | +10 % |
| JL: the line's characters at 10 a minute | 6.0 % | +16 % | 5.0 % | −14 % |
| a gap over 120 s is a break | 2.5 % | +10 % | 2.2 % | −8 % |
| **Rubi `adaptive`** | **2.2 %** | **+9 %** | **1.9 %** | **−7 %** |

The 15 s rule loses most of the reading time of anyone whose lines take more than 15 s, a beginner's
nearly all of it, and the lines it keeps are the fast ones, so characters/hour comes out high. Any rule
that keeps the longer lines does far better; among those, the ones whose cap follows the reader do best
on average, and the per-line caps are also the most stable when the assumptions change:

| rule | as assumed | spread 0.25 | spread 0.6 | no lookups | lookups ×1.5 | interruptions ×3 | ×3, no lookups |
|---|---:|---:|---:|---:|---:|---:|---:|
| Rubi 0.9.7 overlay (15 s) | 52.7 | 51.5 | 55.2 | 47.2 | 58.3 | 53.2 | 47.8 |
| GameSentenceMiner now | 4.4 | 4.4 | 5.3 | 1.5 | 9.0 | 4.6 | 4.3 |
| a gap over 120 s is a break | 2.5 | 2.4 | 2.5 | 2.7 | 2.5 | 6.7 | 7.1 |
| Rubi `adaptive` | 2.2 | 2.2 | 2.2 | 2.4 | 2.8 | 6.3 | 6.8 |

(mean absolute reading-time error, %.) Where each still errs: GameSentenceMiner's 2.5 × median cuts
lines with many lookups (−9 % for a beginner as assumed, −48 % with half as many lookups again); a
fixed 2-minute break counts fast readers' short AFKs whole (+20-28 % with three times the breaks);
Rubi's cap, which follows the slower quarter of the reader's lines, gives a beginner's AFKs more room
(+23-31 % with three times the breaks).

Smaller fixes, measured on the same grid with the adaptive rule:

- **Leaving out the gap around a suspend or standby** (the Shell sees the game leave the front, but
  `history.csv` does not, so a recount could not): 2.2 → 2.1 %. Not done: the cap already bounds such
  a gap to the line's expected time.
- **SceShell's process time instead of the clock** (it may stand still in standby; unverified on a
  console): 2.2 → 2.1 %. Not done.
- **Counting a line again in the backlog or after a menu once only** (a record whose text is one of the
  8 before it): characters 0.5 → 0.2 % (everyday OCR +3 → +1 %), time unchanged. Not done: it needs the
  last texts in the Shell, for a small gain.
- **Sessions** (GameSentenceMiner's 30-minute break: a longer gap credited nothing; measured on the running
  75th percentile, in floats, not with a median per session): 2.2 → 2.1 %. Not done, by
  the owner's decision: Rubi has no sessions.

## GameSentenceMiner's AFK rules, in order

[GameSentenceMiner](https://github.com/bpwhelan/GameSentenceMiner) went through most of these rules on
the way to its own. Its stats were added by bee-san in 2025; the commits are its repository's.

| when | commit | rule | why it changed |
|---|---|---|---|
| 2025-09-06 | `b36aedf7` "Add stats" | every gap at most `afk_timer_seconds`, 120 s (30-600 s), even overnight; sessions (gaps over 1 h) only counted | |
| 2025-09-13 | issue #185 | the timer was hard-coded to 120 s in places of `stats.html` | |
| 2025-12-17 | issue #321 "Sentence Length Aware AFK" | | AFK on 「ああ...」 with a 60 s timer gives 120 characters an hour; suggests [JL](https://github.com/rampaa/JL)'s per-length timer and the reader's own speed for that game, with clamping so a skip cannot set it |
| 2025-12-31 | `cb405a01` | the timer's default 60 s; a `minimum_chars_per_hour` (5,000) setting declared, never used | |
| 2026-03-13 | `6dd5b958` (plan `.opencode/plans/reading_speed.md`), `6906151d` (`test_afk_detection.py`: fixed, 1.2 s a character, an EMA × 3), `5c11e6b9` "remove afk timer" (and its setting); shipped in #394, #409 | each line at most max(15 s, 3 s a character), 300 s ceiling; then a line slower than Q1 − 1.5 IQR is re-timed at the median speed | the flat timer cut long lines, lookups and re-reading, so it understated time and overstated speed, and users had to tune it by hand |
| 2026-06-19 | `f4c5a955` "Adaptive reading-time v2" (a setting, `reading_time_adaptive_v2`: the commit message calls it opt-in, its default is on) | each line at most 2.5 × its expected time at the median pace of the lines (5+ characters), 2 s floor, 300 s ceiling, 3 s a character before a pace; time and characters credited together when the next line comes | short lines and pauses no longer over-count |
| 2026-07-05 | `e59edb51` | 2.5 × at 80 % of the median | so fast, easy lines do not make later caps too tight |
| 2026-07-10 | `ad677c4b` "Use a more conservative adaptive reading pace" | 2.5 × at 50 % of the median (5 × its expected time) | |
| 2026-09-30 | `f13f205f` | the adaptive rule for everyone; the 2026-03 rule and the setting removed | |
| 2026-10-01 | `9eba9390` "use consistent session pace" | back to 2.5 × the median, now per 30-minute session (a longer gap: no credit); a line teaches the pace only with 5+ characters and 1-300 s (`reading_speed_sample`); an empty line gets nothing | sub-second bursts measure text delivery, and a gap over the ceiling would teach a break as reading; filtering the samples, not loosening the cap, fixed the too-fast median |
| 2026-10-05 | `ca5eb642` | a live session expires after 30 minutes idle | |
| 2026-10-10 | `6d58dd08` | a gap across an explicit pause (text intake paused, from `fb5ccb00`) is neither credited nor a sample | |
| 2026-10-21 | `093dc8cd` | the live day's raw time: a gap over 30 minutes falls back to the adaptive cap | |

Rubi's `adaptive` takes the cap, its floor and ceiling, the 3 s fallback and the sample filter from the
current rule (`util/stats/stats_util.py` at `5190b613`), and, as issue #321 suggested, the pace per
game. It leaves out the sessions, and in place of the median of a session it keeps a running 75th
percentile, which needs no list of lines (the Shell keeps 16 bits) and measured best on the simulated
timelines (the median: 4.4 %; 0.8 or 0.5 of the median, GameSentenceMiner's July values: 2.1 and
2.8 %).

Other trackers: [exSTATic](https://github.com/KamWithK/exSTATic) stops its clock 60 s after the last line
(`afk_max_time`, `src/vn/vn.svelte`); JL stops it after the line's characters at
`MinCharactersPerMinuteBeforeStoppingTimeTracking` (10 a minute), at least 1.5 s
(`JL.Core/Statistics/StatsUtils.cs` `SetIdleTimeTimerInterval`, `3f7752ed`); ッツ Ebook Reader pauses its
tracker on reader events (Moderate) or any loss of focus (Strict), and after an idle time without page
interaction (off by default), rolling the idle time back (`book-reading-tracker.svelte`,
`settings-content.svelte`, `e7ec1d26`): the 15 s rule was that idle rollback at 15 s.

## Skip mode

Since API 49 a trusted hook's line (`hook_trust = on`, the default) wakes the Shell even while the line before it
still draws (`kernel/game_ocr.c`), so in the game's skip mode every line that arrives while no job is claimed becomes
a job and a record in `history.csv` ([#602](https://github.com/bee-san/Rubi_for_vita/issues/602)). Each such line
added its characters to the totals and was credited its fraction of a second, so characters and characters/hour
rose with every skip. OCR and untrusted hooks never record a skip: nothing settles, and such a hook's quick lines
wake nobody.

**The rule** (`core/history_lines.c` `vjo_reading_scope_add`, both `reading_time` rules; `tools/reading_stats.py`
`skipped_lines`): a line of 12 or more characters whose next record of its game came less than 1 s later was
skimmed, not read (12 characters in under a second is over 43,200 an hour). Three such lines in a row are a skip:
their characters, their time and their timed characters come out of the counters again, the day's too, and so does
every skimmed line after them, of any length, until a line is not skimmed (the one the skip landed on, which is
read) or the interval breaks (another game, a clock set back, a damaged record; in the overlay also local
midnight, which already breaks the day's interval, so that a skip's lines are all of one day).
Skimmed lines under 12 characters neither count towards the three nor end the run; one or two skimmed lines stay
counted. `history.csv` keeps every line, so line history, lookups and Anki are as before. The writer applies the rule
as it appends and a recount applies it to the file, with the same function: the run so far is kept with the total
(`VjoReadingTime.skim`, `skim_chars`), so a cache or a restart in the middle of a run continues it. Histories that
earlier skips inflated are corrected at the first count: the cache grew from 112 to 120 B, so every game's total is
counted once more from `history.csv`.

This is [GameSentenceMiner](https://github.com/bpwhelan/GameSentenceMiner)'s skip detection
(`docs/SKIP_DETECTION.md`, `text_pipeline/skip_detection.py`) cut down to what Rubi sees. GameSentenceMiner sees
every line, holds each one out of the stats for 1.5 s, and confirms a burst by its volume (300 new characters in
1.5 s, 180 of them outside the largest line), by rapid replacement (4 replacements of 12 or more characters within
750 ms, 100 in all) or by Ctrl held; then it leaves the pending lines out and records a reading break. Rubi records at
most one line per job, so it sees fewer lines and only their times: no volume window and no held button (many games
toggle skip mode, and the button varies). It counts each line as it is recorded, so the total moves at once, and takes
a skip's lines back when the third one shows it is a skip. The thresholds keep GameSentenceMiner's 12 characters and
take three lines in a row in place of its four replacements in 750 ms.

**Simulated** (`python3 tools/reading_time_sim.py --skip`, its `skip mode (trusted hook)`: a skip of 20-200 lines at
0.1 s each, once an hour, a line recorded at each job; four readers, 12 runs of 4 hours) [estimates: simulated]. The time between two
records in a skip is the job's: the host's hook latency bench reads a trusted line 78-86 ms after the game writes it
([native-hooks.md](native-hooks.md#how-fast-measured), measured on the host), and on a console it is unmeasured, so
the scenario takes 0.1-0.5 s and the table tries others. Error against the truth (estimated / true − 1, the
mean, and the worst reader-run), adaptive rule:

| a record every | characters before | after | characters/hour before | after |
|---|---:|---:|---:|---:|
| 0.1-0.3 s | +92.8 % (+160.9) | +0.2 % (+0.5) | +92.4 % (+160.2) | +2.0 % (+3.9) |
| 0.1-0.5 s (the scenario) | +59.3 % (+95.9) | +0.2 % (+0.6) | +59.0 % (+94.3) | +1.9 % (+4.1) |
| 0.3-1 s | +26.8 % (+48.8) | +0.3 % (+1.1) | +26.6 % (+48.1) | +1.9 % (+3.7) |
| 0.5-1.5 s | +17.1 % (+30.3) | +13.5 % (+21.7) | +17.2 % (+29.6) | +13.9 % (+21.2) |
| everyday, trusted hook (0.1-0.5 s) | +18.2 % (+40.3) | +0.5 % (+1.8) | +13.5 % (+32.4) | −3.0 % (−12.4) |

Reading time hardly moves (−1.7 % after, the clean scenario's own error): the skipped lines' credits are small, and
the line the skip landed on is still credited. Every other scenario, and every existing fixture, is unchanged: in
them no three lines of 12 or more characters each go in under a second. Readers faster than the simulation's assume
lose almost nothing either: of the characters of clean reading, auto mode and both everyday mixes, a reader at
30,000 an hour lost none and one at 40,000 an hour 0.04 % (with the spread of a line's time at 0.6) [simulated, the
same report]. With 1.5 s (`SKIM_GAP_US`), which would catch a skip whose jobs take up to 1.5 s, those were 0.1-0.3 %
and 2-3.5 %; the host bench's 80 ms jobs make 1 s enough.

**Cost** (both builds' packaged Shell, the gate's VitaSDK image, against main `4e91602d`): code +192 B, data/bss
+8 B (the total's two new fields), the same pages [measured: `tools/check_memory_ledger.py`'s module rows]; no block,
thread or allocation. The cache file is 8 B larger.

**Not done (the issue's second step).** In skip mode the worker still reads and the Shell still records every line it
can: the kernel could fall back to API 41's behaviour while trusted lines arrive faster than a skip's (no wakeup while
drawing; the settle read takes the line it lands on), as GameSentenceMiner pauses its overlay's OCR until the text
settles. That is a kernel change and waits for a console run of this one.

## Limits

- Simulated, not measured on a console: the error figures depend on the assumed readers and breaks
  (hence the sensitivity table). A console history with the owner's own reading would test them.
- An AFK in the game's auto mode looks like reading: the lines keep coming at a reading pace. No rule
  based on the records' times can tell.
- The pace starts at 1.2 s a character for a game with no history (3,000 an hour); a much faster
  reader's first few dozen lines get a looser cap while it learns.
- The pace stays between 2 ms and 4.19 s a character (`VJO_READING_PACE_MIN` to 16 bits of 64 µs). Below
  32 units its 1/32 steps are 0: before this floor, a game with many records of 489 or more characters, a
  second or more apart and read at under 2 ms a character (a hook returning a page at once), could take
  the pace there for good, and every later line of the game got the 2 s floor (audit of #303).
- Skip mode's rule is untested on a console: how often a trusted hook records a line in a skip there is
  unmeasured (a debug build's `hook line:` log would show it). Jobs a second or more apart would make every
  skimmed line look read. A skip slow enough for a reader (a game's "skip read text" that shows each line a
  second) is reading time by this rule. Four or more lines of 12 or more characters a hook gives together (a
  choice menu's options, recorded a job apart) are taken for a skip, but for the last. `tools/reading_stats.py`
  has no local midnight in its run, so a skip that crosses it can differ from the overlay's by its first lines.
