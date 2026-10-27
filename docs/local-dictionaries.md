# Local dictionaries on PS Vita

Local lookup reads dictionaries from the Vita's storage. It needs no running
computer or dictionary network request; the local dictionaries are the only
ones. The online dictionaries jpdb.io and jiten.moe and the Hachidori relay on
a computer have been removed; Jitendex and every other Yomitan dictionary are
converted and looked up locally as below, and a saved `dictionary = jpdb`,
`jiten` or `hachidori` loads as `local` (the guide's
[Settings and dictionaries](guide.md#settings-and-dictionaries)).
The release defaults to [on-console vita-vn-ocr](vita-vn-ocr.md) and ships plain
JMdict already converted and enabled. Recognition and lookup are offline.
Optional [Anki mining](anki.md) is off unless `anki_mining = on`; its cards
wait in an offline queue on the memory card until you send them. The
instructions below add your own dictionaries.

## Frozen release bundle

Upcoming releases include `plaintext.vjdict`, **Bee's Ultimate Dictionary**:
word definitions from Daijirin4 Plain, Sankoku8 Plain, then JMdict Plain (the
frozen 2026-10-13 downloads), an English summary for each grammar point, a line
for each kanji of a word with its most common meanings, and every entry's Jiten
global frequency rank. It is built once, from frozen snapshots, in the private
`bee-san/plaintext-dicts` repository (the release `ultimate-v1`, commit `e9b6e76`)
and not updated;
public releases include the processed dictionary. The release build checks an
exact source commit and archive/output SHA-256 against
`plaintext-dictionaries.json`.

The bundle removes lower-priority rows sharing a spelling and normalized reading
pair. It keeps all senses from the winning source and distinct spellings/readings;
an empty reading uses its spelling, and the inflection rules stay. No entry
names its source: the definitions carry no `[Daijirin4 Plain]`-style first line
and the attribution is in `plaintext-attribution.txt` and the metadata. The
file names itself instead, `Bee's Ultimate Dictionary` in page 0
([format](#binary-format-version-3-with-frequency-ranks)), and that is the
dictionary's name on Anki cards, in `log.txt` and on the Rubi app's
Dictionaries shown card. A grammar point's summary replaces a function word's
definition (は, に, ので, てしまう) and comes before a content word's on a line of
its own (上, こと, 無くなる); a point no word entry had is an entry of its own, and
its other spellings are lookup keys of it (でしまう finds てしまう). There are
486,529 entries, 1,332 of them grammar points no word entry had. Eleven
definitions are clipped at 7,000 UTF-8 bytes and end in `…`; one empty row and
15 overlong rows are skipped. These are fixed format bounds, not runtime RAM
increases. No dictionary update or conversion occurs during release builds.

Rebuild the frozen artifact with `python3 tools/build.py` in the private repo.
Its `sources.json` verifies all three original ZIPs before processing. Prepare
an existing artifact with `tools/prepare_plaintext.py`; the Vita reads only the
resulting `.vjdict`. This combined file counts as one dictionary for paging.
Preparing checks the frozen file's SHA-256, adds the posting hints (only when
`key_filter` has `hinted_postings`: the frozen file predates them; with
`table_postings` too, version 2's, the lookup tables' bits included) and the
[key filter](#key-filter), and checks the result against its own pin
(`key_filter` in `plaintext-dictionaries.json`). A frozen file that
`--frequency` already gave its hints must be pinned without `hinted_postings`:
adding hints to it again is refused. That file's top-level `bytes` and `sha256`
describe the frozen file in the private repository, and `key_filter` the file
the release ships. A package manifest's `dictionary` block copies both: the
shipped file's checksum is also under `files`. That step is deterministic: it indexes what the
file already holds and changes no entry.

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

Alternatively, once `config.ini` exists (the overlay's installer, `install-vita.sh`, creates it, and so
does any change in the Rubi app; the Shell plugin never writes it), start VitaShell's FTP server and
run:

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
file private. The earlier public releases shipped plain JMdict. Upcoming releases include
the separate frozen plaintext bundle described above; this bilingual archive
is not the source of that bundle. The conversion and selected-position host measurements
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

### Frequency ranks for your own dictionaries

Add a rank-based Yomitan frequency dictionary (`"frequencyMode": "rank-based"`
in its `index.json`, such as Jiten's from https://jiten.moe/frequency-dictionaries)
with `--frequency` to give every entry its rank, written as format 3
([frequency ranks](#frequency-ranks)):

```sh
python3 tools/convert_dictionary.py Jitendex.zip -o main.vjdict --frequency jiten_freq_global.zip
```

The report's `frequency` part counts the ranked entries and how they matched.
Format 3 needs this build or later; earlier plugins refuse it with a format
error, and this build still reads formats 1 and 2 (without ranks).

`--table-hints` (with `--frequency`) also marks the entries of the lookup's
two short tables of common readings and usual spellings, so their words (私,
中, こう …) are ranked without reading each entry
([posting hints](#frequency-ranks), version 2). Rubi 0.9.8 and older refuse such
a file ("Invalid or incomplete local dictionary"), which is why it is not the
default; leave it out for a file an older Rubi reads too. The bundled dictionary
has them.

## The popup

The popup shows the selected entry: the headword and reading at the Japanese
font size, the frequency badge, then the entry's text at the English size,
exactly as the dictionary wrote it. Bee's Ultimate Dictionary ends an entry's
text with its kanji lines, `日=day, counter for days, Japan, sun`, one per
distinct kanji of the spelling, in spelling order (none for a word written in
kana), separated from the definition by one newline. The popup puts a blank line
before them and shows them as written, each on its own line, with the kanji at the
headword's size in the text colour:

```
日本語 (にほんご) #4372
The language used by the Japanese people since ancient times …

日=day, counter for days, Japan, sun
本=real, true, origin, book
語=word, language
```

The kanji lines are the text's last lines that each start with a kanji, then
`=` and its meanings (`vjo_kanji_block`, core/entries.c; whole code points: the
bundle has lines for CJK compatibility ideographs and for characters beyond the
BMP). No other line of the bundle's texts has that shape, and any other
dictionary's text is shown as before.

The body holds 4,096 UTF-16 units and 96 styled spans; a kanji line takes two.
An entry longer than the room left (after the status and Anki
lines before it, and with room kept for the config warnings after it, at their
longest) is clipped at the end of its definition, at a whole character, and marked `…`: the
kanji lines stay whole, and so does a grammar summary, which the bundle puts first. A long entry
scrolls with the right stick.
`every_bundled_entry_fits_the_popup` (host/tests/test_word_lookup.cpp) renders
every entry of the bundle with the body's longest lines before it (the dictionary page, the
Rubi app's notice, a status, the Anki lines) and four warnings after it: none is truncated or
loses a kanji line; 46 are clipped with nothing else in the body, 103 in that worst case, and that
fullest body takes at most 66 of the 96 spans. The layout itself (line wrapping at the
default font sizes, the scroll) is laid out by the console's text widget and
has not been checked on a console.

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

Each definition this converter writes starts with its source dictionary's title
(`[JMdict Plain]`), which names it on Anki cards and in `log.txt`. It preserves
separate senses/entries and indexes both spelling and reading without duplicating
the definition payload. Reading-only terms use their reading as the displayed
headword and are counted in the conversion report. Earlier dictionaries and
higher Yomitan term scores have priority for equal keys. Add `--deduplicate` to keep each spelling/normalized-reading pair from its
first source while preserving all of that source's senses. Without this flag,
all source rows are retained. Reconvert the original ZIPs to rebuild a combined file. The converter writes to a temporary file and
replaces the destination only after success, so a failed conversion preserves
the previous dictionary.

### Dictionaries shown

The overlay shows the best match at the selected character first.
Rubi app → OCR & lookup → **Dictionaries shown** (or `local_dictionary_results`
in `config.ini`, 1–8, default 2) shows the other enabled dictionaries' entries
for the same word below it, in the same body; 1 shows the best match only. The default
of 2 shows a word, then the bundled names' entry for it (渚: the word, then Nagisa); an update
that enables the names turns a count of 1, the default before them, into 2:

```ini
local_dictionaries = jitendex.vjdict,jmdict.vjdict,names.vjdict
local_dictionary_results = 3
```

The best match is unchanged: the longest match, then an exact match, then dictionary
order. Below it, after a blank line, each other enabled dictionary that has an entry
for the same word follows, in `local_dictionaries` order (its definition starts with
its own `[title]`); dictionaries with no entry are left out. The right stick scrolls
the body down to them, as it scrolls a long definition. Every stacked entry is the
same word: a dictionary counts only with an entry for the best match's whole span, so
with `猫舌` shown, a dictionary that has only `猫` adds nothing, and the lookup searches
no shorter span. So a name that is the whole word (渚) is shown, and a name that is only
its start (空 in 空港) is not. A best match too long for the body is clipped first
(`…`), so the entries below it keep their room. Changing the count looks the word up
again without capturing or recognizing the screen again. Separate files are stacked; a
combined file built from several ZIPs is one dictionary.

Until 0.9.6 the other dictionaries were pages, one at a time, with an L or R tap
(`L/R Dictionary 2 of 3`). Stacking reads them with the word: when looking it up, the
worker records where each other dictionary's best entry is on disk (24 bytes per
dictionary, a 208-byte table at the start of the single result arena), then reads each
one's payload into the same 64 KiB arena after the best match's, through the open
session's handles. No allocation is added: an entry that does not fit what is left of
the arena (eight maximum-size definitions do not) is left out with those after it, and
the best match stays. With 1, no locator table is allocated and nothing else is read.

## Memory and lookup behavior

The plugin does **not** unzip banks, build an in-memory map, load a whole index,
or memory-map dictionaries. A term hashes directly to a 4 KiB index bucket on
storage; a hit verifies the full key before reading its definitions. A
[key filter](#key-filter) on storage answers most deinflection candidates that
are not words with no index read. Four shared 4 KiB page buffers and a small
absent-hash cache reduce repeated reads; the filter's one block per lookup is
read into the same buffers. These caches are shared across all enabled files
and released after each request.
When a cached page already contains the entire selected payload, showing it
copies those bytes into the output instead of reading them again. Entries that
cross pages keep the direct payload read. Candidate scratch starts at eight
records and grows up to the same 128-record bound; failed growth reports OOM
without returning a shortened candidate set. The allocation stays at 64 KiB.
The [follow-up benchmark](../reports/dictionary-cache-experiments.md) records the
read savings and the smaller scratch high-water mark.
The Shell reuses open files and parsed headers between selections. The session also
keeps the configured files' filter directories in one 1,024-byte pool, in
`local_dictionaries` order, each copied from page 0 as the file opens: every one that
fits in what the earlier ones left (256 blocks in all; the bundled files' 226 + 10
fit). A lookup then reads no page 0, whichever file it searches; a file whose
directory does not fit reads its page 0 through the page cache at each lookup, as
every file did before (when the session kept one directory, the two bundled files
evicted each other's: 1.88 page-0 reads a lookup). The session keeps 16
word-location records too, including misses. Together these occupy 6,784 bytes
inside the existing 64 KiB allocation [measured Vita build;
[lookup report](../reports/dictionary-lookahead.md); the pool took the single
directory's bytes and its bookkeeping the padding of each file's record, so the
size did not change]. Definitions and the four page
buffers are not retained in this cache. Auto keeps the session with its recognized
text, including while preparing words before the overlay opens. Closing the overlay
ends it after any in-flight lookup. Freeing results, a new full OCR job, a changed
dictionary directory/list or platform, or a dictionary I/O/format error also releases it.

Auto with Whole word prepares up to eight positions starting at the current word, one per idle job
after 100 ms without foreground work [source: `shell/worker.c`]. It uses the normal
matching and navigation rules, keeps the displayed definition intact and yields
between words. A prepared hit reads only its definition; a prepared miss reads
nothing. User lookups, refreshes, subtitles, Anki screenshots and self-tests take
priority. If scratch will not fit beside the displayed entry, preparation stops
and selections use the normal lookup. Text generations and result limits scope
the cache; a changed text or dictionary cannot reuse another's locators.
As the installation instructions require, finish replacing dictionaries with games
closed; files must not be modified during an open session. A new lookup after the
close opens and validates fresh files. The stateless host/core API is unchanged when
`VjoPlatform.local_session` is NULL. Core callers enable word-location reuse with
a nonzero `local_text_generation` and must change it whenever their text changes;
zero leaves locator caching disabled.

| Resource | Bound |
| --- | --- |
| Selected-entry output budget, including metadata | 8,192 + 256 bytes |
| Other dictionaries' page locators (Dictionaries shown 2–8) | 208 bytes in the result arena; none for 1 |
| Lookup scratch, including page caches and deinflection candidates | At most 48 KiB; compile-time checked |
| Local worker result arena | One 64 KiB arena; none for OCR-only jobs |
| Session, filter directories and word locators | 6,784 bytes inside that 64 KiB allocation on Vita [measured]; at most 7 KiB [compile-time bound] |
| Network worker result arenas | none since Google Lens and the Hachidori relay were removed (they were two 256 KiB arenas, plus a connection pool) |
| Persistent recognized text / UI character copy | 4 KiB each |
| Enabled dictionary files | 8 |
| Input text | Up to 4,095 UTF-8 bytes plus NUL |
| Longest candidate term at the cursor | 32 Unicode code points |
| Deinflection candidates per substring | 128 |
| Selected entry | Zero or one resident; the best-ranked posting of the best match per dictionary |
| Same-key index rows examined per candidate/file | First 64, each ranked by a 160-byte head read |

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
samples and 30 combined JPEG/recorded-Lens samples (Google Lens has since been removed) passed on the host. These
results establish the core bounds; physical Vita UI and network memory still
require manual testing.

Longer phrases take precedence. Katakana and hiragana readings share a search
key; full-width ASCII is folded to ASCII. Yomichan's Japanese suffix rules handle
inflections such as `食べました`, `行った`, `高くなかった`, and `来ました`, and
Yomitan's for the spoken forms the old rules lack: `しておく`, `変わらん`,
`しちゃ駄目`, `戻らなきゃ`, `認めざる`, `返さねば`, `しやがって`, `聞きやしねえ`,
`いたしまして` and `知らねー` (`third_party/yomitan/`; the bare `な` of the old
"imperative negative", which Yomitan no longer has, is gone: `失礼な` is 失礼, not
a verb `失礼` read しつらい, and `するな` highlights `する`). Which of Yomitan's
rules were taken, and why some were not, was measured on 5,000 lines of hooked
visual-novel text against Yomitan itself ([the report](../reports/yomitan-check.md)).
The matched original text length is retained in the entry, while the UI
highlights the complete match by default. Rubi app → OCR & lookup → Highlight
can instead highlight the selected character. Tapping the recognized header
selects a character and uses the same bounded lookup as the arrows. Neither
option introduces sentence tokenization. With card mining on, Enter saves
the match at the cursor as an [Anki](anki.md) card.
Part-of-speech rules from the dictionary filter deinflected candidates.
At the selected position the longest source span takes precedence. At that
span, an entry written the way the text is written wins over a homograph:
the text itself first, then its inflections with fewer steps first. Kana
text therefore shows the kana word (the particle から, the honorific さん,
the verb する) and inflected kana its dictionary form (した → する, なってる →
なる) rather than a homograph spelled in kanji (空, 三, 剃る, 下). Without
such an entry, the best-ranked homograph of the candidates with the fewest
inflection steps is shown (わかった: 分かる, not 分かつ: both one step from the text).

Only one entry is shown, and one key's entries are stored in source order,
which for the bundled Daijirin4 is reading order, not frequency (私 lists
あたい first, 先生 シーサン). The lookup therefore ranks a key's postings, reading
only each one's spelling and reading (one bounded head read): first a short
built-in list of common readings of very frequent words (私 → わたし, 僕 → ぼく,
こう → 斯う; `preferred_readings` in `tools/lookup_tables.txt`, every pair checked to
exist in the bundle), then the spelling as written, then an entry that has a
reading (Daijirin4's kanji-character articles have none), then, in a
dictionary with [frequency ranks](#frequency-ranks), the more frequent entry
(with a few usual spellings where the ranks mislead), then the longer article. With several dictionaries, at one span an entry written as the
text is (in any dictionary) beats a homograph in an earlier dictionary, an
exact hit in a later dictionary still beats an inflected hit in an earlier
one, and otherwise configured dictionary order decides. This is
bounded matching, not a full Japanese morphological parser: longest match
can still join a particle to the next word (頭がすっきり shows ガス for がす),
and a word spans a line break only where the text's lines join, a sentence the
game wrapped (not after a short or bracketed speaker's name line or a sentence's end:
[words split over two lines](touch-lookup.md#words-split-over-two-lines)). On the 134 recognized development lines whose VNs the
fixture's filter allows (`tools/collect_word_lookup_lines.py`), stepping with
Right, the ranking changed 375 of 1,376 shown entries (164 of 625 on the 50
committed lines), and no matched span. An independent audit judged a random
sample of 80 changes: 51 better, 25 the same, 4 worse. The remaining losses
were mostly single kanji whose longer article is a rarer reading (泉 せん, 比
ころ, 校 きょう) and same-depth inflection ties (飛ばされ → 飛ぶ), which the
frequency ranks now decide
([word lookup](touch-lookup.md#word-lookup-on-recognized-text)).
The caps above can omit lower-priority matches or unusually complex inflections.
Missing or corrupt enabled files fail the lookup with an actionable error;
there is no network or cloud fallback.

## Frequency ranks

The bundled dictionary (format 3) gives every entry a rank from the Jiten
global frequency list (jiten.moe, CC BY-SA 4.0; [attribution](plaintext-attribution.txt)):
#1 is the most frequent word, and an entry the list does not rank is
explicitly unranked (rank 0, `VJO_NO_RANK`). The popup shows the rank after
the reading (`猫 (ねこ) #1536`, or `unranked`); `frequency_badge = off` in
`config.ini` hides it. An Anki card records it (`rank`, `source` "Jiten";
[the record](anki.md)). The lookup log line ends with it.

Format 3 is format 2 with these additions, so it needs nothing in memory:

- page 0 bytes 64..127: the frequency list's name, NUL-padded (read with
  the 64-byte header, in the same read);
- every payload starts with a 32-bit little-endian frequency word: bits 0-23
  the rank, bit 24 set when a kanji spelling borrows its word's rank in kana
  (Jiten's `N㋕`), bits 25-31 for the converter's report. The ranking's head
  read and the shown entry's payload read include it: no extra read, 4 bytes
  more in each.
- optional **posting hints**: every posting's last u16 holds what the ranking
  reads from its payload's head (the spelling is the key, it normalizes to the
  key, the entry has a reading, and the frequency key below), so a key's
  postings are ranked without reading their heads. With version 1 of the hints
  the head is still read for a key with a preferred reading or a usual
  spelling; **version 2** (the table hints) also marks, in two spare bits of each
  posting's rule bits, the entry that a table names for its key, so those keys
  (私, 中, 頭, こう …: 2.7% of the taps of 5,000 real lines read 4 to 17 heads
  without them) are ranked without their heads too. In every file the head is
  read for katakana or full-width text whose entry's spelling only normalizes
  to the key. `--frequency` writes version 1, and `--table-hints` version 2;
  `tools/prepare_plaintext.py` adds version 2 to the bundled file
  (`convert_dictionary.add_index_hints`, deterministic). Files without them, and
  readers that predate them, rank as before; a reader that predates version 2
  (Rubi 0.9.8 and older) refuses a file that has it. On the bundled
  file the hints take a lookup's payload-head reads from 2.6-4.1 to 0.2-1.1,
  with every shown entry identical (`tools/dict_lookup_bench.py`,
  `reports/dict-lookup-speed.md`), and the table hints 0.39 to 0.10 a cursor
  lookup (both bundled dictionaries; 0.33 to 0.04 for the bundled file alone;
  `tools/dict_stage_report.py` on 64,579 taps, every one identical). The exact
  bits are in [version 3's format](#binary-format-version-3-with-frequency-ranks).

Ranking uses it as a tie-break only, after the flags above (a preferred
reading, the text as written, katakana as hiragana, a reading): first a
word's usual spelling where the ranks mislead (below), then an entry's
own rank before a borrowed one, then the lower rank, compared as 13 bits
(exact below rank 256, within 1/128 of an octave above it, where the longer
article decides). The same score chooses between dictionary forms at the
same number of inflection steps (かった: かう, かる and かつ are one step each;
the more frequent is shown, where the longer article was). Formats 1 and 2
have no ranks and rank as before. A frequency never overrides the longest
span, the text before its inflections, fewer inflection steps, or the
dictionary order the user set. Within those it comes where Yomitan puts its
frequency order: after the length of the inflection chain and the exact
match of the source text (yomidevs/yomitan@67db60d `translator.js`
2197-2207), as bee-san/hachidori#576 adopted after the #575 analysis.
Across dictionary files it does not: Yomitan's single sort dictionary ranks
every file's entries, but here each file carries its own list, whose ranks
do not compare with another list's, so the configured order decides.

Measured with the overlay's lookup itself (`tools/frequency_eval.py`, a tap
at each word form's first character through `vjo_overlay_lookup_at`), over
the word forms of Jiten's 2,162 visual-novel deck exports weighted by their
491,025,510 occurrences (CC BY-SA 4.0, not committed). Before is the format 2
file, without ranks, on which this lookup shows the previous build's entry for
every one of the 246,426 forms:

| | Before (no ranks) | With ranks |
| --- | ---: | ---: |
| Shown entry carries a rank (of the 99.96% of tokens with an entry) | 0% | 99.28% |
| Shown reading is the one Jiten's parser read | 95.81% | 97.17% |
| Same, where the whole word matched | 97.06% | 98.44% |
| Shown entry changed | | 2.77% of tokens: 1.40% to Jiten's reading, 0.04% away from it, 1.32% other |

The changes that most often move to Jiten's reading are kanji whose first or
longest article was a rarer reading: 良い よい (was いい), 奥 おく (おう), 指 ゆび
(さし), 達 たち (たつ), 瞳 ひとみ (め), 年 ねん (とし). Most "other" changes pick
the more frequent of several spellings of one reading: わたし 私 (was 渡し),
おまえ お前 (御前), せい 所為 (姓). Where the ranks alone pick a rarer word, a short
list of usual spellings decides, in files with ranks only (`usual_spellings` in
`tools/lookup_tables.txt`, every pair checked in the bundle): kana text whose word has
no entry spelled in kana ranks the kanji spellings by how often each is written
(いつか would show ５日, not 何時か; なくなる 亡くなる, not 無くなる), and Jiten ranks
kana text by its parser's lemma, so kana やめる is 辞める "to resign" at rank 135
where visual novels' やめて means 止める "to stop" (and おじさん 叔父さん "uncle" for
小父さん); Jiten also ranks 身体 しんたい (2,603) just above the からだ (2,805) the
text reads. The list orders one key's entries and nothing more: the text's own
entry still comes before an inflection's (なくして shows 無くして "without", not
無くす). Rank ties within 1/128 of an octave and readings both spellings share
stay on the article's length. Of the kana-written forms whose entry changed
(5,147,077 tokens), Jiten's kana-form ranks (`N㋕`) write the new entry's word
in kana more often for 38.23%, less often for 4.03% and as often for 57.74%.
The 40 largest changes of that 4.03% (`tools/frequency_eval.py --frequency`'s
`kana_text.most_frequent_worse`: 141,795 tokens, 68% of it, each under 10,000
tokens) show a word spelled wholly in katakana in 13 forms, 38% of their tokens
(54,574): plants and animals, some of them also given names (あやめ アヤメ,
つぐみ ツグミ, くるみ クルミ), and words often written so (いたずら イタズラ,
こたつ コタツ). Read one by one, the other 27 are the same word with a katakana
stem (ハマる, ダルい), a word the ㋕ ranks misjudge (ねる 寝る, かえる 帰る, おこる
怒る), another word of an ambiguous reading (しゅう 週, れい 礼), or a real loss
(ふた 二, にい ２位, こがね 小金, りゅう 流). Coverage of the dictionary itself, and how an entry's rank is
matched to Jiten's list, are in
[plaintext-dictionaries.json](plaintext-dictionaries.json) (`frequency`) and
bee-san/plaintext-dicts' README.

The same measurement for Bee's Ultimate Dictionary against the ranked file before it (main's
bundle, plaintext-dicts `6f841ce`): the shown entry changes for 0.81% of the tokens, 0.80% of them to a grammar
point's entry that is the text as written (できる, かもしれない, いつも, つもり), and the shown
reading is Jiten's for 97.19% (97.17% before; 98.44% and 98.42% where the whole word matched). Of
the changes, 0.05% move to Jiten's reading and 0.03% away from it: alias keys whose summary names
that form (わけじゃない shows わけではない) and a few spellings whose two readings rank alike, where
the longer article decides and the labels' removal changed the lengths (時折 shows ときより).
Evaluation outputs and the changed taps: the fleet's
`shared/artifacts/ultimate-dict-overlay/eval/vn-ultimate-v1.json` and `taps-ultimate-v1/` (not committed;
Jiten's decks are CC BY-SA 4.0).

A selected-entry allocation or file error is reported explicitly. Lookup does
not grow the worker arena or silently display a partial definition. The older
whole-sentence host API remains available for benchmark compatibility; its
64 KiB output/128-token limits are not the local overlay cursor path.

## Supported content and tradeoffs

- Term dictionaries only. Frequency-only, pitch-accent-only and kanji-only ZIPs
  are rejected as inputs with an explanation; a rank-based frequency
  dictionary can add ranks with `--frequency`. Pitch data are not imported.
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
local lookup and changing dictionaries during an in-flight job.
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

### Binary format, version 3 (with frequency ranks)

Version 3 is version 2 with these additions; `--frequency` writes it, and the
bundled dictionary is one.

- The header has magic `VJDICT3\0` and version=3, the rest as in version 2.
  Bytes 64..127 of page 0 hold the frequency list's name: 1-63 bytes of UTF-8
  text without control characters, then NUL padding to byte 127. The runtime
  reads it with the 64-byte header, in the same read, as it reads the rest of
  page 0: bytes 128..159 and 256..4095 are the optional [key filter](#key-filter)'s,
  160..171 the optional posting hints' (below), and 192..255 the dictionary's
  title, as in version 2. Bytes 172..191 are not read.
- Every payload starts with a u32 frequency word, then the three strings as in
  version 2. Bits 0-23 are the rank (1 = the most frequent; 0 = the list does
  not rank the entry). Bit 24 marks a borrowed rank: a kanji spelling ranked
  with its word's rank written in kana. Bits 25-27 record how the rank matched,
  for the converter's report; bits 28-31 are zero. A payload is at least 8
  bytes.
- **Posting hints** (optional). Page-0 bytes 160..167 hold magic `VJPH` and a
  u32 version, 1 or 2 (below); a reader refuses any other version, and a reader
  of version 1 only (Rubi 0.9.8 and older) refuses version 2. Without the magic, a
  posting's last u16 is not read (version 2 requires it to be zero; version 3
  allows any value). With it, the u16 of a posting of key K holds:
  - bit 0, S: the entry's spelling is K, byte for byte;
  - bit 1, N: the spelling normalized is K. Normalizing maps each code point
    U+30A1..U+30F6 to itself minus 0x60 (katakana ァ..ヶ as hiragana) and
    U+FF01..U+FF5E to itself minus 0xFEE0 (full-width ASCII as ASCII), and
    keeps every other code point (ヷ..ヺ, ー and half-width katakana included);
    keys are normalized the same way;
  - bit 2, R: the entry has a reading;
  - bits 3-15: `freq_key` of the payload's frequency word. With
    `rank = word & 0xFFFFFF`, it is 0 for rank 0; otherwise, with e the index
    of rank's highest set bit (0..23) and m the 7 bits after it, truncated
    (`(rank >> (e - 7)) & 127` for e >= 7, else `(rank << (7 - e)) & 127`), it is
    `(bit 24 set ? 0 : 4096) + 4095 - (128 * e + m)`. A reader compares it with
    the scores of the same key's postings that it ranks through their heads,
    so the bits must be exactly this value, not just in the same order.

  S, N and R describe only the reader's head window: the first
  w = min(payload length - 4, 160) bytes after the frequency word. If those w
  bytes hold no NUL, S, N and R are 0. Otherwise, with the spelling's NUL at
  index i of the window, S and N compare bytes 0..i-1, and R is 1 exactly when
  i + 1 < w and byte i + 1 is not NUL. `posting_hint` in
  `tools/convert_dictionary.py` is the reference writer, and `rank_posting` in
  `core/local_dict.c` the reader.

  **Version 2, the table hints.** Page-0 bytes 168..171 hold a u32, the id of
  the lookup tables the bits below were made with: FNV-1a-32 (offset basis
  0x811C9DC5, prime 0x01000193) over the bytes of `preferred_readings` then
  `usual_spellings`, each table as its pairs (key, NUL, value, NUL) and one more
  NUL (`tools/lookup_tables.txt`; `core/lookup_tables.inc` has the bytes and
  `LOOKUP_TABLES_ID`). Every posting's u16 rule bits (bits 0-6: the inflection
  rules) may then have bit 11, P: K is a key of `preferred_readings` and its
  value V is the entry's spelling or reading, and bit 7, U: the same for
  `usual_spellings`. "Is the spelling" is S's comparison with V for K, inside
  the head window; "is the reading" means the reading, from byte i + 1 of the
  window, is V followed by its NUL, the NUL inside the window. Bits 8-10 and
  12-15 are zero, as are bits 7-15 of a key's own rule bits (its index record's).
  A reader whose tables have this id ranks the table keys' postings from the
  hints and these bits; one whose tables differ ignores the bits and reads
  their heads, as with version 1. A posting of version 1, or of a file without
  hints, has neither bit; this reader ignores the two bits in any file that is
  not version 2 of its own tables, and fails a lookup that reads any other rule
  bit past bit 6 (a format error, in every file). `table_bits` in `tools/convert_dictionary.py` is the
  reference writer. Like the other hints the bits are trusted: a file whose
  bits are wrong shows the entries they say, not what its heads would rank
  first, as a wrong frequency key does.

  The converter writes version 1 unless `--table-hints` is given, so a file
  converted for an older Rubi stays readable by it; the bundled dictionary has
  version 2, and the plugin that reads it comes with it. After an update the
  Shell plugin that is still loaded until the Vita restarts predates version 2,
  so until that restart its lookups say the dictionary is invalid.

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

Bytes 192..255 of page 0 may hold the dictionary's own name, its title: 1-63
bytes of UTF-8 text without control characters, then NUL padding, or all zero
for none (bee-san/plaintext-dicts writes `Bee's Ultimate Dictionary`; the
converter here writes none). The runtime reads it with the header, in the same
read (page 0 through the page cache when a lookup opens the file, 256 bytes
when a dictionary page is read), and names the file's entries by it on Anki
cards and in `log.txt`; a malformed title refuses the file. Readers that
predate it never read those bytes.

Payloads are three UTF-8 NUL-terminated strings: spelling, reading, definition.
Spelling and reading indexes share payloads. Rule bits are v1=1, v5=2, vs=4,
vk=8, vz=16, adj-i=32, iru=64. Opening a dictionary reads only its first page
(the header, the key filter's descriptor and version 3's posting hints'); pages,
offsets, bounds and payloads are validated as they are accessed.

#### Key filter

An optional part of version 2 and later files. The converter writes it by
default when the index has at least 4 buckets: with fewer, the index stays in
the page cache and the filter's own read costs more than it saves. Use
`--no-key-filter` to leave it out, or `--add-key-filter FILE [-o OUT]` to add it
to an existing file without changing anything else in it. Readers that predate
it read only the 64-byte header, so they still read a file that has one, and
read it as before.

- **Descriptor.** At byte 128 of page 0, 32 bytes: magic `VJKF`; u32 version=1;
  u32 block count; u32 hash count=7; u64 offset of the first block; u32 key
  count; u32 reserved=0.
- **Directory.** From byte 256 of page 0: one u32 per block, the block's first
  prefix. The first is 0, the rest increase. Bytes 160..191 are not the filter's
  (format 3's posting hints use 160..171), nor are bytes 192..255: a dictionary
  title may use them (bee-san/plaintext-dicts).
- **Blocks.** Page-aligned, 4 KiB each, and they end the file (the header's file
  size includes them). Each is a 32,768-bit Bloom filter, bit `i` being byte
  `i / 8`, bit `i % 8`. A key sets bits `(h1 + k * h2) mod 32768` for k = 0..6,
  where h1 is the low 32 bits of `key_hash`, and h2 the high 32 bits with bit 0
  set (wrapping 32-bit arithmetic).
- **Prefix.** Each key's bits go into the block whose directory range holds the
  key's prefix: its first two code points, 16 bits each (0xFFFF for a code point
  beyond the BMP). The second is 0 for a one-character key.
- **Packing.** The converter packs prefixes in increasing order at about 10
  bits a key (fewer only when 960 blocks would not hold them). It keeps a first
  character's whole range in one block when the range fits one.

A lookup uses one block per file: the one for its cursor's first two
characters, read through the shared page cache when the first candidate
with two characters or more needs it (its directory from the session's pool, or
from page 0). A candidate with two or more characters whose prefix is in that
block's range, and whose bits are not all set, is not a key, so its index page is
not read. A one-character candidate never picks or reads a block: one whose prefix
is in the range of the block a longer candidate picked, while the page cache still
holds that block, is checked against it the same way at no read (the names
dictionary has nearly none of は, が, を: 0.27 reads a lookup saved); one that comes
first or finds no block held searches the index as before (in a dictionary of
words it is nearly always a key, and reading a block for it would cost more than
it saves: +0.17 reads a lookup, measured by #360's plan). So does a candidate whose
prefix lies outside the block's range (an inflection such as した → する).

A Bloom filter has no false negatives, so lookups show exactly the same entries
with or without it (`tools/dict_lookup_bench.py` checks every lookup of the
benchmark sets). The bundled file's filter is 226 blocks (904 KiB) for 661,469
keys. It is never resident: one block a lookup is read into the existing page
cache.

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
