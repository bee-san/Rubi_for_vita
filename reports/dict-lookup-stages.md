# A lookup's stages on real taps: the stage benchmark

`vjo-dict-stage-bench` (`host/dict_stage_bench.c`) runs the Shell's cursor lookup
(`vjo_overlay_lookup_at`, set up as `shell/worker.c` sets it up: one 64 KiB block with the
dictionary session at its start, the line's text generation, the configured Dictionaries shown) at
every tap of a corpus in `tools/yomitan_check/build_corpus.py`'s layout (`ID\tTEXT\tTAPS`), with
every file read attributed and the host time of each stage clocked (`core/local_dict.c` under
`VJO_LOCAL_STATS`: deinflection, search, ranking, the shown entry, the stacked entries; the Vita
build compiles none of it). `tools/dict_stage_report.py` sums a run up, applies the console model
(the owner's console logs' fit: 13.1 ms + 2.98 ms a read, v0.8.0-test.4, 338 cursor lookups,
R² 0.73) and compares two runs tap by tap: every digest must match.

```sh
build/host/vjo-dict-stage-bench DIR plaintext.vjdict,vndb-names.vjdict 2 cursor 5 < corpus.tsv > after.jsonl
python3 tools/dict_stage_report.py after.jsonl --before before.jsonl   # IDENTICAL RESULTS, reads delta
```

`prepared` instead of `cursor` locates each word first (`vjo_local_prepare_at`, Auto's lookahead), its
reads counted apart, and looks the located word up. `word` and `character` (#360) take a line's taps in
order with the worker's lookahead run to its end between them, in that highlight mode (the open overlay,
each line opened on its first word); their prepare reads are the background reads before each tap
([lookahead over the line](dictionary-lookahead-line.md)).

## On the rubi-dict-check corpus (main `892cfb9`, 2026-10-27)

64,579 taps of 5,000 hooked lines (46 train titles; the texts stay local), the bundled
`plaintext.vjdict` and `vndb-names.vjdict`; gcc 11.5 `-O2`, 5 rounds, warm OS cache (host µs are
CPU, not storage). Every tap's shown entry equals `vjo-dict-taps`' (rubi-dict-check's Rubi side).

| configuration | reads/lookup mean (p50 / p95 / max) | header | filter | index | group | head | payload | bytes | host µs mean (p50) | by stage, µs |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| console default (both dictionaries, shown 2), cursor | **8.12** (8 / 12 / 28) | 1.88 | 1.99 | 1.77 | 1.10 | 0.39 | 1.00 | 29,636 | **53.4** (45.4) | deinflect 3.2, search 44.5 (ranking 1.1), entry 2.4, stack 0.1, popup text 1.5 |
| one dictionary (`plaintext.vjdict`, shown 1), cursor | 4.48 (4 / 7 / 22) | 0.00 | 0.94 | 1.18 | 1.06 | 0.33 | 0.96 | 14,824 | 30.2 (25.6) | search 23.6 |
| console default, word located by the lookahead (7.13 reads of background work) | **1.04** (1 / 1 / 2) | 0 | 0 | 0 | 0 | 0 | 1.04 | 459 | **3.4** (3.4) | stack 3.0 |

What the numbers say (the plan is bee-san/Rubi_for_vita#360):

- **The CPU is not the cost.** 53 µs on the host is about 0.5 ms on the Cortex-A9; a 4 KiB read at
  a random offset of the 157 MB file costs about 3 ms on the console (the fit). Deinflection is
  3.2 µs, the index needs 1.1-1.8 page reads per tap (one per key found), the popup's text 1.5 µs.
- **The second dictionary costs 3.6 reads, 1.88 of them page-0 re-reads**: the session caches one
  filter directory and the two files evict each other at every lookup (one dictionary: 0.00 header
  reads).
- **One-character words** are 32 % of taps; their candidates skip the filter.
- **2.7 % of taps read 4 or more payload heads** (1.8 % read 8 or more, 16 for 私, which alone makes
  9,088 of the 24,910 head reads): a key in `preferred_readings` or `usual_spellings` is ranked from
  the heads, not the hints. Those taps make 18.9 reads (est. 69 ms).
- **A located word is the fastest lookup Rubi has**: one read, 3.4 µs.

The console's debug build then appends two log lines before it publishes the result (`lookup …`,
`job N done …`; test.4: 17 ms an append, `job done` 54 ms median against `lookup` 32 ms), and the
overlay needs a frame or two to lay the text out: how a 32 ms lookup is felt as about 100 ms. The
estimates and the changes are in #360.
