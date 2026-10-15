# Dictionary lookup speed: hoshidicts' on-disk tech, our `.vjdict`, a real-line benchmark

The owner (2026-10-15): "analyse how the dicts work, I love that they aren't in memory.
Hoshidicts has some great tech for dicts, such as bloom filters. Benchmark our dict lookup and
see if we can speed it up at all."

**Baseline (main 0023e8a, the bundled `plaintext.vjdict`):** a word lookup makes 12.6-16.0 file
reads. Of those, 6.8-9.6 are hash-index pages read only to learn that a deinflection candidate is
not a key, and 2.6-4.0 are payload heads read to rank a key's entries. The console estimate is
55-73 ms at p50 and 122-155 ms at p95 per lookup. A screen read word by word takes 0.5-0.9 s at
p50 and 1.5-2.2 s at p95.
The CPU part is 0.04-0.05 ms on the host.

All console times here are **estimates**, made from read counts with a fit of the owner's console
logs (§4). Read counts are exact host measurements of the production code.

## 1. hoshidicts (Manhhao/hoshidicts e6af33e1ab3241e16b885ec81f048f05d81f766b, read only)

Line numbers are at that commit.

- **On disk.** Each dictionary is a folder: `hash.table`, `bloom.filter` and `blobs.bin`; for
  version 4, a zstd dictionary `dict.zstd`; `media.bin`/`media.idx`; `index.json`; and a version
  marker `.hoshidicts_N` (`src/query.cpp:91-101`, `src/importer.cpp:756`).
- **What is kept resident.** Opening a dictionary mmaps the table, the filter and the blobs
  (`src/query.cpp:125-147`, `src/memory/memory.cpp:59-80`, `mmap(PROT_READ, MAP_SHARED)`).
  `dict.zstd` and the parsed `index.json` are read into RAM (`src/query.cpp:106-120, 149-153`).
  Data is on disk, and the OS page cache decides what stays resident. The library has no
  cache of its own, apart from one zstd context per thread (`src/query.cpp:39-42`).
- **Bloom filter.** One bit array per dictionary, keyed by the XXH3-64 of each distinct
  expression and reading string (`src/importer.cpp:284-287`, deduplicated in
  `build_offset_index`, `:600-622`, and built at `:736-741`). It has `bit_ceil(max(10 n, 64))`
  bits (`src/hash/bloom.cpp:28`), so 10-20 bits per key, and k = 7 probes (`:13`). The probes use
  double hashing over the whole array: `bit = (h1 + k*h2) & mask`, with h1 and h2 the low and high
  32 bits of the hash (`src/hash/bloom.hpp:12-22`). For our 661,469 keys that is 8,388,608 bits
  (1 MiB, 12.7 bits a key), giving about 0.3% false positives (estimate: (1 - e^(-7/12.7))^7).
- **Index.** `hash.table` is an open-addressing table with linear probing (`src/hash/hash.cpp:18-33`).
  It has `n * 10 / 7` slots (`:37`) of 16 bytes each, {u64 hash, u64 offset} (`src/hash/hash.hpp:21-24`).
  A lookup checks the filter first (`hash.cpp:20`) and probes the table only when the filter says
  "maybe". Only the 64-bit hashes are compared. The slot points to an offset list in `blobs.bin`
  (u32 count + u64 record offsets, `importer.cpp:612-618`). Each term record is checked against
  the query string (`src/query.cpp:244`), so a hash collision cannot show a wrong term. The data
  has no sorted keys, FST or block index.
- **Records.** A term record holds a type byte, the expression, the reading, the glossary offset
  and size, the tags, the rules and the score (`importer.cpp:264-282`). Glossaries are compressed
  with zstd and a trained dictionary of at most 110 KiB (`:178-222`), and identical glossaries are
  stored once (`:244-256, 478-484`). They are decompressed only for the results that are shown
  (`src/lookup.cpp:193-195`, `src/query.cpp:521-525`).
