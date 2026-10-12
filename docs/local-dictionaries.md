# Local dictionaries on PS Vita

Local lookup reads dictionaries from the Vita's storage. It needs no running
computer, dictionary API key, Hachidori Relay, or dictionary network request.
The release defaults to [on-console vita-vn-ocr](vita-vn-ocr.md) and ships plain
JMdict already converted and enabled. Recognition and lookup are offline.
Optional [Anki mining](anki.md) is off unless `anki_mining = on`; its cards
wait in an offline queue on the memory card until you send them. The
instructions below add your own dictionaries.

## Convert and install

1. Download a **Yomitan format 3 term dictionary ZIP**, such as a Jitendex
   Yomitan export. Leave the ZIP compressed. On your computer, use Python 3.9+
   (no pip packages required):

   ```sh
   python3 tools/convert_dictionary.py Jitendex.zip -o main.vjdict
   ```

   On Windows, use `python` if that is the installed Python command. HoshiDicts
   database/WASM files are not the input format; use the original Yomitan ZIPs.

2. Close any running game. Copy `main.vjdict` with VitaShell USB/FTP to:

   ```text
   ux0:data/VitaJPOverlay/dictionaries/main.vjdict
   ```

   Create the `dictionaries` folder if necessary. Finish copying before enabling
   it. A ZIP is not installable on the Vita until converted.

3. In `ux0:data/VitaJPOverlay/config.ini`, set:

   ```ini
   dictionary = local
   local_dictionary_dir = ux0:data/VitaJPOverlay/dictionaries
   local_dictionaries = main.vjdict
   ```

   Return to your game and open the overlay. No reboot is required for dictionary
   changes. The updated plugin binaries themselves require the usual reboot.

Alternatively, after installing the plugin and opening the overlay once to
create `config.ini`, start VitaShell's FTP server and run:

```sh
python3 tools/install_dictionary.py 192.168.1.30 main.vjdict --enable
```

Replace the IP with your Vita's. The port defaults to 1337; `--port` changes it.
The installer uploads under a new versioned filename, first as `.part`, and
renames it only after the upload succeeds. `--enable` backs up your configuration
and selects the uploaded dictionaries, preserving other settings.
It refuses to overwrite a configuration it cannot read. If the connection drops
during configuration replacement, restore a `config.ini.before-local-*` backup
as `config.ini` in VitaShell. Keep games closed while
installing. Without `--enable`, it only uploads and prints the configuration.
Older dictionary versions remain available; remove unused files in VitaShell
when you no longer need them. Remove a name from `local_dictionaries` to disable
that dictionary before deleting its file.

### Your own bilingual Daijirin archive

The bilingual v3 archive uses the same format 3 term banks as v2. Convert your
own copy on the computer; the Vita reads the resulting file directly:

```sh
python3 tools/convert_dictionary.py daijirin4-gpt-6-luna-en-jp-v3.zip \
  -o daijirin.vjdict --max-definition-bytes 7000
```

Enable `daijirin.vjdict` alone to prefer its bilingual definitions, or put it
before `jmdict.vjdict` in `local_dictionaries` to retain plain JMdict as a
fallback. Use the actual installed filenames. This changes dictionary lookup
without changing the OCR engine. The local overlay performs one lookup at the
selected character and displays one entry at a time; set **Dictionaries shown**
to 2 to page to JMdict's entry with Down (see below).

Nested English and Japanese text becomes plain text. Ruby annotation text,
styles and link destinations are omitted; the base characters and visible link
text remain. Images use labels when supplied. Unusually long definitions receive
the explicit truncation notice at the chosen limit.

The archive attributes the dictionary to Sanseido and supplies no redistribution
license in `index.json` or a separate license file. Keep your archive and converted
file private. The public plugin release continues to ship plain JMdict; it does
not include Daijirin data. The conversion and selected-position host measurements
are recorded in [the v3 report](benchmarks/daijirin-v3-selected-20261011.json).
The converted file has 334,741 entries; 53 definitions receive truncation notices
at 7,000 bytes. All payloads stay within 7,021 bytes. Twenty-five selected-position
cases with six repeats each use at most 53,584 arena bytes in a 64 KiB arena.
The report records ambiguous suffix matches separately; it does not establish
the intended reading, translation quality or physical Vita timing.

### Upgrading from the first local build

Install the updated plugin binaries and reboot, then reconvert your original
Yomitan ZIPs with the included converter. New conversions use the faster
version 2 index. Upload and select the new files as above. Existing version 1
files remain readable, including alongside version 2 files, but keep their
older index. `--format-version 1` is available when converting for an older
plugin; older plugins cannot read version 2 files.

