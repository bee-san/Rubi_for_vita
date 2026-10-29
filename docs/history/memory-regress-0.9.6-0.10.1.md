# What grew between Rubi 0.9.6 and 0.10.1 (#528)

A dated record (2026-10-29) of the bisect that answered the owner's "it didnt used to be like this. but after hooking
and vocr6" in [#528](https://github.com/bee-san/Rubi_for_vita/issues/528). It builds the five releases v0.9.6,
v0.9.7, v0.9.8, v0.10.0 and v0.10.1 (both builds, each in the fast gate's images), reads every first-parent commit's
memory ledger between them, and compares them with the owner's console logs of the same releases.

Where a number comes from:

- **[measured: SELF]**: the packaged module's loaded segments. Every normal build made here has exactly the segment
  sizes of the release zip's SELF (read with `tools/vita_self.py`), so the toolchain is the releases'.
- **[ledger]**: the tag's own `include/vjo_ledger_budget.h`, read by its own `tools/check_memory_ledger.py`.
- **[measured: console log]**: the owner's #528 collection (gist `5e67276`), every reconstructed log, a boot seen in
  several snapshots counted once. The normal build logs a game's memory only when a check is refused.
- **[host]**: a host build of the tag's own game worker and kernel (its `host/tests/test_alloc_sim.c` with blocks and
  threads counted), with jobs asked for as that tag's Shell asks for them from its default `config.ini`, and the tag's
  pinned release models. These are x86-64 sizes and not console evidence (AGENTS.md rule 5): the spotter's block is
  57,344 B on the Vita and 61,440 B on the host.
- **[estimate]**: anything else.

## The answer