- **Query path.** `Lookup::lookup` scans every prefix length from 16 code points down to 1
  (`src/lookup.cpp:59`). It does not stop at the first length that matches; it keeps the longest
  match per expression and reading (`:68-89`). Each prefix is expanded into the text-processor
  variants (`src/text_processor/text_processor.cpp:296-379`: a product of NFKC, katakana/hiragana,
  emphatic collapse, full-width, kanji variants, iteration marks, numbers and middle dots). Each
  variant is deinflected recursively (`src/deinflector.cpp:1294-1328`), and every deinflection is
  queried (`src/lookup.cpp:63-66`).
- **Pruning.** Candidates are not pruned before they are queried: the deinflector "may include
  invalid deinflections" (README), and part-of-speech filtering runs after the query
  (`src/lookup.cpp:200-208`). The only skip without I/O is the Bloom filter in front of the table.
  For each query's results, frequency and pitch are looked up before that filter
  (`src/query.cpp:307-309`).
- **Frequency and pitch.** These are separate dictionaries with the same files. Meta records
  (type 1) hold the expression, a mode and the raw JSON (`importer.cpp:312-318`), keyed by the
  hash of the expression only (`:320`). The JSON is parsed at query time, and records are matched
  by reading if the record has one (`src/query.cpp:314-432`).
- **Page sizes.** hoshidicts has none of its own: the OS mmap granularity applies
  (`memory.cpp:13-21`).

**What transfers to the Vita.** hoshidicts relies on mmap and a page cache, and the Shell has
neither. Its data is not in memory either, but each Bloom probe touches seven random bits of a
1 MiB array. On our 5,634 real lookups (§5), a filter laid out like hoshidicts' would need
**33-39 distinct 4 KiB filter pages per lookup at p50** (mean 41.6-50.2). That is more than the
7-9 index reads it would save.

The idea is still right, but on the Vita the filter has to be laid out so that one lookup's
candidates share a page. Every candidate at one cursor starts with the cursor's characters, which
is what makes this possible (§6).

## 2. Our format and lookup (`.vjdict` v2, `core/local_dict.c` at 0023e8a)

- **Index.** Keys are normalized (katakana to hiragana, full-width ASCII to ASCII). Each key is
  hashed with FNV-1a-64 and a finalizer (`key_hash`, `:422`) to one 4 KiB bucket page. A page holds
  170 records of 24 bytes: hash, group offset, posting count, key length, rule union. The converter
  doubles the bucket count until there are at most 140 keys a bucket on average
  (`tools/convert_dictionary.py:256`). The bundled file has 8,192 buckets for 661,469 keys: a
  32 MiB index of 47% occupied pages, with no overflow pages.
- **Key groups and postings.** A group holds the key bytes, then one 16-byte posting per entry:
  payload offset, length, rules and a reserved u16 that must be 0 (`:282`). Payloads are spelling
  NUL reading NUL definition NUL, in source order.
- **Per lookup.** Each lookup opens every configured file, reads its 64-byte header (`check_header`,
  `:178`) and closes it again at the end (`open_dictionaries`, `:204`). Nothing stays open or
  resident between lookups.
- **Selected-character lookup** (`vjo_local_lookup_pages`, `:671`). Spans start at the cursor, up
  to 32 code points, longest first (`:738`). For each span, `deinflect` (`:496`) generates up to 128
  candidates breadth first (`:742`). Each candidate searches each dictionary (`find_matches_v2`,
  `:432`): one bucket page, a binary search, and a full key comparison in the group. Then
  `take_postings` (`:364`) ranks each posting by reading a 160-byte head of its payload
  (`rank_posting`, `:320`): written form, kana form, reading, preferred reading, length. The lookup
  stops at the first span with a match. Proven-absent hashes go into a 64-slot miss cache for the
  request (`:480`).
- **Memory.** The `Workspace` (45.2 KB on the host) fits the 48 KiB scratch bound (`:70`,
  `VJO_LOCAL_SCRATCH_BUDGET`): four shared 4 KiB LRU pages, 128 candidates of 198 bytes, and the
  miss cache. With the 8,448-byte selected-output budget, it sits inside the Shell's 64 KiB local
  result arena (`shell/worker.c:29`, a Paf allocation) and is released after each lookup.
  Lookups run in SceShell (AGENTS.md rule 1; overlay-shell-slim keeps them there because some
  games have no `ux0:`).
