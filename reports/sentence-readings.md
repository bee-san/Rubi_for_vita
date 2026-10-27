# Sentence readings at export: wrong and missing readings (#311)

[#311](https://github.com/bee-san/Rubi_for_vita/issues/311) asks for readings over a card's sentence
(`SentenceFurigana`), made by the Rubi app at export from the local dictionaries, and for their default to be set
by a measurement: "Count wrong and missing readings per 1,000 kanji tokens on a few thousand real VN lines ... Compare
with Yomitan's `{sentence-furigana}` over the same dictionaries, and with a morphological analyser's readings. Fix
the threshold in the PR before measuring. The default is ON only if wrong readings are rare by it; otherwise it is
OFF." This report is that measurement. Everything here ran on the host; nothing has run on a console.

## Protocol (fixed before the test lines were read)

This section was committed and pushed before the test lines were drawn, with the rules
(`core/sentence_reading.c`) at their final defaults.

- **Lines.** Real visual-novel dialogue: vita-vn-ocr's vocr6-ingest page pool (`pagepool/v1`, train pages of its
  train titles, the hook's text with the speaker's line removed), the same pool as
  [Rubi's lookups beside Yomitan's](yomitan-check.md).
  - *Development:* that report's 5,000 lines of 46 titles. The rules were designed and their constants chosen on
    these lines, with Sudachi's readings and the reviewer's judgements of the disagreements.
  - *Test:* 3,000 more lines of the same titles, drawn by `tools/sentence_readings/corpus.py` (seed 20261027),
    none of them a development line or text. Measured once, with the committed rules.
  - The texts are the games' scripts: they stay local (only the manifest of ids and text hashes, and counts, are
    committed).
