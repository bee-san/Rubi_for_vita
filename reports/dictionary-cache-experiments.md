# Dictionary scratch and cache experiments

Baseline: `0df7d88`, the committed and pushed session/directory cache, Auto
lookahead and event wakeups. This comparison measures additional improvements
against that version, rather than the older stateless implementation.

The kept changes allocate deinflection candidates as needed and reuse a payload
already wholly present in a lookup's page cache. Definitions, ranking, highlight
spans, file format, session lifetime and the 128-candidate bound stay the same.

## Results

The same pinned 157,298,688-byte dictionary and 50 committed VOCR lines were used
for both versions, with 11 rounds. SHA-256 pins, exact executable hashes and all
first-word, word and character aggregates are in the
[experiment artifact](../docs/benchmarks/dict-cache-experiments-20261017.json).
Every return code, entry count, definition digest and highlight agrees in all
cases and timed rounds.

Measured host counts for 616 word selections:

| Metric | Baseline | Kept changes |
|---|---:|---:|
| Unprepared selection reads | 2,850 | 2,825 |
| Unprepared arena high-water mark, session included | 61,792 B | 39,640 B |
| Prepared foreground reads | 890 | 870 |
| Preparation reads | 1,960 | 1,974 |
| Prepared total reads | 2,850 | 2,844 |
| Prepared arena high-water mark, session included | 61,792 B | 43,984 B |
| Preparations skipped for OOM | 5 | 0 |

Ordinary selections save 25 reads (0.9%). With preparation opportunities, the
extra scratch space moves more searches into the background: foreground reads
fall by 20 (2.2%) and total reads fall by six. All five previously skipped
preparations now fit. The 50 cold first selections use 306 reads instead of 312;
the 1,321-character sweep uses 6,028 instead of 6,064 reads with sessions.

The prepared case assumes an idle opportunity before each subsequent selection;
it does not reproduce a console's idle intervals or eight-position lookahead
hit rate. Its total is 19 reads above the new unprepared case: background
preparation releases the page buffers, so selection can no longer reuse some
payload bytes that were in a ranking-head page. This is the measured work/latency
tradeoff, with background work included.

These high-water marks describe used space inside the existing allocation. The
Shell still allocates 65,536 bytes; this does not return 22 KiB to the system heap.
It leaves more of that block available while the displayed entry remains alive.
Warm-host timings are diagnostic only: competing load and clocks were not
controlled. No console latency or CPU3 reduction is claimed.

## Experiments and decisions

| Experiment | Observation on the word workload | Decision |
|---|---|---|
| Grow candidates from eight to at most 128 | Same total reads; lower scratch peak; all five preparations fit | Keep |
| Retain the existing four page buffers across selections | No reads saved over adaptive scratch alone on the forward sweeps | Drop |
| Retain pages and reuse cached payloads | 2,825 total prepared reads, 19 fewer than the kept version | Drop persistent lifetime for this small additional saving |
| Reuse payloads only inside the current lookup | 25 reads saved for unprepared selections; no retained page state | Keep |

Persistent pages are not universally ineffective: the experiment only tests the
recorded navigation workloads. The prototypes and exact measured executables are
preserved locally under `build/`; their hashes and aggregates are in the artifact.
The selected implementation retains only the original handle/header, directory
and word-location session state.

Candidate storage remains the last allocation in the parent arena and grows in
place, preserving breadth-first candidate order. Failure to grow returns OOM and
closes the lookup normally; it never truncates to the currently allocated count.
A compile-time check covers the worst-case session, output, locators and full
128-candidate scratch within 64 KiB. Cached payload copying requires every byte
to be inside one already loaded page; cross-page entries keep their direct read
and all payload validation still runs.

Reproduce the selected comparison after building `vjo-dict-bench` in normal host
builds for `0df7d88` and this revision:

```sh
VJO_DICT_BENCH_SESSION=1 BEFORE DIR plaintext.vjdict host/fixtures/word_lookup/vocr_lines.tsv words 11 > before.jsonl
VJO_DICT_BENCH_SESSION=1 AFTER DIR plaintext.vjdict host/fixtures/word_lookup/vocr_lines.tsv words 11 > after.jsonl
VJO_DICT_BENCH_PREPARE=1 BEFORE DIR plaintext.vjdict host/fixtures/word_lookup/vocr_lines.tsv words 11 > before-prepared.jsonl
VJO_DICT_BENCH_PREPARE=1 AFTER DIR plaintext.vjdict host/fixtures/word_lookup/vocr_lines.tsv words 11 > after-prepared.jsonl
```

## Validation and next target

The lookup tests cover a candidate-vector growth failure and retry, payloads that
fit or cross pages, and every real file-read failure for both small and 5 KiB
entries. All 39 lookup tests also pass on Cortex-A9 Linux under qemu-arm.
Host worker tests include the real shipped dictionary. Sanitizer,
normal/debug native build and packaged syscall checks pass. All 68 CTest entries
outside the combined Python discovery group, six normal-build focused entries
and 67 focused Python tests pass. Existing unrelated OCR pin/budget failures
remain as documented in the previous report.

Measured native Shell pages stay at 163,840 B for normal and 172,032 B for debug
on the same local SDK. Their text/data-bss segments are 87,756/73,264 B and
98,276/73,264 B respectively. No allocation or budget was added. No console
installation or performance run was performed.

The next larger opportunity is changing storage layout: ordinary word selections
still make 644 group-page reads after reaching their index buckets. Packing keys
and postings with the corresponding buckets could remove some of those separate
reads. That needs a converter/format prototype and its own equality benchmark;
it has not been tested in this round.