## Several dictionaries

Up to eight `.vjdict` files can be enabled in priority order:

```ini
local_dictionaries = jitendex.vjdict,grammar.vjdict,names.vjdict
```

Filenames must contain only ASCII letters, digits, underscores, hyphens and
single dots, be at most 63 bytes, and end in `.vjdict`. No relative directory
paths or `..` are allowed in this list. The directory is configured separately.
You can also combine several ZIPs into one file, in priority order:

```sh
python3 tools/convert_dictionary.py Jitendex.zip Grammar.zip Names.zip -o main.vjdict
```

Each definition includes its source dictionary title. The converter preserves
separate senses/entries and indexes both spelling and reading without duplicating
the definition payload. Reading-only terms use their reading as the displayed
headword and are counted in the conversion report. Earlier dictionaries and
higher Yomitan term scores have priority for equal keys. Reconvert the original
ZIPs to update a combined file. The converter writes to a temporary file and
replaces the destination only after success, so a failed conversion preserves
the previous dictionary.

### Dictionaries shown

By default the overlay shows only the best match at the selected character.
Settings → OCR & lookup → **Dictionaries shown** (or `local_dictionary_results`
in `config.ini`, 1–8, default 1) shows the other enabled dictionaries' entries
for the same word, one page at a time:

```ini
local_dictionaries = jitendex.vjdict,jmdict.vjdict,names.vjdict
local_dictionary_results = 3
```

Page 1 is unchanged: the longest match, then an exact match, then dictionary
order. Down shows the next enabled dictionary, in `local_dictionaries` order,
that has an entry at the selected character; Up goes back. The body reads
`▲▼ Dictionary 2 of 3` while paging is available; dictionaries with no entry
are skipped. Each page highlights its own match, which can be shorter than
page 1's, for example `猫` in one dictionary and `猫舌` in another; in Whole word
mode, Right skips the highlighted match. Every new character, from the arrows
or a tap, starts at page 1. Changing the count looks the word up again without capturing
or recognizing the screen again. Separate files are paged; a combined file
built from several ZIPs is one dictionary.

Paging was chosen over a stacked view: one definition fills the existing body
and scrolls with the stick, and only it is held in memory. A stacked view
would keep several definitions resident and make each entry shorter.

Only the entry on screen is in memory. When looking up a word, the worker
records where each other dictionary's best entry is on disk: 24 bytes per
dictionary, a 208-byte table at the start of the single result arena. Up/Down
releases the shown entry, then opens only that page's dictionary and reads its
header and one payload into the same arena. It needs no page cache,
deinflection scratch or allocation, and is checked against the file's size
when located; a replaced file reports an error and is located again next time.
With 1, lookups perform exactly the same reads and arena use as before; no
table is allocated.

## Memory and lookup behavior

The plugin does **not** unzip banks, build an in-memory map, load a whole index,
or memory-map dictionaries. A term hashes directly to a 4 KiB index bucket on
storage; a hit verifies the full key before reading its definitions. Four shared
4 KiB page buffers and a small absent-hash cache reduce repeated reads. These
caches are shared across all enabled files and released after each request.
Dictionaries are opened for a lookup and closed on both success and error;
no dictionary-sized persistent allocation is added.

| Resource | Bound |
| --- | --- |
| Selected-entry output budget, including metadata | 8,192 + 256 bytes |
| Other dictionaries' page locators (Dictionaries shown 2–8) | 208 bytes in the result arena; none for 1 |
| Lookup scratch, including page caches and deinflection candidates | At most 48 KiB; compile-time checked |
| Local worker result arena | One 64 KiB arena; none for OCR-only jobs |
| Network worker result arenas | Two 256 KiB arenas, plus connection pool |
| Persistent recognized text / UI character copy | 4 KiB each |
| Enabled dictionary files | 8 |
| Input text | Up to 4,095 UTF-8 bytes plus NUL |
| Longest candidate term at the cursor | 32 Unicode code points |
| Deinflection candidates per substring | 128 |
| Selected entry | Zero or one resident; first posting of the best match per dictionary |
| Same-key index rows examined per candidate/file | First 64 |

Local lookup starts only at the selected Unicode character. Left/right moves
one character, including supplementary characters; it reuses saved OCR text
without capture, inference or whole-sentence tokenization. Background local
OCR and subtitles do not open dictionaries or allocate a result arena.
The prior entry is detached before the single arena is reset, and rapid moves
publish only the latest requested position. Closing, changing regions or
switching games cannot publish an old lookup result.

