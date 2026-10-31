"""Event-timeline simulator for Rubi's reading statistics (docs/reading-time.md).

It builds reading sessions whose true reading time is known (a reader of a given speed, with word
lookups, AFK breaks, suspends, standby, menus, the backlog, skip and auto mode), models what Rubi's
history.csv records of them (Read text: Automatically: one record per settled screen whose text is
not the text of the record before it, timed at the read's start), and scores each reading-time
method against the truth:

    python3 tools/reading_time_sim.py                 # the table of errors (markdown)
    python3 tools/reading_time_sim.py --json out.json # and every number, per scenario and profile
    python3 tools/reading_time_sim.py --fixtures DIR  # event-timeline fixtures: history.csv + truth
    python3 tools/reading_time_sim.py --skip          # skip mode's filter (docs/reading-time.md#skip-mode)

Every parameter below is an assumption, not a measurement on a console (docs/reading-time.md says
which ones and why); `--sensitivity` reruns the comparison with each one changed. Nothing here reads a
real history: no history.csv from a console exists on the build machine.
"""
from __future__ import annotations

import argparse
import json
import math
import random
import statistics
import sys
from dataclasses import dataclass, field, replace
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from reading_stats import adaptive_credit, skipped_lines  # noqa: E402  (the overlay's rules, in its integers)

# ---- the readers ----


@dataclass(frozen=True)
class Profile:
    """A reader: true speed (characters an hour, word lookups included), how often they look up."""
    name: str
    cph: float
    lookups_per_char: float  # Poisson rate of word lookups (L + R and the dictionary)
    lookup_s: tuple = (4.0, 20.0)  # seconds one lookup takes (uniform)
    sigma: float = 0.4       # log-normal spread of a line's reading time around its expectation
    advance_s: float = 0.6   # pressing on to the next line


PROFILES = (
    Profile("beginner 3k", 3000, 0.06),
    Profile("learner 6k", 6000, 0.025),
    Profile("intermediate 10k", 10000, 0.008),
    Profile("fluent 18k", 18000, 0.002),
)

# The characters of a text box: the 50 real VOCR regions of host/fixtures/word_lookup (as the writer
# counts them, core/reading_chars.h), and a share of very short boxes (……, えっ) the sample lacks.
BOX_CHARS = (6, 7, 9, 10, 10, 12, 14, 14, 16, 16, 16, 19, 19, 22, 23, 24, 24, 24, 24, 24, 26, 26, 27, 27,
             27, 27, 28, 28, 28, 29, 29, 30, 30, 30, 31, 32, 33, 33, 35, 35, 36, 37, 37, 37, 42, 42, 43, 43,
             44, 44)
SHORT_SHARE = 0.06
STANDBY_AFTER = 300.0  # the console's own standby after this long without input [assumed]


def mean_box_chars() -> float:
    return (1 - SHORT_SHARE) * statistics.mean(BOX_CHARS) + SHORT_SHARE * 2.0


# ---- the sessions ----


# A trusted hook's line (API 49, hook_trust = on) wakes the Shell even while the line before it still draws, so
# in skip mode a line is recorded whenever no job is claimed: one line per job, the job's time apart. The host's
# hook latency bench reads a trusted line 78-86 ms after the game writes it (docs/native-hooks.md, measured on the
# host); on a console unmeasured, hence up to 0.5 s here [assumed, docs/reading-time.md#skip-mode].
SKIP_JOB = (0.1, 0.5)


@dataclass(frozen=True)
class Scenario:
    """What happens besides reading. Rates are per hour of true reading; durations in seconds."""
    name: str
    source: str = "ocr"                     # ocr or hook: how Rubi gets the line
    afk_short: tuple = (0.0, (20, 120))     # (per hour, (min, max) s): a message, a drink
    afk_long: tuple = (0.0, (120, 3600))    # log-uniform durations
    suspend: tuple = (0.0, (60, 900))       # PS button: LiveArea or the Rubi app beside the game
    sleep: tuple = (0.0, (300, 28800))      # standby (log-uniform): many end the session
    menu: tuple = (0.0, (5, 40))            # save, load, settings
    menu_recorded: float = 0.5              # a menu's text in the box is read and recorded
    backlog: tuple = (0.0, (1, 4))          # (per hour, lines re-shown)
    backlog_recorded: float = 0.6           # an old line re-shown in the box is recorded again
    skip: tuple = (0.0, (20, 200))          # (per hour, lines skipped at 0.1 s each)
    skip_job: tuple = ()                    # a trusted hook (API 49): a skipped line recorded every (min, max) s
    auto_share: float = 0.0                 # lines read in the game's auto mode
    partial: float = 0.0                    # a typewriter pause records a part of the line first
    flicker: float = 0.0                    # the same screen read again with a different reading
    hours: float = 4.0                      # true reading time per run


