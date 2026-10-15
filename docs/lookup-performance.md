# Disk lookup benchmark and index design

Compared with the first local backend (`23fd3e0`), the new index cuts the two
sentence workloads from 3,005 / 3,267 file reads to 116 / 106 (96.1% / 96.8%
fewer). The returned definitions, ordering and highlight ranges match exactly.
Host lookup time improves about 15–17× for those sentences. These are **host
measurements**, not measured Vita or SD-card latency.

## What changed

1. **Page-sized hash buckets.** A normalized term hashes directly to a 4 KiB
   index page containing full 64-bit hashes. An ordinary miss needs one page
   read rather than a binary search repeatedly fetching scattered keys.
   A hit verifies the full key and reads its adjacent posting list; hashes
   are never accepted as proof of equality. Rare overflow pages handle
   crowded buckets. Dictionary size affects storage, not allocated RAM.
2. **Four shared LRU pages.** All enabled files share 16 KiB of page buffers.
   Repeated searches can reuse pages; this does not multiply per dictionary.
3. **A 64-slot absent-hash cache.** Proven misses are remembered for the current
   request. Filtered senses and duplicate results are not negative-cache entries,
   and a hash collision cannot create a false negative.
4. **Suffix dispatch.** Deinflection visits only rules with the matching final
   UTF-8 byte, preserving their previous order and all 569 source rules. It no
   longer scans the entire rule table for every candidate.

## Results

Jitendex Yomitan release **2026.10.03.0**, 436,549 imported entries / 662,343
index records / 558,655 unique normalized keys. The converter reports the same
13 truncated definitions and 6 skipped long headwords in both formats.

| Query | Old reads | New reads | Old median | New median | Host speedup |
| --- | ---: | ---: | ---: | ---: | ---: |
| 図書館 | 36 | 5 | 13.310 µs | 4.346 µs | 3.06× |
| としょかん | 32 | 4 | 13.155 µs | 3.966 µs | 3.32× |
| 食べませんでした | 186 | 9 | 56.059 µs | 6.890 µs | 8.14× |
| 今日は学校に行って、友達と話しました。 | 3,005 | 116 | 923.822 µs | 61.057 µs | 15.13× |
| 彼女の名前を思い出せなかった。 | 3,267 | 106 | 1004.102 µs | 59.555 µs | 16.86× |
| 猫。猫。猫。猫。猫。 | 168 | 5 | 50.305 µs | 5.137 µs | 9.79× |
| 霻霻霻霻霻霻霻霻 | 1,261 | 11 | 394.447 µs | 8.142 µs | 48.45× |

Lookup arena peaks range from **110,192 to 110,288 bytes (107.6–107.7 KiB)**
for these queries, versus 100,824–100,920 bytes before. The additional 9,368
bytes fit inside the existing **112 KiB lookup cap**: 64 KiB result capacity
plus at most 48 KiB scratch, checked at compile time. The current Lens worker uses two 256 KiB result arenas plus a bounded
connection pool from the guarded ScePaf heap. These figures do not include all process memory or stacks.

The tradeoff is storage: **92,845,647 → 103,097,379 bytes** (88.5 → 98.3 MiB,
about 11.0% larger) for this Jitendex conversion. The primary index has 4,096
buckets plus 15 overflow pages. A target occupancy of 140 out of 170 slots
keeps padding modest while avoiding long overflow chains. Conversion does
more work on the computer to build the grouped, hashed index.

## Method and limits

- `vjo-local-bench` calls the real overlay-from-text pipeline, with file callbacks
  that count reads, requested bytes, nonsequential offsets, and handle closure.
  The timer excludes process startup, OCR, terminal rendering and network calls.
- Each case has one unreported timing warmup and 100 measured requests. The
  application page/miss caches start empty on **every request**. The Linux OS
  file cache is warm; counted read calls are not claimed to be physical reads.
- A digest covers every displayed entry and its first-token/highlight coordinates.
  The comparison aborts if behavior differs; all seven cases match.
- Fixed caches are released after lookup. There is no across-screen resident
  dictionary cache or in-memory index whose size grows with installed data.
- SD-card random I/O is the hardware risk these changes address. For illustration
  only, an assumed 0.2 ms cost per read would contribute 601 ms before versus
  23.2 ms after for the first sentence. That is an arithmetic model, **not a Vita
  prediction**; actual device measurements are still needed.
- Tests cover v1 compatibility, mixed formats, deliberately colliding hashes,
  overflow buckets, invalid chains/offsets/postings, result equality, I/O reduction
  and the memory limit. ASan/UBSan also pass (LeakSanitizer is disabled because
  the execution environment restricts its process inspection).