- **SceShell did not grow.** Every #528 failure was SceShell's PAF heap, and Rubi's Shell takes no more of it in
  0.10.1 than in 0.9.6. Its PAF allocation sites (four) and their budgets are the same in all five releases, and its
  module lost two pages in 0.10.1 (#434). In the console logs, the PAF heap was 3,726,280-3,727,040 B free when the
  Shell started (the median of each release from 0.8.0 to 0.10.1). Rubi held 8,192 B at every game start and exit
  from 0.9.8 on (a few exits of older releases, with the overlay or a result arena still open, held 28,672 or
  73,728 B), with a peak of at most 225,280 B in each of 0.9.6-0.10.1 [measured: console log]. The low game starts
  (131-380 KB free) are at API 25, 31, 50 and 57, so they predate hooks and VOCR6. Each followed a drop of about
  2.1 MB that Rubi's own ledger does not account for (#528).
- **The game process grew a little.** The OCR worker gained 20,480 B of pages in the normal build. The text spotter
  (#396, the default finder since 0.10.0) keeps one more block resident and widens a Small or Normal read's
  workspace. A read takes the same blocks and threads in every release.
- **The largest change in a game is how much Rubi may take when the game is short of memory**: #280's fixed 1 MiB
  reserve, in place of 4, 8 or 16 MiB. That was the owner's tested decision (2026-10-23) and stays.
- **OCR is not slower.** Recognition per CTC frame in CLANNAD was 6,067 µs with VOCR5-L and 5,903 µs with VOCR6.2-L
  [measured: console history.csv].
- **One regression was in the Shell's lookups**: 0.10.1's alternate readings (#520) read the dictionary 2.8 times as
  often as 0.10.0. #544 removes part of it (below).

## Module pages [measured: SELF]

Text + data/bss, each rounded up to 4 KiB pages. The Shell's pages are SceShell's, the kernel's are the 36 MiB KD
pool's, and the worker's are each enabled game's.

| release (API) | Shell normal | Shell debug | kernel normal | kernel debug | worker normal | worker debug |
|---|---:|---:|---:|---:|---:|---:|
| v0.9.6 (41) | 163,840 | 176,128 | 118,784 | 122,880 | 172,032 | 180,224 |
| v0.9.7 (41) | 163,840 | 176,128 | 118,784 | 122,880 | 172,032 | 180,224 |
| v0.9.8 (50) | 163,840 | 176,128 | 118,784 | 122,880 | 192,512 | 200,704 |
| v0.10.0 (54) | 163,840 | 176,128 | 118,784 | 122,880 | 208,896 | 217,088 |
| v0.10.1 (57) | 155,648 | 167,936 | 114,688 | 118,784 | 192,512 | 204,800 |

## The ledger, change by change [ledger]

All 200 first-parent commits from v0.9.6 to v0.10.1. Both builds moved together unless noted. Nothing changed from
v0.9.6 to v0.9.7.

| PR | change | effect |
|---|---|---|
| #307, head gate and two-level head (API 42) | worker +8,192; weights and cascade +4,096 each; workspace +8,192 | game worst case +16,384 |
| #306, VOCR6's LMs | LM budget 3,784,704 → 3,604,480 | game worst case −180,224 |
| #335, vertical text (API 47) | worker +4,096 | |
| #330, line finder fix (API 50) | worker +8,192 | |
| #368, SC3 charset (three MAGES games) | a 16,384 B item | game worst case +16,384 |
| #396, the spotter by default (API 54) | worker +16,384; spotter item 516,096 (57,344 B for the pinned blob); workspace +77,824 | game worst case +593,920 |
| #355, the pause for the frame (off by default) | worker +4,096; frame item 524,288 (in place of the tall line's) | game worst case +8,192 |
| #433, kernel rows in place | kernel −4,096 | |
| #455, one allocator and loader | worker −4,096 (normal only) | |
| #471, the gate's weights a block | worker −4,096; gate item 4,096 | game worst case +4,096 |
| #434, the strip's text without its arena | Shell −8,192 | SceShell's permanent pages −8,192 |
| #517, the current game's hook profile from disk | worker −12,288 | |

The pools of the normal build, permanent / worst case:

| release | shell pages | shell PAF | kernel | game | game limit |
|---|---:|---:|---:|---:|---:|
| v0.9.6, v0.9.7 | 204,800 / 225,280 | 8,192 / 241,664 | 151,552 / 241,664 | 208,896 / 10,567,680 | 10,579,968 |
| v0.9.8 | 204,800 / 225,280 | 8,192 / 241,664 | 151,552 / 241,664 | 229,376 / 10,403,840 | 10,604,544 |
| v0.10.0 | 204,800 / 225,280 | 8,192 / 241,664 | 151,552 / 241,664 | 245,760 / 11,014,144 | 11,214,848 |
| v0.10.1 | 196,608 / 217,088 | 8,192 / 241,664 | 147,456 / 237,568 | 229,376 / 11,026,432 | 11,214,848 |

## Static buffers [measured: ELF sections, normal build]

`.data` + `.bss`. The Shell's 0.10.0 growth is a table moved into the data segment (`deinflect_rules`, #386), with
the same pages.

| release | Shell | worker | kernel |
|---|---:|---:|---:|
| v0.9.6 | 73,242 | 32,976 | 76,068 |
| v0.9.7 | 73,174 | 34,872 | 76,068 |
| v0.9.8 | 73,230 | 35,320 | 76,076 |
| v0.10.0 | 77,666 | 36,832 | 76,092 |
| v0.10.1 | 69,106 (#434: the strip's 8,480 B arena) | 28,820 (#471, #517) | 70,712 (#433) |

## SceShell's PAF heap per release [measured: console log]

| API | release | boots | free at the Shell's start (median) | at a game start (median / lowest) | Rubi at starts and exits | Rubi's peak | refusals |
|---|---|---:|---:|---:|---:|---:|---:|
| 25 | v0.8.0 | 27 | 3,726,840 | 2,457,224 / 2,097,896 | 8,192 (one exit 28,672) | 225,280 | 3 |
| 31 | test build (hooks) | 2 | 3,734,928 | 2,511,648 / 274,936 | 8,192 | 94,208 | 33 |
| 33, 34, 36 | v0.9.0-v0.9.5 | 19 | 3,726,584-3,727,040 | 2,415,408-2,468,072 / 2,051,528 | 8,192 (three exits 28,672) | 241,664 | 0 |
| 41 | v0.9.6, v0.9.7 | 10 | 3,726,280 | 2,464,248 / 2,446,472 | 8,192 (exits 28,672 and 73,728 once each) | 225,280 | 0 |
| 50 | v0.9.8 | 9 | 3,726,456 | 2,362,768 / 379,512 | 8,192 | 225,280 | 0 |
| 54 | v0.10.0 | 12 | 3,726,504 | 2,474,936 / 2,384,952 | 8,192 | 225,280 | 0 |
| 57 | v0.10.1 | 10 | 3,726,472 | 2,377,992 / 131,432 | 8,192 | 225,280 | 52 |

What Rubi asks of the heap per scenario is the same code in all five releases. An OCR read takes at most one
64 KiB result arena (after OCR; given back at the next job's start). A lookup takes nothing (it runs in that arena).
An overlay open takes its 20 KiB text block (since #534, once at the Shell's start), a line-history visit 16 KiB,
and an Anki save 128 KiB. CLANNAD's reads per active minute went from 3.7-3.9 to 4.4-4.7 at 0.9.8
[measured: console history.csv], so the arena is taken and given back about 20% more often.

## The game process per read [host]

At CLANNAD's free USER (91,226,112 B), 917 x 125 text box, each release's default `config.ini`:

| release | size | first read's peak | resident between reads | blocks + threads: first read / new text / same text |
|---|---|---:|---:|---|
| v0.9.6-v0.9.8 | Small | 3,952,640 | 2,998,272-3,002,368 | 5+2 / 2+2 / 2+0 |
| v0.10.0, v0.10.1 | Small | 4,128,768 | 3,059,712 | 6+2 / 2+2 / 2+0 |
| v0.9.6-v0.9.8 | Normal | 5,857,280-5,861,376 | 4,653,056 | 5+2 / 2+2 / 2+0 |
| v0.10.0, v0.10.1 | Normal | 5,980,160 | 4,714,496 | 6+2 / 2+2 / 2+0 |
| v0.9.6-v0.9.8 | Large | 4,587,520 | 3,461,120-3,465,216 | 4+2 / 2+2 / 2+0 |
| v0.10.0, v0.10.1 | Large | 4,648,960 | 3,522,560 | 5+2 / 2+2 / 2+0 |

All of the change is #396 (its parent has 0.9.8's figures). A read's workspace grew from 892,928 to 1,007,616 B
(Small) and from 1,146,880 to 1,204,224 B (Normal). Large's, 1,064,960 B, did not change, and the console agrees:
1,052,016 → 1,056,816 → 1,056,672 B in CLANNAD [measured: console history.csv].

With less free USER, Large selected (text box, first read):

| free USER | v0.9.6-v0.9.8 (and #280's parent) | v0.10.0 and later (#280) |
|---:|---|---|
| 12 MiB | Normal and its LM, no cascade: peak 3,063,808-3,182,592 | Large and its LM: peak 4,648,960, 3,522,560 resident |
| 8 MiB | Small and its LM: peak 2,519,040-2,695,168 | Large and its LM: peak 4,648,960 |
| 6 MiB | Small and its LM: peak 2,060,288-2,068,480 | Large and its LM: peak 4,648,960, 1,642,496 B left to the game |
| 4 MiB | refused (`VJO_E_OOM`) | Large without its LM: peak 2,871,296, 1,323,008 B left to the game |

No game-process refusal or crash in the collection is attributed to this, and the normal build does not log the
free USER of the native ports the owner played.

## OCR speed in CLANNAD [measured: console history.csv]

OCR reads only (medians), and the share of all reads a hook answered without recognition (from 0.9.8, trusted hooks,
without a capture either). Releases are split at the install records, so the OCR samples after 0.9.8 are small.

| release | reads | answered by a hook | CTC frames | find ms | recognize ms | µs per frame | read ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| before 0.9.6 (VOCR5-L) | 1,726 | 53% | 45 | 145 | 267 | 6,070 | 699 |
| 0.9.6, 0.9.7 (VOCR5-L) | 270 | 74% | 40 | 126 | 218 | 6,067 | 1,042 |
| 0.9.8 (VOCR5-L, two-level head) | 375 | 97% | 36 | 105 | 215 | 5,970 | 499 |
| 0.10.0 (VOCR6.2-L) | 353 | 96% | 68 | 172 | 306 | 4,956 | 707 |
| 0.10.1 (VOCR6.2-L) | 110 | 94% | 45 | 88 | 266 | 5,903 | 591 |

## Lookups [host]

Each release's own lookup code and shipped `plaintext.vjdict`, the same 56 VOCR lines
(`host/fixtures/word_lookup/vocr_lines.tsv`), and the dictionary session kept as the Shell keeps it:

| | v0.9.6-v0.9.8 | v0.10.0 | v0.10.1 |
|---|---:|---:|---:|
| reads per first-word lookup | 6.12 | 5.12 (#370) | 12.52 |
| reads per lookup, every letter | 4.56 | 4.25 | 11.81 |
| result arena peak, every letter | 32,880 | 32,880 | 39,848 |

The 64 KiB arena itself did not change. 0.10.1's alternates pass (#520, `vjo_local_alternates`) read the head of
every entry under the shown word's key, homophones included. It also searched the index again with a cold page
cache and read each alternate's payload. #544 passes over the homophones unread, as their posting hints allow,
with identical results at every tap. Reads per cursor lookup (`vjo-dict-stage-bench`, 1,156 taps) went from 10.41
to 8.74 with the bundled dictionary, and from 12.01 to 10.34 with the names dictionary too. That is about 5 ms a
lookup on the console [estimate: 1.67 reads at the test.4 fit's 2.98 ms a read,
[docs/lookup-performance.md](../lookup-performance.md); 6.4 ms at docs/performance.md's older 3.82 ms]. The rest of #520's cost
(the second index search, about 2.2 reads; the alternates' own payloads, which the popup shows) is left.

## Handed on

- The spotter's scratch widens Small's and Normal's workspace (#396) and only grows the scratch
  (`core/vocr_ocr.c` `layout_spot`): the game worker's area.
- #280's reserve stays the owner's decision. The game worker's whole-MiB model of the console's free-memory reports
  shows its effect on games that are short of memory.
- OCR speed: no regression to act on.

The scripts, the per-release logs and the tables' raw data are with the swarm's reports, not in this repository.