def everyday(source: str) -> Scenario:
    return Scenario("everyday", source, afk_short=(4.0, (20, 120)), afk_long=(0.5, (120, 3600)),
                    suspend=(0.5, (60, 900)), sleep=(0.25, (300, 28800)), menu=(3.0, (5, 40)),
                    menu_recorded=0.5 if source == "ocr" else 0.15, backlog=(1.0, (1, 4)),
                    backlog_recorded=0.6 if source == "ocr" else 0.1, skip=(0.3, (20, 200)),
                    auto_share=0.1, partial=0.005 if source == "ocr" else 0.0,
                    flicker=0.01 if source == "ocr" else 0.0)


SCENARIOS = (
    Scenario("clean"),
    Scenario("afk breaks", afk_short=(4.0, (20, 120)), afk_long=(0.5, (120, 3600))),
    Scenario("suspend + standby", suspend=(1.0, (60, 900)), sleep=(0.5, (300, 28800))),
    Scenario("menus + backlog", menu=(3.0, (5, 40)), backlog=(1.0, (1, 4))),
    Scenario("skip mode", skip=(1.0, (20, 200))),
    Scenario("auto mode", auto_share=0.5),
    everyday("ocr"),
    replace(everyday("hook"), name="everyday (hook)"),
)
# Skip mode's own scenario (--skip, docs/reading-time.md#skip-mode): out of the grid above, where it would measure the
# skip filter, which only Rubi's rule has, rather than the AFK rules the grid compares.
TRUSTED_SKIP = Scenario("skip mode (trusted hook)", "hook", skip=(1.0, (20, 200)), skip_job=SKIP_JOB)


@dataclass
class Rec:
    """One history.csv record, as the stats see it."""
    time: float      # RTC seconds (the record's `time`)
    mono: float      # SceShell's process time (`monotonic_us`): stands still in standby [assumed]
    game: str
    chars: int
    text: object     # what identifies the text (the writer compares hashes of it)
    source: str = "ocr"


@dataclass
class Truth:
    seconds: float = 0.0  # active reading: on a line, lookups included; no AFK, suspend, standby, menu
    chars: int = 0        # each dialogue line read once
    lines: int = 0
    pauses: list = field(default_factory=list)  # (start, end) RTC seconds the game was not in front
    blanks: list = field(default_factory=list)  # RTC seconds of reads that found the box blank (no record)


def log_uniform(rng: random.Random, lo: float, hi: float) -> float:
    return math.exp(rng.uniform(math.log(lo), math.log(hi)))


def box_chars(rng: random.Random) -> int:
    return rng.randint(0, 4) if rng.random() < SHORT_SHARE else rng.choice(BOX_CHARS)


def line_seconds(rng: random.Random, p: Profile, chars: int) -> float:
    """A line's true reading time: reading at the profile's pace, its lookups, the press on."""
    lookup_mean = sum(p.lookup_s) / 2
    base = 3600.0 / p.cph - p.lookups_per_char * lookup_mean - p.advance_s / mean_box_chars()
    per_char = base / math.exp(p.sigma ** 2 / 2)  # the log-normal's mean is exp(sigma^2 / 2)
    t = p.advance_s + chars * per_char * rng.lognormvariate(0.0, p.sigma)
    n = poisson(rng, chars * p.lookups_per_char)
    return t + sum(rng.uniform(*p.lookup_s) for _ in range(n))


def poisson(rng: random.Random, lam: float) -> int:
    n, l, prod = 0, math.exp(-lam), rng.random()
    while prod > l:
        n += 1
        prod *= rng.random()
    return n


def happens(rng: random.Random, per_hour: float, seconds: float) -> bool:
    return per_hour > 0 and rng.random() < per_hour * seconds / 3600.0


