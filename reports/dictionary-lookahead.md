# Dictionary lookup preparation

This records the implementation committed as `0df7d88`. The subsequent
[cache experiments](dictionary-cache-experiments.md) retain its session and
lookahead behavior while reducing scratch usage and redundant payload reads.

Auto with Whole word prepares upcoming selections while the worker is idle. A
bounded cache keeps entry locations and misses, never full definitions. The first
word is still published before any lookahead work. Prepared hits use one payload
read and prepared misses use none; unprepared selections use the existing lookup.

## Before and after

The before executable is the previous handle-session implementation, preserved at
`build/lookup-speed-before/vjo-dict-bench` before rebuilding. Its SHA-256 is
`ad2c2eca80ad43a2b319000f237b045b535eea47203b79286c620be9187431b4`.
Both executables are normal host builds. Base source is `1f113ad6f29f452cb9ab7a81df6d8d2af5d31b8b`;
the first session change was uncommitted, so the executable hash identifies that
exact before version. Its counts also reproduce the previous
[session benchmark](../docs/benchmarks/dict-session-20261017.json).

The dictionary is the pinned shipped 157,298,688-byte file, SHA-256
`1c563c86a29801267c37fdf1def42650b02c69657f4144f09bd26d61b80d79b5`.
The 50 public VOCR lines in `host/fixtures/word_lookup/vocr_lines.tsv` produce 616
word selections. Eleven rounds verify every return code, definition digest,
entry count, cursor and highlight end. All outputs agree between versions.

Exact host file-operation counts:

| 616 selections | Before | After, unprepared | After, preparation opportunity |
|---|---:|---:|---:|
| Foreground reads | 3,349 | 2,850 | 890 |
| Background preparation reads | 0 | 0 | 1,960 |
| Total reads | 3,349 | 2,850 | 2,850 |
| Header-page reads in foreground | 549 | 50 | 50 |
| File opens, foreground + background | 50 | 50 | 50 |
| Foreground reads per selection | 5.44 | 4.63 | 1.44 |
| Host peak bytes, session included | 57,416 | 61,792 | 61,792 |

Directory reuse saves 499 reads (14.9%) without precomputation. With an idle
preparation opportunity before each subsequent selection, foreground reads fall
73.4%; total work is counted above, rather than treating precomputation as free.
Five preparations returned OOM because scratch would not fit beside the displayed
definition. Each fell back to normal lookup with identical results. The first
selection remains cold: the 50 first-word lookups make 312 reads in all variants.

This prepared case measures completed opportunities, not the console's actual
idle time or hit rate. The real scheduler prepares one word per idle job, up to
eight positions from the current selection, and gives foreground work priority.
The artifact's prepared character sweep is diagnostic only; runtime lookahead is
disabled in Character mode. Warm-host foreground timings are recorded as such;
they are not SD-card latency or a measured console speedup.

[All aggregates and provenance](../docs/benchmarks/dict-lookahead-20261017.json).
Raw per-selection JSONL and the local comparison script are under `build/`.
Reproduce the three word cases with the same directory, file and lines:

```sh
VJO_DICT_BENCH_SESSION=1 build/lookup-speed-before/vjo-dict-bench DIR plaintext.vjdict host/fixtures/word_lookup/vocr_lines.tsv words 11
VJO_DICT_BENCH_SESSION=1 build/host-normal/vjo-dict-bench DIR plaintext.vjdict host/fixtures/word_lookup/vocr_lines.tsv words 11
VJO_DICT_BENCH_PREPARE=1 build/host-normal/vjo-dict-bench DIR plaintext.vjdict host/fixtures/word_lookup/vocr_lines.tsv words 11
```

## Lifetime and budgets

The existing 65,536-byte result allocation holds the session followed by the
resettable arena. The session is 6,784 bytes on Vita and 6,856 on x86-64 [measured:
Vita disassembly and host benchmark peak]; its compile-time cap is 7 KiB. It holds
at most eight file handles, one 1,024-byte filter-directory buffer and sixteen
208-byte location records [source: `core/local_dict.c`, `core/local_dict.h`].
The shipped directory uses 904 bytes; directories over 256 blocks keep the original
page-cache path. No page-cache buffers or definitions were added to the session.

Preparation uses scratch after the displayed entry, releases it afterwards and
stops on OOM without touching the entry. It runs on the existing worker, after
100 ms idle, and yields after each word. Subtitles, refreshes, screenshots,
self-tests and user commands take priority. Opening during preparation can show
the cached word immediately; opening on a different screen starts that screen's
recognition after the in-flight preparation finishes. Cache keys include text
generation, cursor, configured dictionary set and result limit. Configuration,
I/O failures, overlay close and freeing results invalidate the relevant state.
As before, replace dictionaries with games closed.

The generated 569-rule table now uses string offsets and byte rule masks, eight
bytes per record instead of sixteen on the baseline Vita build [measured/source].
This pays for the code within the existing module page limits; rule order,
normalization, ranking and deinflection results are unchanged.

API 25 adds `vjoNotifyShell`, restricted to the registered Shell. Commands,
recognized text and completed jobs signal a bit in the existing kernel event.
The control thread wakes for it instead of relying on its 50 ms timeout. There
is no new event object, thread or allocation. This requires matching API-25
plugins; actual firmware wakeup latency is unmeasured.

## Validation and limits

The host lookup and worker suites cover retained definitions, inflections, misses,
generation invalidation, OOM fallback, user-input priority, opening during
preparation and changed screens. The benchmark checks equality across all modes
and timed rounds, including a deliberately changed result that must fail.
All 68 CTest entries outside the combined Python discovery group pass, including
the real bundled-dictionary worker test; the normal build's focused six entries
also pass. AddressSanitizer/UndefinedBehaviorSanitizer and the 38-test Cortex-A9
Linux lookup suite pass. The 67 focused Python tests include generated rules,
dictionary conversion, memory accounting and syscall ABI validation.

Normal and debug native plugins build within their existing page budgets. The
packaged syscall metadata/relocation checks pass for all three matching plugins.
On the same local Vita SDK, measured Shell loaded pages are:

| Shell variant | Before | After |
|---|---:|---:|
| Normal | 163,840 B | 163,840 B |
| Debug | 176,128 B | 172,032 B |

No budget number was raised. Normal Shell segments measure 87,548 text and
73,264 data/bss bytes; debug measures 98,052 and 73,264 bytes. These are packaged
module measurements, not console free-memory samples.

No plugin has been installed and no physical-console timing or CPU3 measurement
has been taken. Broader validation still has the separately reproduced baseline
OCR pin/name failures and the normal OCR module's existing exact-budget mismatch
described in the [first-stage report](dictionary-session.md).