Unused selected-output capacity and scratch are released after lookup. The
1.5 MiB Paf UI reserve remains unchanged. A refused result allocation preserves
the recognized header and reports a Shell UI memory error. These bounds describe
lookup allocations, not total process memory or guaranteed available memory.
The [selected-position measurements](benchmarks/local-character-memory-20261011.json)
used at most 54,000 arena bytes over nine illustrative full-JMdict positions;
generated maximum-entry/full-text tests used 53,584 bytes with static text and
61,776 with two full text copies in the 64 KiB arena.

The [dictionary page measurements](benchmarks/dictionary-pages-20261012.json)
compare the selected lookup before and after paging on the host, with JMdict
alone and with eight enabled files (Jitendex, JMdict, Pixiv Light and a second
Jitendex, each twice). With one result, read counts and arena peaks are
identical to the previous release and times are within noise. With eight
pages, locating the others adds 14–30 reads and 208 arena bytes to the first
lookup; a later page is two reads (its header and payload) in one file,
about 6.5 µs on the host, using at most 9,072 arena bytes. These are host
measurements with a warm OS file cache: SD-card reads on the Vita are slower,
and an assumed 0.2 ms per read would add about 3–6 ms to the first lookup and
0.4 ms per page.

The [Daijirin import measurement](benchmarks/daijirin-import-20261010.json)
records conversion of the bilingual v2 archive: 334,741 entries, two reading-only
headwords preserved, ten unindexable rows skipped, and 53 unusually long
definitions clipped to the explicit 7,000-byte conversion limit. All 110 lookup
samples and 30 combined JPEG/recorded-Lens samples passed on the host. These
results establish the core bounds; physical Vita UI and network memory still
require manual testing.

Longer phrases take precedence. Katakana and hiragana readings share a search
key; full-width ASCII is folded to ASCII. Yomichan's Japanese suffix rules handle
inflections such as `食べました`, `行った`, `高くなかった`, and `来ました`.
The matched original text length is retained in the entry, while the UI
highlights the complete match by default. Settings → OCR & lookup → Highlight
can instead highlight the selected character. Tapping the recognized header
selects a character and uses the same bounded lookup as the arrows. Neither
option introduces sentence tokenization. With `anki_host` set, × sends the
match at the cursor to [Anki](anki.md).
Part-of-speech rules from the dictionary filter deinflected candidates.
At the selected position, the longest exact term takes precedence, followed
by deinflected candidates and configured dictionary order. This is bounded
matching, not a full Japanese morphological parser.
The caps above can omit lower-priority matches or unusually complex inflections.
Missing or corrupt enabled files fail the lookup with an actionable error;
there is no automatic relay/cloud fallback.

A selected-entry allocation or file error is reported explicitly. Lookup does
not grow the worker arena or silently display a partial definition. The older
whole-sentence host API remains available for benchmark compatibility; its
64 KiB output/128-token limits are not the local overlay cursor path.

## Supported content and tradeoffs

- Term dictionaries only. Frequency-only, pitch-accent-only and kanji-only ZIPs
  are rejected with an explanation. Frequency ranks/pitch data are not imported.
- Definitions are plain text, decided once on the computer: HTML glossaries
  and Yomitan structured content are flattened when converting, and the Vita
  displays the stored text without parsing markup or applying CSS. Block
  boundaries are retained; Jitendex-style sense markers (`①`) and inline labels
  (`noun kana`, `See also 猫車`) keep their separators. Images use alt/title text
  or an omitted-image label; ruby readings, styles, scripts, media and link
  targets are omitted. Existing `.vjdict` files already store plain text and
  keep working; reconvert to get the separators and ruby removal in HTML
  glossaries. JMdict from `tools/prepare_jmdict.py` converts byte-identically.
- The converter caps each definition at 4,096 UTF-8 bytes by default, appends
  `[definition truncated]`, and reports the count. Change this with
  `--max-definition-bytes 2048` (allowed range 256–7,000) to trade detail for
  more results per screen.
  For structured bilingual dictionaries, `--max-definition-bytes 7000` retains
  more English and Japanese text while keeping the existing 8 KiB entry limit.
  Definitions longer than that still receive the truncation notice; the converter
  does not silently increase the console's lookup memory budget.
- Headwords/readings beyond the supported term length, and rows with neither
  spelling nor reading, are skipped and counted.
  Half-width katakana, arbitrary Unicode normalization, and custom HoshiDicts
  database files are not supported by this local format.
