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

Each change is measured against the baseline with the golden check: the same entry and highlight
for every lookup of every set. Each lands in its own pull request, and the results are added here.

1. **A key filter with locality** (next PR): Bloom blocks of 4 KiB partitioned by the key's first
   two code points. It is read through the existing page cache, so it adds no RAM, and it is
   appended to the file, so readers without it still work. A simulation on the traces gives 0.9
   filter reads plus 1.1 index reads per lookup, replacing 7-9.6.
2. **Posting rank hints** in the v3 postings, which dict-freq's format leaves to this work:
   bits 0-2 hold spelling = key, normalized spelling = key, and has-a-reading. Ranking then needs
   no payload heads, saving 2.6-4.0 reads.
3. **Keeping the file open across lookups**, an option overlay-shell-slim agrees to: it would save
   the per-lookup open, seek and close, at most the fixed part. That is about 11 ms for a cursor
   lookup, by the fit. This needs a console test before any claim.