- **Kanji tokens.** SudachiPy 0.7.0 with sudachidict_core 20261015, split mode C: every token with a kanji
  (`vjo_is_kanji`'s: CJK ideographs, 々〆ヵヶ). A system's readings laid over a token can join it with its neighbours
  into one unit, judged whole, counted once per kanji token in it (`tools/sentence_readings/compare.py`).
- **Verdicts.** A unit is *correct* when the system's rendering of it (each reading in place of its kanji) is
  Sudachi's reading, *missing* when some of its kanji have no reading and the rest agree with Sudachi, and otherwise
  differs from Sudachi. Every unit that differs is read in context by the reviewer (the rubi-rm1 agent, Claude) and
  judged: the system right, both right, Sudachi right, neither, or unclear. Sudachi right, neither and unclear
  count as **wrong**; so does a unit not reviewed. A unit Sudachi has no reading for (an unknown word) is not
  counted.
- **Systems.**
  - *Rubi:* `vjo-sentence-readings` (`host/sentence_readings.c`), the app's own code
    (`core/sentence_reading.c` `vjo_sentence_rubies`, default rules) on each whole line, with the shipped
    `plaintext.vjdict` (Bee's Ultimate Dictionary, plaintext-dicts `e9b6e76`) and `vndb-names.vjdict`, in that
    order. On a card the mined word takes the card's own reading instead (`vjo_sentence_furigana`).
  - *Yomitan:* release 26.10.12.0 (`e7334a9`), headless (`tools/sentence_readings/yomitan_furigana.mjs`): its
    scanning parser as its Anki note builder runs it for `{sentence-furigana}`, over the files the bundle is built
    from (Daijirin4, Sankoku8, JMdict Plain, the VNDB names) and Jiten's list as the sort dictionary
    (`tools/yomitan_check/prepare_yomitan_dicts.py`).
  - *The analyser:* Sudachi itself, the reference above.
- **The threshold.** **Sentence readings is ON by default if Rubi's wrong readings on the test lines are at most 5
  per 1,000 kanji tokens; otherwise it is OFF.** Yomitan's rates are reported beside Rubi's; they do not decide.
  If Yomitan's units that differ from Sudachi are more than 300, a seeded random sample of 300 of them is judged and
  its wrong rate estimated from it, with a 95 % interval.

## Results

On the 3,000 test lines (41 titles, 85,945 characters; their ids and text hashes in
[`sentence-readings/test-manifest.json`](sentence-readings/test-manifest.json)): 14,265 kanji tokens, 14,148 with a
reading of Sudachi's. Per 1,000 of those:

| | Right | Missing | **Wrong** |
| --- | ---: | ---: | ---: |
| **Rubi**, measured at the protocol's commit | 823.9 | 174.3 | **1.84** |
| Rubi, as merged (two fixes after the measurement, below) | 824.1 | 174.1 | **1.77** |
| Yomitan's `{sentence-furigana}`, same dictionaries | about 920.6 | 33.7 | **about 45.7** (95 %: 40.0-51.4) |

**1.84 is under the threshold of 5: Sentence readings is ON by default.**

The protocol above (with the rules' code) was committed and pushed as `cf2e273` at 17:15:51Z on 2026-10-27. The
test lines were drawn at 17:16:03Z, and the code was measured on them unchanged. After the rebases onto main (the
last onto `13bc976`) the commit is `efaa72a`: the protocol's text and the engine's code are the same, byte for byte.

- *Right* is Sudachi's reading, or another the reviewer judged right where the two differ.
- *Rubi:* 603 tokens differed from Sudachi. Of them 577 were judged right: 218 where Sudachi was wrong
  (何[なに]を, 大谷[おおたに], 額[ひたい], 通り[どおり] after a noun) and 359 where both are (私: わたし or わたくし,
  傍: そば or かたわら). 26 were wrong: 24 Sudachi's way and 2 neither.
- *Yomitan:* 1,405 tokens differed from Sudachi, in 1,341 units, more than 300, so a seeded sample of 300 units
  was judged, as the protocol says. 142 of them were wrong. That is 46 % of the sample's tokens, or about 45.7 per
  1,000 (bootstrap interval over the sample's units). Its common errors are the ones Rubi's rules leave
  without a reading: 言葉 read けとば (a rare entry), 彼 read あれ or か, 君 read きみ in 颯斗君, 様 read よう in 柳生様,
  家 read うち, 一言 read いちげん, 説明 read ときあか (a rare 説明す).
- *The analyser:* Sudachi was wrong on at least 220 tokens, 15.6 per 1,000: those where Rubi's reading was
  judged right or neither was. That is a lower bound, since only the tokens where a system disagrees were
  judged.

Why Rubi left words without a reading (test lines, words with kanji, not tokens):

| | Words |
| --- | ---: |
| Matched words with kanji | 13,474 |
| Given a reading | 11,070 |
| None: another entry as frequent reads it otherwise | 1,204 |
| None: a single kanji in a compound the lookup split | 517 |
| None: the entry's reading does not fit the text, or it has none (a name) | 352 |
| None: a part of it, as a word, reads it otherwise | 100 |
| None: an inflection the lookup did not find (働か read as 働 alone, 扱いだ as 扱ぐ's) | 100 |
| None: VNDB's name of it reads it otherwise | 86 |
| None: a counter after a number, a date's day | 42 |
| None: the text's own reading in brackets differs | 3 |

On the development lines the same code gives 812.8 right, 184.6 missing and 2.63 wrong per 1,000 kanji tokens
(24,682), every difference with Sudachi judged.

**After the rebase onto #386's lookups** (main `9ad8eb5`: its deinflection rules and `core/local_dict.c` changed),
the engine was run again on both sets: the test lines' readings are the same, line for line (1.77 wrong, 824.1
right, 174.1 missing per 1,000), and one development line gains a reading, Sudachi's. The figures here are those
as-rebased ones.

### After the measurement

Reading the test lines' disagreements showed two faults, which were fixed after the measurement:

- A match of kana alone, or a rare one from kana, gave way to a word one character on in the wrong case.
  ラ変 in から変わった (らへん) took 変 and read it へん; the fix reads 変わった (かわった).
- A number with its counter as one entry (５分, ごふん) was not placed on the digit.

Their effect on the test lines is in the table: 3 lines change, 1 reading less is wrong, 2 more are given. A
third change was tried and not kept: letting a rare inflected match fall back to its kanji alone. It mended one
development line (損 read そこ, from 損ぬ) and cost two correct readings on the test lines.

### Export time

The export looks up each card's sentence again. On the 3,000 test lines a sentence took 17.3 lookups and 98.5
file reads (median 16 and 90, 95th percentile 37 and 216). The console fit of
[lookup-performance.md](../docs/lookup-performance.md) is 13.1 ms per lookup and 2.98 ms per read. By it, a card
takes about 0.52 s (median 0.49 s, 95th percentile 1.11 s), so 100 cards take about 52 s more. This is an
estimate: the app's lookups have not been timed on a console. A cache of 256 KiB of 4 KiB blocks
would have saved 39 % of the reads (295,476 to 180,853, simulated); it is not built.

## Reproduce

```sh
A=/path/to/work   # the corpus, the systems' outputs and the adjudication stay local
python3 tools/sentence_readings/corpus.py --pagepool POOL/pagepool/v1 --splits POOL/splits_v1.json \
    --exclude DEV/corpus.jsonl -o $A/lines.jsonl --manifest $A/manifest.json
python3 tools/sentence_readings/sudachi.py $A/lines.jsonl -o $A/sudachi.jsonl   # sudachipy, sudachidict_core
build/host/vjo-sentence-readings --dir DICTS --dicts plaintext.vjdict,vndb-names.vjdict < $A/lines.tsv > $A/rubi.tsv
node tools/sentence_readings/yomitan_furigana.mjs --yomitan YOMITAN --dicts STORED --lines $A/lines.jsonl \
    --out $A/yomitan.tsv                                  # as tools/yomitan_check sets Yomitan up
python3 tools/sentence_readings/compare.py --corpus $A/lines.jsonl --sudachi $A/sudachi.jsonl \
    --system rubi=$A/rubi.tsv --system yomitan=$A/yomitan.tsv --adjudication $A/adjudication.tsv \
    -o $A/report.json --patterns $A/patterns.tsv
```

`lines.tsv` is `id<TAB>text` from `lines.jsonl` (a line break as `\n`, a backslash as `\\`). The counts
are in [`sentence-readings/results.json`](sentence-readings/results.json). The fleet's copy of every input and output
is `shared/artifacts/rubi-rm1/` (not committed).

## Limits

- Host only: the app's own code, on the host's file reads. Nothing here ran on a console.
- Hooked text, not OCR. A recognition error makes another word, and with it another reading.
- One reviewer (the rubi-rm1 agent) judged the disagreements; where a reading depends on the scene, the
  line's context was read. Agreement with Sudachi was not reviewed: where both are wrong the same way, the
  error is not counted.
- Missing readings are many: one kanji token in six has none. The rules choose no reading over a doubtful one.