[Raw measurements and source checksum](benchmarks/local-lookup-jitendex.json).
The benchmark dataset is not bundled. [Jitendex source release](https://github.com/stephenmk/stephenmk.github.io/releases/tag/2026.10.03.0).

## Reproduce

Build the current code and benchmark tool. Build the baseline with the same
compiler/options and the same instrumented driver:

```sh
git worktree add --detach .refs/lookup-before 23fd3e0
cp host/local_bench.c .refs/lookup-before/host/
cat >> .refs/lookup-before/CMakeLists.txt <<'CMAKE'
add_executable(vjo-local-bench host/local_bench.c)
target_link_libraries(vjo-local-bench vjo_host)
CMAKE
cmake -S .refs/lookup-before -B .refs/lookup-before/build/host -G Ninja
cmake --build .refs/lookup-before/build/host
cmake -S . -B build/host -G Ninja
cmake --build build/host
```

Place the pinned Jitendex ZIP in `build/lookup-benchmark/`, verify its SHA-256
against the raw report, and convert the exact same input into both formats:

```sh
python3 tools/convert_dictionary.py build/lookup-benchmark/jitendex.zip \
  --format-version 1 -o build/lookup-benchmark/jitendex-v1.vjdict
python3 tools/convert_dictionary.py build/lookup-benchmark/jitendex.zip \
  -o build/lookup-benchmark/jitendex-v2.vjdict
python3 tools/compare_lookup_benchmarks.py \
  --before .refs/lookup-before/build/host/vjo-local-bench \
  --after build/host/vjo-local-bench \
  --directory build/lookup-benchmark --runs 100
```

The query set is in `tools/lookup_benchmark_cases.json`; `--cases` accepts
another set. `--before-dicts` and `--after-dicts` accept comma-separated files
for multi-dictionary comparisons. `tools/benchmark_local_dictionary.py` remains
a reproducible synthetic size/memory stress test and now uses the timed driver
instead of timing CLI process startup.

## Real recognized lines (2026-10-15)

`tools/dict_lookup_bench.py` drives `vjo-dict-bench`, the production lookup with every read
attributed. It runs over the committed real VOCR lines and over line-label pools whose text stays
local. Each line is looked up as the Shell does: the first word, Right through the text, or every
letter. A per-screen sum and a console estimate from the owner's logs come with each run, and a
golden check fails on any change in what is shown (`tools/dict_lookup_golden.tsv`, CTest
`dict_lookup_golden`).

On main 8f1ec8f with the bundled `plaintext.vjdict`, a lookup makes 12.6-16.0 reads:
- 5.8-8.5 for deinflection candidates that are not keys, nearly all of them index pages;
- the found key's bucket page, its key group, 2.6-4.0 payload heads to rank its entries, a
  header and the shown payload.

The console estimate is 56-74 ms at p50. The analysis, hoshidicts' design, the method and all
results are in [reports/dict-lookup-speed.md](../reports/dict-lookup-speed.md), with the raw
numbers in [dict-lookup-baseline-20261015.json](benchmarks/dict-lookup-baseline-20261015.json).

The bundled dictionary with frequency ranks (format 3, #127) costs about the same. On the committed
lines, main `ac905b1` with its bundle (format 2 and the key filter) against this change with the
format 3 bundle and the same filter, a lookup makes 8.48 → 8.42 reads (first word), 8.59 → 8.62
(Right through the text) and 9.28 → 9.37 (every letter). The difference is in the ranking's payload
heads (3.40 → 3.28, 3.52 → 3.55, 4.00 → 4.09): other entries are shown, and each payload is 4 B
longer, which shifts page boundaries. The console estimate's mean moves by -0.2 to +0.4 ms (53.5 →
53.3, 54.0 → 54.1, 56.5 → 56.9 ms) and its p50 by one read's step at most (48.0 → 51.7, 48.4 → 48.0,
51.7 → 51.7 ms; estimates). The result arena's peak is 53,840 → 54,416 B of its 65,536. The golden
file is recorded on the format 3 bundle with the filter. Every digest changes, because an entry's
text now carries its rank. With the badge text held equal, the ranks change the entry shown for 6 of
50 first words, 36 of 629 word steps and 97 of 1,321 letters.

```sh
python3 tools/dict_lookup_bench.py --bench build/host/vjo-dict-bench --directory build/host/plaintext \
  [--before OTHER_BENCH | --before-directory DIR | --before-dicts FILES] \
  [--pool LABELS.jsonl --reserved /path/to/vita-vn-ocr/eval/manifests/reserved.json] --rounds 21 -o out.json
```

### Key filter

The key filter ([format](local-dictionaries.md#key-filter)) answers most deinflection
candidates that are not keys from one 4 KiB Bloom block per lookup. The block is chosen by
the cursor's first two characters and read through the existing page cache.

Whole word stepping, the same lookups, every shown entry identical; console times are
estimates from the console fit:

| line set | lookups | reads | index-page reads | console ms p50 / p95 (est.) | per screen p50 / p95 (est.) |
|---|---:|---:|---:|---:|---:|
| 50 committed VOCR lines | 629 | 13.51 → 8.59 | 6.96 → 1.13 | 66.7 → 48.4 / 122.6 → 77.8 | 962 → 714 / 1532 → 1213 ms |
| 500 realplus-v3 screens | 5,005 | 15.21 → 8.34 | 8.94 → 1.14 | 70.4 → 48.0 / 148.7 → 81.6 | 485 → 358 / 2175 → 1361 ms |
| 500 owner-pool-2 screens | 5,423 | 15.78 → 8.33 | 9.55 → 1.15 | 70.4 → 48.0 / 148.7 → 77.8 | 744 → 521 / 2034 → 1238 ms |

The filter costs 0.91-1.00 reads and 0 bytes of RAM, and adds 904 KiB to the bundled file.
Results for every mode and set:
[dict-key-filter-20261015.json](benchmarks/dict-key-filter-20261015.json).

## Dictionary pages

`tools/benchmark_dictionary_pages.py` measures the selected-character lookup
and each **Dictionaries shown** page against a baseline build, interleaving the
builds in rounds. It fails unless the first entry is identical and a single
result reads exactly as before. `vjo-local-bench DIR FILES TEXT RUNS CURSOR
RESULTS` prints the first lookup's residency and every other page's time, reads,
files opened and arena peak. Results:
[dictionary-pages-20261012.json](benchmarks/dictionary-pages-20261012.json).

```sh
python3 tools/benchmark_dictionary_pages.py --before .refs/main/build/host/vjo-local-bench \
  --after build/host/vjo-local-bench --directory DIR \
  --dicts jitendex.vjdict,jmdict.vjdict --runs 20 --rounds 5
```