def simulate(p: Profile, s: Scenario, seed: int, game: str = "vita-PCSG00415"):
    """One run: the records Rubi writes and the truth. Clock in seconds from an arbitrary epoch."""
    rng = random.Random(f"{p.name}|{s.name}|{s.source}|{seed}")
    t0 = 1_790_000_000.0
    t = t0          # RTC now
    asleep = 0.0    # standby so far: the process time lags the RTC by it
    truth = Truth()
    raw = []        # (time, mono, chars, text): every read Rubi completes, before the repeat check
    line_id = 0
    history = []    # ids of earlier lines (the backlog)
    hook = s.source == "hook"
    base_per_char = 3600.0 / p.cph - p.lookups_per_char * sum(p.lookup_s) / 2

    def read(at: float, chars: int, text) -> None:
        raw.append((at, at - t0 - asleep, chars, text))

    def record_delay(chars: int) -> float:
        if hook:  # API 41: a hooked line starts the read at once
            return rng.uniform(0.05, 0.25)
        reveal = chars * 1.15 / 30.0  # typing out at 30 a second, punctuation included
        return reveal + 0.3 + rng.uniform(0.0, 0.15)  # the region settles 300 ms after (kernel/scene.c)

    def back_to_line(chars: int, text, delay: float, recorded: bool) -> None:
        """The line shown again (after a menu or the backlog) until the player presses on: a read of
        it is a new record when the record before it was another text."""
        nonlocal t
        r = rng.uniform(0.5, 2.0)
        truth.seconds += r
        if recorded and r > delay:
            read(t + delay, chars, text)
        t += r

    while truth.seconds < s.hours * 3600.0:
        if happens(rng, s.skip[0], 60.0):  # a skip: lines flash by, nothing settles, nothing is read
            n = rng.randint(*s.skip[1])
            if s.skip_job:  # a trusted hook: the line shown as each job starts is recorded
                skipped = [box_chars(rng) for _ in range(n)]
                at = t + rng.uniform(*s.skip_job)
                while at < t + n * 0.1:
                    k = int((at - t) / 0.1)
                    read(at, skipped[k], ("line", line_id + 1 + k))
                    at += rng.uniform(*s.skip_job)
            t += n * 0.1
            line_id += n
        chars = box_chars(rng)
        line_id += 1
        text = ("line", line_id)
        auto = rng.random() < s.auto_share
        # In the game's auto mode the line stays for a set time, which the reader set to about their
        # own pace (looking nothing up meanwhile)
        reading = (1.0 + chars * base_per_char * rng.uniform(0.8, 1.2) if auto
                   else line_seconds(rng, p, chars))
        delay = record_delay(chars)
        appear = t
        if reading > delay:  # else the player pressed on before the read: no record of this line
            read(appear + delay, chars, text)
            if s.partial and not hook and chars >= 6 and rng.random() < s.partial:
                u = rng.uniform(0.3, 0.7)  # a pause in the typing passed as settled: a part first
                read(appear + delay * u, int(chars * u), ("part", line_id))
            if s.flicker and reading > delay + 3.0 and rng.random() < s.flicker:
                at = appear + rng.uniform(delay + 1.0, reading - 2.0)  # a refresh read it differently
                read(at, max(0, chars + rng.choice((-1, 1))), ("misread", line_id))
                if rng.random() < 0.5:  # and the next read right again
                    read(at + 1.0, chars, text)
        truth.seconds += reading
        truth.chars += chars
        truth.lines += 1
        t += reading
        # What interrupts this line (after it is read: the gap to the next record is the same)
        if happens(rng, s.afk_short[0], reading):
            t += rng.uniform(*s.afk_short[1])
        if happens(rng, s.afk_long[0], reading):
            d = log_uniform(rng, *s.afk_long[1])
            t += d
            if d > STANDBY_AFTER:  # the Vita goes to standby by itself [assumed: 5 minutes]
                asleep += d - STANDBY_AFTER
        if happens(rng, s.suspend[0], reading):
            d = rng.uniform(*s.suspend[1])
            truth.pauses.append((t, t + d))
            t += d  # back in the game, the same text is read again: the repeat check drops it
        if happens(rng, s.sleep[0], reading):
            d = log_uniform(rng, *s.sleep[1])
            truth.pauses.append((t, t + d))
            t += d
            asleep += d
        if happens(rng, s.menu[0], reading):
            d = rng.uniform(*s.menu[1])
            shown = rng.random() < s.menu_recorded
            if shown:
                read(t + 0.5, rng.randint(3, 12), ("menu", rng.randint(0, 5)))
            elif not hook and rng.random() < 0.5:
                read(t + 0.5, 0, None)  # the box read blank: the repeat check starts over
                shown = True
            t += d
            back_to_line(chars, text, delay, shown)
        if happens(rng, s.backlog[0], reading) and history:
            k = min(len(history), rng.randint(*s.backlog[1]))
            again = False
            for old_text, old_chars in history[-k:][::-1]:
                d = rng.uniform(3.0, 10.0)  # re-reading an old line is reading, but no new characters
                truth.seconds += d
                if rng.random() < s.backlog_recorded:
                    read(t + 0.4, old_chars, old_text)
                    again = True
                t += d
            back_to_line(chars, text, delay, again)
        history.append((text, chars))
        del history[:-8]
    raw.sort(key=lambda r: r[0])
    recs, prev = [], None
    for at, mono, chars, text in raw:  # shell/history.c: the record before's text is not written again
        if text is None:
            prev = None  # a blank read: vjo_history_reset, so the text before may be written again
            truth.blanks.append(at)
            continue
        if text == prev:
            continue
        prev = text
        recs.append(Rec(at, mono, game, chars, text, s.source))
    return recs, truth


# ---- the methods: reading seconds, characters and timed characters from the records ----


@dataclass
class Result:
    seconds: float
    chars: int
    timed_chars: int

    def cph(self) -> float | None:
        return self.timed_chars * 3600.0 / self.seconds if self.seconds > 0 else None


def gaps(recs):
    """(gap to the next record of the same game, record) for each record; None for the last one."""
    for r, n in zip(recs, recs[1:] + [None]):
        yield (n.time - r.time if n is not None and n.game == r.game else None), r


def per_line(credit):
    """A method from credit(gap, record) -> seconds or None (no time: the line is untimed)."""
    def run(recs, truth=None):
        sec, timed = 0.0, 0
        for g, r in gaps(recs):
            if g is None:
                continue
            c = credit(g, r)
            if c is not None:
                sec += c
                timed += r.chars
        return Result(sec, sum(r.chars for r in recs), timed)
    return run