- **I/O.** A read is `sceIoLseek` + `sceIoRead` (`shell/platform_vita.c:266-279`). An open is
  `sceIoOpen` + a seek to the end (`:283-289`).

**What the reads are** (§5, Whole word stepping): one header read, 7.0-9.6 index pages, about 1
key group, 3.2-3.5 payload heads and one payload. Of 8.4-11.7 deinflection candidates generated
per lookup, 7.95-11.1 are searched (the rest come after the match) and 1.03-1.04 are found.
**Seven to nine of every lookup's reads find nothing.**

## 3. The benchmark (`host/dict_bench.c`, `tools/dict_lookup_bench.py`)

`vjo-dict-bench` runs the production `core/local_dict.c`, built with `VJO_LOCAL_STATS` (counters
only; every other build compiles none of them). It uses the Shell's call
(`vjo_overlay_lookup_at`), the release defaults (Whole word, Japanese-only, one dictionary page)
and a fresh 64 KiB arena per lookup.

Each line becomes the overlay's sentence (`vjo_overlay_ocr_text`) and is looked up in three modes:
- **first:** the first word, which is what Auto OCR prepares.
- **words:** then Right after each match to the end of the text (`vjo_word_step`).
- **letters:** every selectable letter (Character mode, or a tap on each).

For each lookup it records:
- every read, by what it fetches (header, index, group, head, payload, filter);
- bytes, distinct pages and nonsequential reads;
- the candidates generated, searched and found;
- the arena peak;
- the median host time of N-1 timed rounds;
- a digest of the shown entry's text and highlight.

`tools/dict_lookup_bench.py` adds the per-screen sums and the console model. Given a second build
or file, it fails unless every lookup in every mode shows the same entry.
`tools/dict_lookup_golden.tsv` (digests only) pins what the 50 committed lines show with the
bundled file; CTest's `dict_lookup_golden` checks it.

**Line sets:**
- the 50 committed real VOCR lines (`host/fixtures/word_lookup/vocr_lines.tsv`);
- 500 screens of vita-vn-ocr's realplus-v3 line labels;
- 500 screens of the owner's owner-pool-2 labels.

The two pools use kept, non-name lines grouped by screenshot, with screenshots ordered by a
seeded sha256. Every VN in `reserved.json`'s `test` set is left out (vita-test, psp-test, the
CLANNAD family and their shared releases). Pool text is read into a temporary file and never
committed; only aggregates are.

The host OS file cache stays warm, so host times measure CPU, not storage.

## 4. Console model (estimate)

The 90 lookup log lines of the v0.8.0-test.1 and test.2 console logs (private; only the fit is
quoted) give **ms = 21.0 + 3.73 x reads** (least squares, R² 0.56, reads 5-33 per lookup).
`docs/performance.md` has 15.3 + 3.82 from test.1's 30 lookups.

The fixed part is the open, size seek and close, plus CPU (0.19-0.64 ms on the A9,
`docs/performance.md`). Bytes do not improve the fit. Reads above 4 KiB, which those lookups never
made, are bounded at 0.146 ms per extra KiB: the 1,155,968 B of VOCR3-H15 weights load in 32 KiB
reads within 218-231 ms, SHA-256 included.

Any change below that saves reads saves about 3.7 ms per read on the console, whatever the true
intercept is.

## 5. Baseline

Main 0023e8a's lookup; gcc 11.5 `-O2 -g` on the host; 21 rounds. Raw numbers:
[`docs/benchmarks/dict-lookup-baseline-20261015.json`](../docs/benchmarks/dict-lookup-baseline-20261015.json).