- Source ZIPs are read on the computer one JSON row at a time, with an 8 MiB
  per-row safety limit. SQLite sorts the index on disk with a 4 MiB page cache.
  Allow temporary disk space for the payload, sorting database and final output;
  `--temp-dir` chooses the scratch disk. Installation size is printed at the end.
- Storage latency replaces RAM usage: slow SD cards and many dictionaries can
  make lookups slower. Host measurements do not establish Vita hardware latency.

## Verification and development

```sh
cmake -S . -B build/host -G Ninja
cmake --build build/host
ctest --test-dir build/host --output-on-failure
./build/host/vjo-cli --dict local --local-dir . --local-dicts main.vjdict \
  --text '猫は食べました。' --nav --stats
python3 tools/benchmark_local_dictionary.py --entries 1000000
```

CTest runs conversion, real disk lookup, deinflection, UTF-16 highlights,
structured/HTML glossaries, dictionary priority, format corruption, interrupted
configuration replacement, and memory-size regression checks. Worker tests cover
local lookup without API keys and changing dictionaries during an in-flight job.
The generated deinflection table is checked against its pinned source.

### Host benchmark (2026-10-08)

With Jitendex (436,549 entries), the new index cuts two sentence workloads
from 3,005 / 3,267 file reads to 116 / 106. Host lookup times improve 15–17×,
with identical definitions and highlights. Lookup arena peaks stay below
108 KiB, inside the existing 112 KiB cap. The converted file grows from
88.5 MiB to 98.3 MiB (about 11%). See the [benchmark report](lookup-performance.md)
for all queries, raw measurements, methodology and reproduction commands.

The benchmark driver times the lookup pipeline without process startup, counts
file reads, and checks result equality. Application caches start empty for
each request; the host OS file cache is warm. These are **not physical Vita or
SD-card measurements**. The synthetic size/memory benchmark above uses the
same driver, and regression tests check that arena use does not grow with
dictionary size.

### Binary format, version 2 (default)

All integers are little-endian unless explicitly stated. No C structs are cast
onto disk data. The 64-byte header contains magic `VJDICT2\0`; four u32 values
(version=2, header size=64, total posting count, hash-record stride=24); then
five u64 values (index offset=4096, data offset, exact file size, primary bucket
count, total index page count). The header is padded to 4 KiB. Bucket count is
a power of two; the data region follows all index pages.

Keys use FNV-1a-64 followed by the avalanche finalizer implemented in
`key_hash()` in the converter and runtime. The low bits select a primary
bucket. Each 4 KiB index page has a 16-byte header: magic `VJHP`, u32 record
count (at most 170), and u64 next-page offset (zero for none). Overflow pages
follow all primary pages, have increasing offsets, and are limited to a
64-page chain. Records are sorted by unsigned hash within each bucket.

Each 24-byte record contains u64 hash, u64 key-group offset, u32 posting count,
u16 key length and u16 union of deinflection rule bits. A group holds the raw
UTF-8 key followed by 16-byte postings: u64 payload offset, u32 payload length,
u16 rule bits and u16 reserved=0. Postings retain dictionary priority,
descending term score and original row order. Hash collisions are resolved
by comparing the full key. All offsets are absolute.

Payloads are three UTF-8 NUL-terminated strings: spelling, reading, definition.
Spelling and reading indexes share payloads. Rule bits are v1=1, v5=2, vs=4,
vk=8, vz=16, adj-i=32, iru=64. Opening a dictionary reads only its header;
pages, offsets, bounds and payloads are validated as they are accessed.

### Binary format, version 1 (legacy, still readable)

All integers are little-endian. No C structs are cast onto disk data. The header
is 64 bytes: magic `VJDICT1\0`; four u32 values (version=1, header size=64,
record count, record stride=24); then five u64 values (index offset=64,
data offset, exact file size, reserved=0, reserved=0).

Each index record is 24 bytes: u64 key offset, u64 entry offset, u16 key byte
length, u16 deinflection rule bits, u32 entry byte length. Offsets are absolute.
Keys are sorted by unsigned UTF-8 bytes, then dictionary priority, descending
term score and original row order. The data region holds raw keys and payloads.
A payload is three UTF-8 NUL-terminated strings: spelling, reading, definition.
Rule bits are v1=1, v5=2, vs=4, vk=8, vz=16, adj-i=32, iru=64.

The runtime validates header layout, exact file length, offsets, record sizes
and string termination before use. Validation is incremental so opening a large
file never scans the whole dictionary. It is not a cryptographic integrity check.