def method_drop(limit: float):
    """A gap of at most `limit` is the line's reading time; a longer one is a break and the line gets none:
    Rubi 0.9.7 (the overlay: 15 s, core/history_lines.c; tools/reading_stats.py: 5 minutes)."""
    return per_line(lambda g, r: g if 0 < g <= limit else None)


def method_cap(cap: float, session: float = math.inf):
    """A flat AFK cap: every line gets min(gap, cap) (a gap over `session` ends a session: none)."""
    return per_line(lambda g, r: min(g, cap) if 0 < g <= session else None)


def method_lencap(per_char: float, floor: float = 2.0, ceiling: float = math.inf, session: float = math.inf):
    """A cap from the line's length alone: min(gap, per_char a character, floor to ceiling)."""
    return per_line(lambda g, r: min(g, min(max(floor, per_char * r.chars), ceiling)) if 0 < g <= session else None)


def split(recs, session: float):
    """The records' (gap, chars) pairs in sessions: a gap over `session` ends one and is not credited."""
    out, cur = [], []
    for g, r in gaps(recs):
        if g is None:
            continue
        if g > session:
            out.append(cur)
            cur = []
            continue
        cur.append((max(0.0, g), r.chars))
    out.append(cur)
    return out


def timed_in(recs, session: float):
    return sum(r.chars for g, r in gaps(recs) if g is not None and g <= session)


