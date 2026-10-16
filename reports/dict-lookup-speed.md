# Dictionary lookup speed: hoshidicts' on-disk tech, our `.vjdict`, a real-line benchmark

The owner (2026-10-15): "analyse how the dicts work, I love that they aren't in memory.
Hoshidicts has some great tech for dicts, such as bloom filters. Benchmark our dict lookup and
see if we can speed it up at all."

**Baseline (main 8f1ec8f, whose lookup code is 0023e8a's; the bundled `plaintext.vjdict`):** a word
lookup makes 12.6-16.0 file reads. Of those:
- 5.8-8.5 are reads for deinflection candidates that are not keys: their bucket pages, and
  rarely the key group of another key with the same hash;
- about 1 is the found key's own bucket page;
- 2.6-4.0 are payload heads read to rank a key's entries.

The console estimate is 56-74 ms at p50 and 123-156 ms at p95 per lookup. A screen read word by
word takes 0.5-1.0 s at p50 and 1.5-2.2 s at p95. The CPU part is 0.04-0.05 ms on the host.

**Results** (§6; Whole word stepping over the 50 committed lines and 500 screens each of two label
pools, every shown entry and highlight identical at each step):

| step | reads per lookup | console ms p50 (est.) | RAM added | disk |
|---|---:|---:|---:|---:|
| baseline (#118) | 13.5-15.8 | 66.7-70.4 | | |
| key filter (#123, 6.1) | 8.3-8.6 | 48.0-48.4 | 0 | +904 KiB |
| posting hints (6.3) | 5.3-5.5 | 40.5 | 0 | 0 |

The dictionary stays on disk: nothing of it is resident between lookups, and the Shell's permanent
memory is unchanged.

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
  32 bits of the hash. A lookup stops at the first clear bit (`src/hash/bloom.hpp:12-22`; the loop
  is `:15-19`). For our 661,469 keys that is 8,388,608 bits (1 MiB, 12.7 bits a key). Their bit
  density is 0.424, so the filter has 0.247% false positives ((1 - e^(-7/12.7))^7, and 0.424^7).
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
  Frequency and pitch are looked up for each query's results before the part-of-speech filter
  drops any of them (`src/query.cpp:307-309`).
- **Frequency and pitch.** These are separate dictionaries with the same files. Meta records
  (type 1) hold the expression, a mode and the raw JSON (`importer.cpp:312-318`), keyed by the
  hash of the expression only (`:320`). The JSON is parsed at query time, and records are matched
  by reading if the record has one (`src/query.cpp:314-432`).
- **Page sizes.** hoshidicts has none of its own: the OS mmap granularity applies
  (`memory.cpp:13-21`).

**What transfers to the Vita.** hoshidicts relies on mmap and a page cache, and the Shell has
neither. Its data is not in memory either, but each Bloom probe tests up to seven random bits of
a 1 MiB array; at its 0.424 density, an absent key tests 1.7 on average. On our 11,057 real
lookups (§5, Whole word), a filter laid out like hoshidicts' would need **14-17 distinct 4 KiB
filter pages per lookup at p50** (mean 17.0-20.9). That is more than the 5.8-8.5 index reads it
would save.

The idea is still right, but on the Vita the filter has to be laid out so that one lookup's
candidates share a page. Every candidate at one cursor starts with the cursor's characters, which
is what makes this possible (§6).

## 2. Our format and lookup (`.vjdict` v2, `core/local_dict.c` at 0023e8a, the same at 8f1ec8f)

- **Index.** Keys are normalized (katakana to hiragana, full-width ASCII to ASCII). Each key is
  hashed with FNV-1a-64 and a finalizer (`key_hash`, `:422`) to one 4 KiB bucket page. A page holds
  170 records of 24 bytes: hash, group offset, posting count, key length, rule union. The converter
  doubles the bucket count until there are at most 140 keys a bucket on average
  (`tools/convert_dictionary.py:256`). The bundled file has 8,192 buckets for 661,469 keys: a
  32 MiB index of 47% occupied pages, with no overflow pages.
- **Key groups and postings.** A group holds the key bytes, then one 16-byte posting per entry:
  payload offset, length, rules and a reserved u16 that must be 0 (`:282`). Payloads are spelling
  NUL reading NUL definition NUL, in source order.
- **Per lookup.** Each lookup opens every configured file and reads its 64-byte header
  (`open_dictionaries`, `:204`; `check_header`, `:178`). It closes the files again at the end of
  the lookup (`done:`, `:796`). Nothing stays open or resident between lookups.
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

**What the reads are** (§5, Whole word stepping):
- one header read;
- 7.0-9.6 index pages: 6.0-8.5 for candidates that are not keys, and about 1 for the found key's
  bucket;
- about 1 key group, 3.2-3.5 payload heads and one payload.

Of 8.4-11.7 deinflection candidates generated per lookup, 7.95-11.1 are searched (the rest come
after the match), and 1.03-1.04 are found. **Six to nine of every lookup's reads find nothing.**

## 3. The benchmark (`host/dict_bench.c`, `tools/dict_lookup_bench.py`)

`vjo-dict-bench` runs the production `core/local_dict.c`, built with `VJO_LOCAL_STATS` (counters
only; every other build compiles none of them). It uses the Shell's call
(`vjo_overlay_lookup_at`), the release defaults (Whole word, Japanese-only, one dictionary page)
and a fresh 64 KiB arena per lookup.

Each line is normalised as the game worker returns recognized text (`vjo_text_normalize`, #88).
It then becomes the overlay's sentence (`vjo_overlay_ocr_text`), and is looked up in three modes:
- **first:** the first word, which is what Auto OCR prepares.
- **words:** then Right after each match to the end of the text (`vjo_word_step`): every word in
  reading order, as one row. The Shell wraps Right within the header's laid-out rows; as a set of
  lookups that changes nothing.
- **letters:** every selectable letter (Character mode, or a tap on each).

For each lookup it records:
- every read, by what it fetches (header, index, group, head, payload, filter), and the index and
  group reads of candidates that found no key;
- bytes, distinct pages and nonsequential reads;
- the candidates generated, searched and found. In a v2 file "found" is a key with fitting rules,
  and "hash present" is any key with the candidate's hash. In a v1 file "found" is the key, whatever
  its rules, and "hash present" is not counted.
- the arena peak;
- the median host time of N-1 timed rounds, without the page bookkeeping;
- a digest of the shown entry's text and highlight. A timed round that shows something else
  fails the run.

`tools/dict_lookup_bench.py` adds the per-screen sums and the console model. Given a second build
or file, it fails unless every lookup in every mode shows the same entry.
`tools/dict_lookup_golden.tsv` (digests only) pins what the 50 committed lines show with the
bundled file; CTest's `dict_lookup_golden` checks it.

**Line sets:**
- the 50 committed real VOCR lines (`host/fixtures/word_lookup/vocr_lines.tsv`);
- 500 screens of vita-vn-ocr's realplus-v3 line labels: the snapshot `labels_v3i7.jsonl`, sha256
  259729ce…, since the live `labels.jsonl` keeps changing;
- 500 screens of the owner's owner-pool-2 labels (`labels.jsonl`, sha256 b16a529e…).

The tool records each pool file's sha256 and its selection's. The JSON has them in full.

The two pools use kept, non-name lines grouped by screenshot, with screenshots ordered by a
seeded sha256. Every VN in `reserved.json`'s `test` set is left out (vita-test, psp-test, the
CLANNAD family and their shared releases). Pool text is read into a temporary file and never
committed; only aggregates are.

The host OS file cache stays warm, so host times measure CPU, not storage.

## 4. Console model (estimate)

The owner's v0.8.0-test.2 console log (private; only fits are quoted) has 57 lookup lines. Its
first 4,966 lines are test.1's whole log, so test.1 adds no other lookup. A least-squares fit of
the 57 gives **ms = 21.9 + 3.73 x reads** (R² 0.52, reads 5-33 per lookup). By kind of lookup:

| lookups | fit |
|---|---|
| cursor (Right, a tap), n 35 | 11.2 + 3.76 x reads, R² 0.85 |
| first word and on open, n 22 | 35.4 + 3.91 x reads, R² 0.27 |

The fastest lookup at each of the 21 read counts follows 10.2 + 3.49 x reads.
`docs/performance.md` has 15.3 + 3.82, from test.1's 30 lookups.

- **The fixed part** covers the open, size seek and close, and the CPU (0.19-0.64 ms on the A9,
  `docs/performance.md`). For the cursor lookups the user waits on, it is about 11 ms. A first-word
  lookup's logged time also starts just before the lookup (`shell/worker.c:1100-1104`), so the OCR
  job's steps are not in it. Why its intercept is higher (35 ms) is unmeasured.
- **Bytes.** They do not improve the fit. Reads above 4 KiB, which those lookups never made, are
  bounded at 0.146 ms per extra KiB: the 1,155,968 B of VOCR3-H15 weights load in 32 KiB reads
  within 218-231 ms, the A9 SHA-256 estimate included.

**A read costs 3.5-3.9 ms** in every one of these fits, so any change that saves reads saves
about 3.7 ms per read, whatever the intercept. The tables use 21.9 + 3.73 x reads.

## 5. Baseline

Main 8f1ec8f's lookup; gcc 11.5 `-O2 -g` on the host; 21 rounds. Raw numbers:
[`docs/benchmarks/dict-lookup-baseline-20261015.json`](../docs/benchmarks/dict-lookup-baseline-20261015.json).

| set | mode | lookups | reads mean (p50/p95) | header | index | group | head | payload | KiB read | candidates gen. / searched / found | console ms p50 / p95 (est.) | per screen ms p50 / p95 (est.) | host µs p50 |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fixture (50 lines) | first | 50 | 14.28 (11/36) | 1.00 | 7.88 | 1.00 | 3.40 | 1.00 | 49.6 | 9.24 / 8.98 / 1.00 | 62.9 / 156.2 | 63 / 156 | 38.3 |
| fixture (50 lines) | words | 629 | 13.51 (12/27) | 1.00 | 6.96 | 1.03 | 3.52 | 0.99 | 46.9 | 8.43 / 7.95 / 1.03 | 66.7 / 122.6 | 962 / 1532 | 41.9 |
| fixture (50 lines) | letters | 1,321 | 13.97 (12/29) | 1.00 | 6.87 | 1.10 | 4.00 | 0.99 | 48.8 | 8.41 / 7.80 / 1.09 | 66.7 / 130.1 | 1985 / 3036 | 43.3 |
| realplus-v3 (500 screens) | first | 500 | 12.57 (9/32) | 1.00 | 6.80 | 1.04 | 2.73 | 1.00 | 42.8 | 8.08 / 7.70 / 1.04 | 55.5 / 141.3 | 56 / 141 | 37.5 |
| realplus-v3 (500 screens) | words | 5,005 | 15.21 (13/34) | 1.00 | 8.94 | 1.04 | 3.23 | 1.00 | 53.7 | 10.87 / 10.39 / 1.04 | 70.4 / 148.7 | 485 / 2175 | 46.3 |
| realplus-v3 (500 screens) | letters | 10,905 | 15.43 (13/34) | 1.00 | 8.63 | 1.08 | 3.73 | 0.99 | 54.6 | 10.58 / 9.98 / 1.06 | 70.4 / 148.7 | 1092 / 4548 | 46.7 |
| owner-pool-2 (500 screens) | first | 500 | 13.84 (10/35) | 1.00 | 8.26 | 1.02 | 2.57 | 1.00 | 47.9 | 9.84 / 9.42 / 1.03 | 59.2 / 152.5 | 59 / 152 | 40.5 |
| owner-pool-2 (500 screens) | words | 5,423 | 15.78 (13/34) | 1.00 | 9.55 | 1.04 | 3.19 | 1.00 | 55.9 | 11.65 / 11.13 / 1.04 | 70.4 / 148.7 | 744 / 2034 | 50.4 |
| owner-pool-2 (500 screens) | letters | 12,188 | 16.01 (14/34) | 1.00 | 9.30 | 1.08 | 3.64 | 0.99 | 56.9 | 11.45 / 10.83 / 1.06 | 74.1 / 148.7 | 1696 / 4621 | 50.2 |

Other measurements:
- Nearly every read is at a new offset: 11.2-14.0 nonsequential reads per lookup, over 11.5-14.8
  distinct pages.
- Absent candidates' index and group reads: 5.8-8.5 per lookup (5.96 / 7.93 / 8.54 with Whole word
  stepping, for the three sets).
- Host throughput is 16,800-20,700 lookups/s.
- The arena peak is 53,648 B of the 64 KiB in every set (host build).
- 98.9-100% of lookups show an entry.

The CPU is not the problem; the file reads are.

## 6. Speed-ups

Each change is measured against the baseline with the golden check: every lookup of every set
and mode must show the same entry and highlight. Each lands in its own pull request.

### 6.1 Key filter: 4 KiB Bloom blocks chosen by the cursor (PR "Key filter")

The design takes hoshidicts' Bloom filter and lays it out so a lookup needs one page.
- **Partitioning.** Keys are grouped by their first two code points and packed in prefix order
  into 4 KiB blocks of about 10 bits a key, k = 7.
- **Directory.** A directory in page 0 maps prefixes to blocks. Page 0 is read anyway (it holds the
  header), so with one dictionary the directory costs no extra read. With several configured files
  the 4-page cache can evict page 0 and the blocks between them. With 4 copies of the bundled file,
  audit-dict-lookup-speed-2 measured 6.1-6.5 header and 5.3-6.2 filter reads per lookup, instead
  of 4. Net reads still fall from 37.3-47.4 to 19.2-21.5, with identical results.
- **Use.** Every candidate at one cursor that keeps the cursor's first two characters, which is
  nearly all of them, is checked against one block. That block is read once, into the existing
  4 KiB page cache. A candidate whose bits are not all set is not a key, so its index page is
  never read.
- **Bypass.** One-character candidates (nearly always keys) and candidates with another prefix
  (した → する) search the index as before.
- **No false negatives.** The filter has none, so results cannot change. Candidates that are not
  keys now cost 0.08-0.22 reads per lookup, instead of 5.8-8.5. Those reads come from false
  positives and from the candidates that bypass the filter.

| set | mode | lookups | reads before → after | index-page reads | console ms p50 before → after (est.) | p95 | per screen p50 | per screen p95 |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| fixture (50 lines) | first | 50 | 14.28 → 8.48 | 7.88 → 1.08 | 62.9 → 48.0 | 156.2 → 70.4 | 63 → 48 | 156 → 70 |
| fixture (50 lines) | words | 629 | 13.51 → 8.59 | 6.96 → 1.13 | 66.7 → 48.4 | 122.6 → 77.8 | 962 → 714 | 1532 → 1213 |
| fixture (50 lines) | letters | 1,321 | 13.97 → 9.28 | 6.87 → 1.27 | 66.7 → 51.7 | 130.1 → 85.3 | 1985 → 1494 | 3036 → 2397 |
| realplus-v3 (500 screens) | first | 500 | 12.57 → 7.87 | 6.80 → 1.11 | 55.5 → 48.0 | 141.3 → 74.1 | 56 → 48 | 141 → 74 |
| realplus-v3 (500 screens) | words | 5,005 | 15.21 → 8.34 | 8.94 → 1.14 | 70.4 → 48.0 | 148.7 → 81.6 | 485 → 358 | 2175 → 1361 |
| realplus-v3 (500 screens) | letters | 10,905 | 15.43 → 8.94 | 8.63 → 1.23 | 70.4 → 51.7 | 148.7 → 85.3 | 1092 → 843 | 4548 → 3021 |
| owner-pool-2 (500 screens) | first | 500 | 13.84 → 7.71 | 8.26 → 1.10 | 59.2 → 48.0 | 152.5 → 70.4 | 59 → 48 | 152 → 70 |
| owner-pool-2 (500 screens) | words | 5,423 | 15.78 → 8.33 | 9.55 → 1.15 | 70.4 → 48.0 | 148.7 → 77.8 | 744 → 521 | 2034 → 1238 |
| owner-pool-2 (500 screens) | letters | 12,188 | 16.01 → 8.90 | 9.30 → 1.25 | 74.1 → 51.7 | 148.7 → 81.6 | 1696 → 1202 | 4621 → 3020 |

Raw numbers: [`docs/benchmarks/dict-key-filter-20261015.json`](../docs/benchmarks/dict-key-filter-20261015.json).

- **Reads and bytes.** The filter itself costs 0.91-1.00 reads per lookup. The worst lookup's reads
  fall from 38-69 to 25-34, and bytes per lookup from 43-57 KiB to 27-34 KiB.
- **The index.** Index-page reads fall from 6.8-9.6 to 1.08-1.27 per lookup. That is about the
  1.10-1.25 keys whose hash is present: the floor for any index.
- **Host CPU.** 34-39 µs at p50 instead of 36-50 µs; with Whole word stepping, 24,800-27,100
  lookups/s instead of 17,000-20,800.
- **Memory.** No RAM is added: the block shares the existing 16 KiB page cache inside the 48 KiB
  scratch. The `Workspace` grows by 24 B per configured dictionary, and the arena peak goes from
  53,648 B to 53,840 B (host build) of the 64 KiB arena. Nothing stays resident between lookups.
- **Disk.** The bundled file grows by 904 KiB (226 blocks, 661,469 keys), from 140,556,089 B to
  141,484,032 B.
- **Shell code.** +638 B in `core/local_dict.o`, +648 B in the Shell's text. On main (Vita build)
  it fits the Shell module's existing text pages, as
  measured on the base this merges onto (the PR body has the rows). Before #117 removed 6.6 KB of
  Shell text, on ba5f69a, it would have added a page.
- **Compatibility, both ways.**
  - Older readers read only the 64-byte header and show the same entries from a filtered file
    (main's reader on it: identical, same reads).
  - Files without a filter read exactly as before (this reader on the frozen file: identical,
    same reads; the first read is now a whole 4 KiB page 0 instead of its first 64 bytes).
  - The bundled file gets the filter in `tools/prepare_plaintext.py`. The step is deterministic,
    and its sha256 is pinned in `docs/plaintext-dictionaries.json`. The private repository and
    its frozen artifact are unchanged.

Alternatives considered for the filter's shape (simulated on the probe traces, §5's sets,
Whole word):
- **hoshidicts' layout** (one bit array, k = 7 over 1 MiB, stopping at the first clear bit): 14-17
  distinct 4 KiB pages per lookup (p50), so more reads than it saves.
- **One partition per first character:** 172 blocks, 0.36-0.83 false positives per lookup, because
  し alone has 19,235 keys.
- **First three characters:** about the same as two.
- **Also filtering one-character candidates:** +0.17-0.18 reads per lookup. Switching blocks
  when a candidate's prefix leaves the block's range: +0.02.
- **Bits per key:** 10 is the bundled filter, 226 blocks. It leaves 1.13-1.15 index reads per
  lookup (measured; candidates that are not keys 0.12-0.14). 8 bits would leave 1.22-1.28 index
  searches with 178 blocks, and 6 bits 1.44-1.60 with 139 blocks (the converter at those
  settings, measured by audit-dict-lookup-speed-2 and -2-b; a simulation of the packing rule
  gave 1.22-1.28 and 1.49-1.65).
- **Small dictionaries.** On JMdict subsets, the filter saves reads only from 4 hash buckets up
  (`FILTER_MIN_BUCKETS`). Each subset is the first N rows of JMdict Plain's term banks after a
  shuffle with a fixed seed (7), converted with and without the filter. Reads per lookup, Whole
  word, on the fixture lines, without → with the filter:

  | rows | keys | buckets | reads |
  |---:|---:|---:|---|
  | 100 | 176 | 2 | 2.82 → 3.08 |
  | 300 | 520 | 4 | 4.07 → 3.16 |
  | 600 | 1,034 | 8 | 6.60 → 3.24 |
  | 5,000 | 8,514 | 64 | 9.65 → 3.67 |
  | 80,000 | 125,740 | 1,024 | 12.24 → 6.79 |

  Below 4 buckets, the few index pages mostly stay in the page cache.

### 6.3 Posting hints: rank a key's entries without their payloads (format 3)

Ranking a key's postings read a 160-byte head of each payload: spelling, reading, and in format 3
the frequency word. Format 3 (#127) leaves each posting's last u16 to index hints, and this change
fills it:
- bit 0: the spelling is the key;
- bit 1: the spelling normalizes to the key;
- bit 2: the entry has a reading;
- bits 3-15: the 13-bit frequency key of the payload's frequency word.

Page-0 bytes 160..167 (`VJPH`, version 1) announce them, so a v3 file without hints reads as
before. `rank_posting` builds the score from the hint exactly as from the head. It still reads the
head in two cases:
- for a key in the preferred-readings table, or in #127's usual-spellings table;
- for katakana or full-width text whose entry's spelling normalizes to the key without being it,
  since only the head says whether it is spelled as the text is.

The bundled file gets the hints in `tools/prepare_plaintext.py`, deterministically, before the key
filter: 840,994 postings, 17 s on the host. `--frequency` writes them when converting. The file's
size is unchanged.

Bundled dictionary (format 3, with the key filter), the same lookups, every shown entry and
highlight identical (36,521 lookups):

| set | mode | lookups | reads before → after | payload-head reads | console ms p50 before → after (est.) | p95 | per screen p50 | per screen p95 |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| fixture (50 lines) | first | 50 | 8.42 → 6.22 | 3.28 → 1.08 | 51.7 → 40.5 | 70.4 → 66.7 | 52 → 40 | 70 → 67 |
| fixture (50 lines) | words | 629 | 8.62 → 5.51 | 3.55 → 0.46 | 48.0 → 40.5 | 81.6 → 59.2 | 714 → 553 | 1198 → 859 |
| fixture (50 lines) | letters | 1,321 | 9.37 → 5.57 | 4.09 → 0.34 | 51.7 → 40.5 | 85.3 → 59.2 | 1509 → 1128 | 2412 → 1778 |
| realplus-v3 (500 screens) | first | 500 | 7.87 → 5.59 | 2.72 → 0.48 | 48.0 → 40.5 | 74.1 → 51.7 | 48 → 40 | 74 → 52 |
| realplus-v3 (500 screens) | words | 5,005 | 8.38 → 5.35 | 3.26 → 0.26 | 48.0 → 40.5 | 81.6 → 48.0 | 358 → 288 | 1368 → 1058 |
| realplus-v3 (500 screens) | letters | 10,905 | 9.06 → 5.41 | 3.83 → 0.22 | 51.7 → 40.5 | 81.6 → 51.7 | 858 → 650 | 3043 → 2224 |
| owner-pool-2 (500 screens) | first | 500 | 7.69 → 5.49 | 2.56 → 0.41 | 48.0 → 40.5 | 70.4 → 48.0 | 48 → 40 | 70 → 48 |
| owner-pool-2 (500 screens) | words | 5,423 | 8.39 → 5.34 | 3.25 → 0.23 | 48.0 → 40.5 | 77.8 → 48.0 | 516 → 402 | 1249 → 948 |
| owner-pool-2 (500 screens) | letters | 12,188 | 9.01 → 5.43 | 3.74 → 0.20 | 51.7 → 40.5 | 81.6 → 51.7 | 1213 → 925 | 3050 → 2316 |

Raw numbers: [`docs/benchmarks/dict-posting-hints-20261015.json`](../docs/benchmarks/dict-posting-hints-20261015.json).

- **Heads.** Payload-head reads fall from 2.6-4.1 to 0.2-1.1 per lookup, as 3.5-10.5 postings per
  lookup are ranked from their hints. The worst lookup's reads fall from 23-38 to 20-22, and bytes
  per lookup from 27-34 KiB to 18-21 KiB.
- **Host CPU.** 28-33 µs at p50 instead of 35-40 µs.
- **Memory and code.** No RAM is added. The Workspace stays 45,872 B (ARMv7) and 45,968 B (host):
  the hint is passed from the posting read to the ranking, not stored, and the file's hint flag
  sits in a `Dictionary`'s padding. The Shell's text grows inside its pages (Vita build; the PR
  body has the rows).
- **Remaining reads.** A Whole word lookup now reads, on average:
  - the header page and the filter block, about 1 each;
  - index pages, about 1.1;
  - the key group, about 1;
  - payload heads, 0.2-0.5;
  - the shown payload, 1.

### 6.2 What else was considered, by gain per KB of memory

| change | RAM (SceShell) | disk | reads saved per lookup | status |
|---|---:|---:|---:|---|
| key filter (6.1) | 0 (existing page cache) | +904 KiB | 4.7-7.5 (measured) | this work |
| posting rank hints (6.3): spelling = key, normalized spelling = key, has a reading and the 13-bit frequency key, in the v3 postings' u16 | 0 | 0 | 2.2-3.8 (measured: heads 2.6-4.1 → 0.2-1.1) | this work |
| keep the file open across one overlay session's lookups | 0 (a descriptor; overlay-shell-slim: fine) | 0 | 1 header read and the open/seek/close: the fixed part, est. about 11 ms for a cursor lookup | needs a console test: on the host nothing can measure `sceIoOpen` |
| bigger page cache kept across one text's lookups | 16-128 KiB resident in SceShell | 0 | 0-2.8% (audit-overlay-perf-1: 4 pages 0.0%, 8 pages 1.5%, 32 pages 2.8%) | rejected: AGENTS.md rule 1, and too small |
| block index / sorted prefix table instead of the hash | 0 | about 0 | ≤ 0.1 after the filter (index reads are at the floor) | not worth a format change |
| batching candidates by sorted key | 0 | 0 | ≤ 0.1 after the filter (the absent ones are no longer searched) | not needed |
| a resident filter (904 KiB) in the Shell | 904 KiB | 0 | 0.9 (the filter read) | rejected: AGENTS.md rule 1 (overlay-shell-slim) |
| hoshidicts' layout (one 1 MiB bit array, k = 7) | 0 | +1 MiB | negative: 14-17 filter pages a lookup at p50 | rejected (§1) |

After the filter, a lookup reads: header 1, filter 0.9, index 1.1, key group 1.0, payload heads
2.6-4.0, payload 1. The heads are the next target. Keeping the file open would then remove the
header read and the fixed open cost.