| set | mode | lookups | reads mean (p50/p95) | header | index | group | head | payload | KiB read | candidates gen. / searched / found | console ms p50 / p95 (est.) | per screen ms p50 / p95 (est.) | host µs p50 |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixture (50 lines) | first | 50 | 14.28 (11/36) | 1.00 | 7.88 | 1.00 | 3.40 | 1.00 | 49.6 | 9.24 / 8.98 / 1.00 | 62.0 / 155.3 | 62 / 155 | 39.2 |
| fixture (50 lines) | words | 629 | 13.51 (12/27) | 1.00 | 6.96 | 1.03 | 3.52 | 0.99 | 46.9 | 8.43 / 7.95 / 1.03 | 65.8 / 121.7 | 948 / 1517 | 44.5 |
| fixture (50 lines) | letters | 1,321 | 13.97 (12/29) | 1.00 | 6.87 | 1.10 | 4.00 | 0.99 | 48.8 | 8.41 / 7.80 / 1.09 | 65.8 / 129.2 | 1958 / 2999 | 45.2 |
| realplus-v3 (500 screens) | first | 500 | 12.57 (9/32) | 1.00 | 6.80 | 1.04 | 2.73 | 1.00 | 42.8 | 8.08 / 7.70 / 1.04 | 54.6 / 140.4 | 55 / 140 | 39.7 |
| realplus-v3 (500 screens) | words | 5,005 | 15.21 (13/34) | 1.00 | 8.94 | 1.04 | 3.23 | 1.00 | 53.7 | 10.87 / 10.39 / 1.04 | 69.5 / 147.8 | 478 / 2155 | 47.9 |
| realplus-v3 (500 screens) | letters | 10,905 | 15.43 (13/34) | 1.00 | 8.63 | 1.08 | 3.73 | 0.99 | 54.6 | 10.58 / 9.98 / 1.06 | 69.5 / 147.8 | 1076 / 4508 | 47.5 |
| owner-pool-2 (500 screens) | first | 500 | 13.84 (10/35) | 1.00 | 8.26 | 1.02 | 2.57 | 1.00 | 47.9 | 9.84 / 9.42 / 1.03 | 58.3 / 151.6 | 58 / 152 | 40.9 |
| owner-pool-2 (500 screens) | words | 5,423 | 15.78 (13/34) | 1.00 | 9.55 | 1.04 | 3.19 | 1.00 | 55.9 | 11.65 / 11.13 / 1.04 | 69.5 / 147.8 | 733 / 2016 | 49.8 |
| owner-pool-2 (500 screens) | letters | 12,188 | 16.01 (14/34) | 1.00 | 9.30 | 1.08 | 3.64 | 0.99 | 56.9 | 11.45 / 10.83 / 1.06 | 73.2 / 147.8 | 1678 / 4577 | 51.1 |

Other measurements:
- Nearly every read is at a new offset: 11.2-14.0 nonsequential reads per lookup, over 11.5-14.8
  distinct pages.
- Host throughput is 16,700-19,900 lookups/s.
- The arena peak is 53,648 B of the 64 KiB in every set (host build).
- 98.9-100% of lookups show an entry.

The CPU is not the problem; the file reads are.

## 6. Speed-ups

Each change is measured against the baseline with the golden check: the same entry and highlight
for every lookup of every set. Each lands in its own pull request, and the results are added here.

1. **A key filter with locality** (next PR): Bloom blocks of 4 KiB partitioned by the key's first
   two code points. It is read through the existing page cache, so it adds no RAM, and it is
   appended to the file, so readers without it still work. A simulation on the traces gives 0.9
   filter reads plus 1.1 index reads per lookup, replacing 7-9 index reads.
2. **Posting rank hints** in the v3 postings, which dict-freq's format leaves to this work:
   bits 0-2 hold spelling = key, normalized spelling = key, and has-a-reading. Ranking then needs
   no payload heads, saving 2.6-4.0 reads.
3. **Keeping the file open across lookups**, an option overlay-shell-slim agrees to: it would save
   the per-lookup open, seek and close (most of the 21 ms intercept, if the fit attributes it
   rightly). This needs a console test before any claim.