def gsm_v1(recs, truth=None):
    """GameSentenceMiner 2026-03-13 (5c11e6b9, #394) to 2026-09-30: each line at most max(15 s, 3 s a
    character), 300 s ceiling (stage 1); then lines slower than Q1 - 1.5 IQR in characters a second are
    re-timed at the median speed (stage 2, with 10 or more lines of 5+ characters). web/stats.py
    _reading_time_legacy (f4c5a955); every gap of the set, no sessions."""
    gs = [[min(g, min(max(15.0, c * 3.0), 300.0)), c] for g, c in split(recs, math.inf)[0]]
    speeds = [(c / g, i) for i, (g, c) in enumerate(gs) if c >= 5 and g > 0]
    if len(speeds) >= 10:
        ss = sorted(v for v, _ in speeds)
        q1, q3, med = ss[len(ss) // 4], ss[3 * len(ss) // 4], ss[len(ss) // 2]
        lo = q1 - 1.5 * (q3 - q1)
        for v, i in speeds:
            if v < lo and med > 0:
                gs[i][0] = gs[i][1] / med
    return Result(sum(g for g, _ in gs), sum(r.chars for r in recs), timed_in(recs, math.inf))


def gsm_v2(scale: float = 1.0, filtered: bool = False, session: float = math.inf):
    """GameSentenceMiner's adaptive cap: each line at most 2.5 times its expected time at the median
    characters a second (times `scale`), 2 s floor, 300 s ceiling, 3 s a character without a pace.
      2026-06-19 f4c5a955 (opt-in), 2026-09-30 f13f205f (for all): scale 1, then 0.8 (e59edb51, 07-05) and
        0.5 (ad677c4b, 07-10); samples: 5+ characters, any gap; one median over the whole set.
      2026-10-01 9eba9390 (now): scale 1; samples 5+ characters in 1 s to 300 s (reading_speed_sample);
        a median per 30-minute session, whose longer gaps are not credited; an empty line gets nothing."""
    def run(recs, truth=None):
        sec = 0.0
        for sess in split(recs, session):
            if filtered:
                samples = [c / g for g, c in sess if c >= 5 and 1.0 <= g <= 300.0]
            else:
                samples = [c / g for g, c in sess if c >= 5 and g > 0]
            cps = statistics.median(samples) * scale if samples else 0.0
            for g, c in sess:
                if filtered and c <= 0:
                    continue
                cap = max(2.0, c / cps * 2.5) if cps > 0 else max(2.0, c * 3.0)
                sec += min(g, min(cap, 300.0))
        return Result(sec, sum(r.chars for r in recs), timed_in(recs, session))
    return run


def adaptive_lines(recs) -> list:
    """(credit_us, characters) of each record by the adaptive rule, in the overlay's integers."""
    pace, lines = 0, []
    for g, r in gaps(recs):
        credit = 0
        gap = int(round(g * 1e6)) if g is not None else 0
        if gap > 0:
            credit, pace = adaptive_credit(gap, r.chars, pace)
        lines.append((credit, r.chars))
    return lines


def rubi_adaptive(recs, truth=None):
    """Rubi's adaptive rule (reading_time = adaptive): tools/reading_stats.py adaptive_credit, the same
    integers as core/history_lines.c vjo_reading_time_add; per game, no sessions; skip mode's lines out."""
    return skip_filtered(adaptive_lines(recs))


def rubi_adaptive_unfiltered(recs, truth=None):
    """rubi_adaptive with every line counted, skip mode's too (Rubi before #602)."""
    lines = adaptive_lines(recs)
    return Result(sum(c for c, _ in lines) / 1e6, sum(n for _, n in lines), sum(n for c, n in lines if c))


def skip_filtered(lines) -> Result:
    """(credit_us, characters) of each record: the totals less skip mode's lines (tools/reading_stats.py)."""
    out = set(skipped_lines(lines))
    kept = [x for i, x in enumerate(lines) if i not in out]
    return Result(sum(c for c, _ in kept) / 1e6, sum(n for _, n in kept), sum(n for c, n in kept if c))


def method_online(q: float = 0.5, k: float = 2.5, step: float = 0.125, floor: float = 2.0,
                  ceiling: float = 300.0, session: float = math.inf, start: float = 1.2):
    """Exploration, in floats: a running q-quantile of the seconds a character takes (up by step * q above
    it, down by step * (1 - q) below; samples as GameSentenceMiner's: 5+ characters, 1 s to 300 s), each
    line at most k times its expected time at it, floor to ceiling."""
    def run(recs, truth=None):
        pace, sec, timed = start, 0.0, 0
        for g, r in gaps(recs):
            if g is None or g <= 0 or g > session:
                continue
            sec += min(g, min(max(floor, k * pace * r.chars), ceiling))
            timed += r.chars
            if r.chars >= 5 and 1.0 <= g <= 300.0:
                pace *= (1 + step * q) if g > pace * r.chars else (1 - step * (1 - q))
        return Result(sec, sum(r.chars for r in recs), timed)
    return run


def with_pauses(base):
    """`base`, with a gap that spans a pause (the game not in front: a suspend, standby) left out, as a
    break: what the Shell could do, as it sees the game leave the front (history.csv does not say)."""
    def run(recs, truth):
        parts, kept = [], []
        for r, n in zip(recs, recs[1:] + [None]):
            kept.append(r)
            if n is not None and any(r.time < a and b <= n.time for a, b in truth.pauses):
                parts.append(kept)
                kept = []
        parts.append(kept)
        out = Result(0.0, 0, 0)
        for part in parts:
            x = base(part, truth)
            out = Result(out.seconds + x.seconds, out.chars + x.chars, out.timed_chars + x.timed_chars)
        return out
    return run


def with_process_time(base):
    """`base` on SceShell's process time (monotonic_us) instead of the RTC: it leaves out standby, if the
    process clock stands still in it (unverified on a console)."""
    def run(recs, truth):
        lag0 = recs[0].time - recs[0].mono if recs else 0.0
        return base([replace(r, time=r.time - (r.time - r.mono - lag0)) for r in recs], truth)
    return run


def distinct_lines(base, n: int = 8):
    """`base` on the distinct new lines only: a record whose text is one of the `n` records before it (the
    backlog, a line again after a menu) is left out, so its time goes to the line before it and its
    characters count nothing."""
    def run(recs, truth):
        seen, kept = [], []
        for r in recs:
            if r.text not in seen:
                kept.append(r)
            seen.append(r.text)
            del seen[:-n]
        return base(kept, truth)
    return run


METHODS = {
    # Rubi today
    "Rubi 0.9.7 overlay: drop > 15 s": method_drop(15.0),
    "Rubi 0.9.7 tool: drop > 5 min": method_drop(300.0),
    # GameSentenceMiner's rules, in order (docs/reading-time.md, the report's history)
    "GSM 2025-09: cap 120 s": method_cap(120.0),
    "GSM 2025-12: cap 60 s": method_cap(60.0),
    "GSM 2026-03: 3 s/char + IQR": gsm_v1,
    "GSM 2026-06: 2.5x median": gsm_v2(1.0),
    "GSM 2026-07a: 2.5x 0.8-median": gsm_v2(0.8),
    "GSM 2026-07b: 2.5x 0.5-median": gsm_v2(0.5),
    "GSM 2026-10: 2.5x median/session": gsm_v2(1.0, filtered=True, session=1800.0),
    # other trackers
    "JL: chars / 10 per minute": method_lencap(6.0, floor=1.5),
    "exSTATic: cap 60 s": method_cap(60.0),
    # fixed alternatives for Rubi
    "drop > 60 s": method_drop(60.0),
    "drop > 120 s": method_drop(120.0),
    "lencap 2 s/char": method_lencap(2.0, ceiling=300.0),
    # Rubi's adaptive rule and its neighbours
    "Rubi adaptive (q75, 2.5x)": rubi_adaptive,
    "online median 2.5x": method_online(0.5, 2.5),
    "online median 4x": method_online(0.5, 4.0),
    "online q75 3x": method_online(0.75, 3.0),
    "online q75 2.5x, 30-min break": method_online(0.75, 2.5, session=1800.0),
    "Rubi adaptive + pauses": with_pauses(rubi_adaptive),
    "Rubi adaptive + process time": with_process_time(rubi_adaptive),
    "Rubi adaptive + distinct lines": distinct_lines(rubi_adaptive),
}
HEADLINE = ("Rubi 0.9.7 overlay: drop > 15 s", "Rubi 0.9.7 tool: drop > 5 min", "GSM 2025-09: cap 120 s",
            "GSM 2025-12: cap 60 s", "GSM 2026-03: 3 s/char + IQR", "GSM 2026-06: 2.5x median",
            "GSM 2026-07b: 2.5x 0.5-median", "GSM 2026-10: 2.5x median/session", "JL: chars / 10 per minute",
            "drop > 120 s", "Rubi adaptive (q75, 2.5x)")
CANDIDATES = HEADLINE + ("online median 4x", "online q75 2.5x, 30-min break")


def evaluate(methods=None, seeds: int = 12, profiles=PROFILES, scenarios=SCENARIOS):
    """{scenario: {profile: {method: {time, cph, chars}}}}: mean relative errors (est / true - 1) over
    the seeds, in percent."""
    methods = methods or METHODS
    out = {}
    for s in scenarios:
        out[s.name] = {}
        for p in profiles:
            acc = {m: {"time": [], "cph": [], "chars": []} for m in methods}
            for seed in range(seeds):
                recs, truth = simulate(p, s, seed)
                true_cph = truth.chars * 3600.0 / truth.seconds
                for m, f in methods.items():
                    res = f(recs, truth)
                    acc[m]["time"].append(100.0 * (res.seconds / truth.seconds - 1))
                    acc[m]["cph"].append(100.0 * ((res.cph() or 0.0) / true_cph - 1))
                    acc[m]["chars"].append(100.0 * (res.chars / truth.chars - 1))
            out[s.name][p.name] = {m: {k: statistics.mean(v) for k, v in d.items()} for m, d in acc.items()}
    return out


def summary(results, key: str = "time"):
    """{method: (mean |error|, worst |error|)} over every scenario and profile."""
    errs = {}
    for per_profile in results.values():
        for per_method in per_profile.values():
            for m, e in per_method.items():
                errs.setdefault(m, []).append(abs(e[key]))
    return {m: (statistics.mean(v), max(v)) for m, v in errs.items()}


def markdown(results, methods, key: str) -> str:
    cols = [(s, p) for s in results for p in results[s]]
    lines = ["| method | " + " | ".join(f"{s}, {p}" for s, p in cols) + " | mean abs | worst |",
             "|---|" + "---:|" * (len(cols) + 2)]
    summ = summary(results, key)
    for m in methods:
        cells = [f"{results[s][p][m][key]:+.0f}" for s, p in cols]
        lines.append(f"| {m} | " + " | ".join(cells) + f" | {summ[m][0]:.1f} | {summ[m][1]:.0f} |")
    return "\n".join(lines)


def ranking(results, methods) -> str:
    """Every method's mean and worst |error| over the whole grid: reading time, characters/hour,
    characters; best reading time first."""
    t, c, ch = summary(results, "time"), summary(results, "cph"), summary(results, "chars")
    lines = ["| method | time: mean abs % | time: worst % | chars/hour: mean abs % | chars/hour: worst % |"
             " characters: mean abs % |", "|---|---:|---:|---:|---:|---:|"]
    for m in sorted(methods, key=lambda m: t[m][0]):
        lines.append(f"| {m} | {t[m][0]:.1f} | {t[m][1]:.0f} | {c[m][0]:.1f} | {c[m][1]:.0f} | {ch[m][0]:.1f} |")
    return "\n".join(lines)


def compact(results, methods, key: str) -> str:
    """One row per method, one column per scenario (the profiles' mean |error|), and the overall."""
    scen = list(results)
    lines = ["| method | " + " | ".join(scen) + " | all: mean abs | all: worst |",
             "|---|" + "---:|" * (len(scen) + 2)]
    summ = summary(results, key)
    for m in methods:
        cells = []
        for s in scen:
            v = [results[s][p][m][key] for p in results[s]]
            cells.append(f"{statistics.mean(v):+.0f} ({min(v):+.0f}..{max(v):+.0f})")
        lines.append(f"| {m} | " + " | ".join(cells) + f" | {summ[m][0]:.1f} | {summ[m][1]:.0f} |")
    return "\n".join(lines)


# ---- fixtures for the host tests ----
# host/fixtures/reading_time/*.tsv: one simulated timeline each, as history.csv records it (one game), with
# its truth and what each rule credits, in the integers the overlay counts. host/tests/test_shell_history.c
# appends the records through the real writer and checks the counters; tools/tests/test_reading_stats.py
# checks this tool and the error against the truth.
FIXTURES = (  # (scenario, profile): every scenario, a learner; the everyday one at the two ends too
    *((s.name, "learner 6k") for s in SCENARIOS + (TRUSTED_SKIP,)),
    ("everyday", "beginner 3k"), ("everyday", "fluent 18k"))
FIXTURE_HOURS = 1.0


def text_id(text) -> str:
    """A record's text as the fixture names it: L<n> a dialogue line, P<n> a part of one (typing out),
    X<n> a misreading of one, M<n> a menu's."""
    kind, n = text
    return {"line": "L", "part": "P", "misread": "X", "menu": "M"}[kind] + str(n)


def overlay_totals(lines) -> tuple:
    """(credit_us, characters) of each record, in the overlay's integers: (active_us, timed_chars, characters)
    less skip mode's lines."""
    out = set(skipped_lines(lines))
    kept = [x for i, x in enumerate(lines) if i not in out]
    return sum(c for c, _ in kept), sum(n for c, n in kept if c), sum(n for _, n in kept)


def rule_15s(recs) -> tuple:
    """Rubi 0.9.7's overlay rule in its integers: (active_us, timed_chars, characters)."""
    lines = []
    for r, n in zip(recs, recs[1:] + [None]):
        gap = int(round(n.time * 1e6)) - int(round(r.time * 1e6)) if n else 0
        lines.append((gap if 0 < gap <= 15_000_000 else 0, r.chars))
    return overlay_totals(lines)


def rule_adaptive(recs) -> tuple:
    """The adaptive rule in the overlay's integers (tools/reading_stats.py adaptive_credit), one game."""
    pace, lines = 0, []
    for r, n in zip(recs, recs[1:] + [None]):
        gap = int(round(n.time * 1e6)) - int(round(r.time * 1e6)) if n else 0
        credit = 0
        if gap > 0:
            credit, pace = adaptive_credit(gap, r.chars, pace)
        lines.append((credit, r.chars))
    return (*overlay_totals(lines), pace)


def fixture_name(scenario: str, profile: str) -> str:
    return (f"{scenario}-{profile.split()[0]}".replace(" + ", "-").replace(" (", "-").replace(")", "")
            .replace(" ", "-"))


def write_fixtures(directory: Path) -> list:
    """FIXTURES as directory/<name>.tsv; returns the names."""
    directory.mkdir(parents=True, exist_ok=True)
    scen, prof = {s.name: s for s in SCENARIOS + (TRUSTED_SKIP,)}, {p.name: p for p in PROFILES}
    made = []
    for sname, pname in FIXTURES:
        s, p = replace(scen[sname], hours=FIXTURE_HOURS), prof[pname]
        recs, truth = simulate(p, s, 0)
        a, f = rule_adaptive(recs), rule_15s(recs)
        name = fixture_name(sname, pname)
        lines = [f"# tools/reading_time_sim.py --fixtures: {sname}, {pname} ({s.source}), seed 0, "
                 f"{FIXTURE_HOURS:g} h of reading",
                 f"# truth: seconds={truth.seconds:.3f} characters={truth.chars} lines={truth.lines}",
                 f"# adaptive: active_us={a[0]} timed_chars={a[1]} characters={a[2]} pace={a[3]}",
                 f"# 15s: active_us={f[0]} timed_chars={f[1]} characters={f[2]}",
                 "# time_us\tcharacters\ttext (-: a read that found the box blank, no record)"]
        rows = [(int(round(r.time * 1e6)), f"{r.chars}\t{text_id(r.text)}") for r in recs]
        rows += [(int(round(t * 1e6)), "0\t-") for t in truth.blanks]  # a blank read: no record
        lines += [f"{us}\t{rest}" for us, rest in sorted(rows)]
        (directory / f"{name}.tsv").write_text("\n".join(lines) + "\n", encoding="utf-8")
        made.append(name)
    return made


def read_fixture(path: Path):
    """(header values, records [(time_us, characters, text id)]) of a fixture written by write_fixtures."""
    head, recs = {}, []
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("# ") and ": " in line and "=" in line:
            key, _, vals = line[2:].partition(": ")
            head[key] = {k: float(v) if "." in v else int(v) for k, v in (kv.split("=") for kv in vals.split())}
        elif line and not line.startswith("#"):
            us, chars, text = line.split("\t")
            if text != "-":
                recs.append((int(us), int(chars), text))
    return head, recs


def skip_report(seeds: int = 12) -> str:
    """Skip mode (docs/reading-time.md#skip-mode): the characters' and characters/hour's error (estimated / true - 1,
    %: the mean and the worst run of every reader) with every line counted and with skip mode's lines out, for a
    trusted hook's skip at several job times; then what the filter takes from fast readers' ordinary reading."""
    def error(scenario):
        rows = {"before": [], "after": []}
        for p in PROFILES:
            for seed in range(seeds):
                recs, truth = simulate(p, scenario, seed)
                true_cph = truth.chars * 3600.0 / truth.seconds
                for key, method in (("before", rubi_adaptive_unfiltered), ("after", rubi_adaptive)):
                    res = method(recs)
                    rows[key].append((100.0 * (res.chars / truth.chars - 1), 100.0 * ((res.cph() or 0) / true_cph - 1)))
        cells = []
        for col in (0, 1):
            for key in ("before", "after"):
                v = [r[col] for r in rows[key]]
                cells.append(f"{statistics.mean(v):+.1f} % ({max(v, key=abs):+.1f})")
        return cells[0], cells[1], cells[2], cells[3]
    lines = ["| a record every | characters before | after | characters/hour before | after |", "|---|---:|---:|---:|---:|"]
    for job in ((0.1, 0.3), SKIP_JOB, (0.3, 1.0), (0.5, 1.5)):
        name = f"{job[0]:g}-{job[1]:g} s" + (" (the scenario)" if job == SKIP_JOB else "")
        lines.append(f"| {name} | " + " | ".join(error(replace(TRUSTED_SKIP, skip_job=job))) + " |")
    lines.append(f"| everyday, trusted hook ({SKIP_JOB[0]:g}-{SKIP_JOB[1]:g} s) | "
                 + " | ".join(error(replace(everyday("hook"), skip_job=SKIP_JOB))) + " |")
    lines += ["", "| reader | spread | characters of ordinary reading taken as skipped |", "|---|---:|---:|"]
    ordinary = [s for s in SCENARIOS if s.name in ("clean", "auto mode", "everyday", "everyday (hook)")]
    for cph in (18000, 30000, 40000):
        for sigma in (0.4, 0.6):
            p = Profile(f"{cph}", cph, 0.002, sigma=sigma)
            lost = total = 0
            for s in ordinary:
                for seed in range(seeds // 3 or 1):
                    recs, _ = simulate(p, s, seed)
                    lines_ = adaptive_lines(recs)
                    lost += sum(lines_[i][1] for i in skipped_lines(lines_))
                    total += sum(n for _, n in lines_)
            lines.append(f"| {cph:,} an hour | {sigma} | {100.0 * lost / total:.2f} % |")
    return "\n".join(lines)


def sensitivity(seeds: int = 6, methods=CANDIDATES):
    """The methods' mean |time error| over the grid with one assumption changed at a time."""
    base_profiles = PROFILES

    def more(s: Scenario, f: float) -> Scenario:
        return replace(s, afk_short=(f * s.afk_short[0], s.afk_short[1]), afk_long=(f * s.afk_long[0], s.afk_long[1]),
                       suspend=(f * s.suspend[0], s.suspend[1]), menu=(f * s.menu[0], s.menu[1]))
    variants = {
        "as assumed": (base_profiles, SCENARIOS),
        "spread 0.25": (tuple(replace(p, sigma=0.25) for p in base_profiles), SCENARIOS),
        "spread 0.6": (tuple(replace(p, sigma=0.6) for p in base_profiles), SCENARIOS),
        "no lookups": (tuple(replace(p, lookups_per_char=0.0) for p in base_profiles), SCENARIOS),
        "lookups x1.5": (tuple(replace(p, lookups_per_char=1.5 * p.lookups_per_char) for p in base_profiles),
                         SCENARIOS),
        "interruptions x3": (base_profiles, tuple(more(s, 3.0) for s in SCENARIOS)),
        "interruptions x3, no lookups": (tuple(replace(p, lookups_per_char=0.0) for p in base_profiles),
                                         tuple(more(s, 3.0) for s in SCENARIOS)),
        "no interruptions": (base_profiles, (SCENARIOS[0],)),
    }
    out = {}
    for name, (profiles, scenarios) in variants.items():
        res = evaluate({m: METHODS[m] for m in methods}, seeds, profiles, scenarios)
        out[name] = {m: (round(v[0], 1), round(v[1], 0)) for m, v in summary(res).items()}
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seeds", type=int, default=12)
    ap.add_argument("--json", type=Path)
    ap.add_argument("--fixtures", type=Path)
    ap.add_argument("--sensitivity", action="store_true")
    ap.add_argument("--skip", action="store_true", help="skip mode's report only (docs/reading-time.md#skip-mode)")
    ap.add_argument("--all", action="store_true", help="every method, not just the headline ones")
    args = ap.parse_args(argv)
    if args.skip:
        print(skip_report(args.seeds))
        return 0
    if args.fixtures:
        for stem in write_fixtures(args.fixtures):
            print(stem)
        return 0
    res = evaluate(seeds=args.seeds)
    methods = list(METHODS) if args.all else list(HEADLINE)
    print(f"\n### All methods, over every scenario and profile ({args.seeds} runs each)\n")
    print(ranking(res, list(METHODS)))
    for key, title in (("time", "Reading time"), ("cph", "Characters/hour"), ("chars", "Characters")):
        print(f"\n### {title}: error in % (mean over {args.seeds} runs; scenario mean (profile range))\n")
        print(compact(res, methods if key != "chars" else list(METHODS), key))
    if args.sensitivity:
        print("\n### Sensitivity: |reading-time error| %, mean (worst) over the grid, one assumption changed\n")
        sens = sensitivity()
        cols = list(sens)
        print("| method | " + " | ".join(cols) + " |\n|---|" + "---:|" * len(cols))
        for m in CANDIDATES:
            print(f"| {m} | " + " | ".join(f"{sens[c][m][0]:.1f} ({sens[c][m][1]:.0f})" for c in cols) + " |")
    if args.json:
        args.json.write_text(json.dumps({"results": res, "summary": {k: summary(res, k) for k in ("time", "cph", "chars")},
                                         **({"sensitivity": sensitivity()} if args.sensitivity else {})}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
