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
| **Rubi**, measured at the protocol's commit, as first counted | 823.9 | 174.3 | **1.84** |
| **Rubi**, measured at the protocol's commit, recounted after the audit (three verdicts corrected, below) | 823.6 | 174.3 | **2.12** |
| Rubi as merged in #407 (four changes after the measurement, below), recounted | 823.9 | 174.1 | **2.05** |
| Rubi after the audit's follow-up (通りに, below) | 823.8 | 174.3 | **1.91** |
| Yomitan's `{sentence-furigana}`, same dictionaries | about 920.0 | 33.7 | **about 46.4** (95 %: 40.5-52.1) |

**1.84, recounted 2.12, is under the threshold of 5: Sentence readings is ON by default.**

The protocol above (with the rules' code) was committed and pushed as `cf2e273` at 17:15:51Z on 2026-10-27. The
test lines were drawn at 17:16:03Z, and the code was measured on them unchanged. After the rebases onto main (the
last onto `73db61e`) the commit is `e6810d8`: the protocol's text and the engine's code are the same, byte for byte.

- *Right* is Sudachi's reading, or another the reviewer judged right where the two differ.
- *The verdicts* are given by pattern (the unit, its rendering and Sudachi's reading) and, where the reading
  depends on the context, by place. The independent audit (audit-rubi-rm1) read 40 verdicts drawn at random
  (seed 311) in context and agreed with all 40, and read every place of each pattern with two places or more:
  three places judged right were wrong. 通り read どおり after a verb, twice (言われた通りに, 指示された通りに:
  とおり; after a noun, as the pattern's other four places, どおり is right), and 声高らか read こわだからか (it is
  声 + 高らか, こえたからか). They are judged per place now, and every figure here is recounted with them: at the
  protocol's commit 30 tokens are wrong, not 26.
- *Rubi* (at the protocol's commit): 603 tokens differed from Sudachi. Of them 573 were judged right: 214 where
  Sudachi was wrong (何[なに]を, 大谷[おおたに], 額[ひたい], 通り[どおり] after a noun) and 359 where both are (私:
  わたし or わたくし, 傍: そば or かたわら). 30 were wrong: 28 Sudachi's way and 2 neither.
- *Yomitan:* 1,405 tokens differed from Sudachi, in 1,341 units, more than 300, so a seeded sample of 300 units
  was judged, as the protocol says (`compare.py --sample yomitan=300 --seed 20261027`: the sample and the estimate
  are committed code). 143 of them were wrong (声高らか, which Yomitan reads as Rubi did, among them since the
  correction): 147 of the sample's 315 tokens, 46.7 %, or about 46.4 per 1,000 (95 % bootstrap interval over the
  sample's units, 10,000 resamples). Its common errors are the ones Rubi's rules leave
  without a reading: 言葉 read けとば (a rare entry), 彼 read あれ or か, 君 read きみ in 颯斗君, 様 read よう in 柳生様,
  家 read うち, 一言 read いちげん, 説明 read ときあか (a rare 説明す).
- *The analyser:* Sudachi was wrong on at least 216 tokens, 15.3 per 1,000: those where Rubi's reading was
  judged right or neither was. That is a lower bound, since only the tokens where a system disagrees were
  judged.

Why Rubi left words without a reading (test lines, words with kanji, not tokens; the code after the audit's
follow-up):

| | Words |
| --- | ---: |
| Matched words with kanji | 13,474 |
| Given a reading | 11,067 |
| None: another entry as frequent reads it otherwise | 1,207 |
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
the engine was run again on both sets: the test lines' readings are the same, line for line (2.05 wrong, 823.9
right, 174.1 missing per 1,000, as recounted), and one development line gains a reading, Sudachi's. Run again on main `73db61e`
(#398 and #402's lookup fixes), both sets' readings, lookups and reads are the same as on `9ad8eb5`, line for line.
The figures here are those as-rebased ones.

### After the measurement

Four changes to the engine came after the measurement, in #407 (the as-merged row). Reading the test lines'
disagreements showed two faults, fixed then:

- A match of kana alone, or a rare one from kana, gave way to a word one character on in the wrong case.
  ラ変 in から変わった (らへん) took 変 and read it へん; the fix reads 変わった (かわった).
- A number with its counter as one entry (５分, ごふん) was not placed on the digit.

Two more were made while the engine moved into the app, and #407 did not list them (the audit found them in the
diff since `cf2e273`):

- A dictionary reading that fits the word in no way at all is no longer placed on the whole word (`place()`).
- A romanized name's `n'` reads ん (Shin'ichi: しんいち; it gave no kana before), and an apostrophe elsewhere is
  kept (the VNDB names rule).

The four together change 3 test lines (部員 and 上 gain a reading; 変 of 変わった goes from へん to か): 1 reading
less is wrong, 2 more are given. A fifth change was tried and not kept: letting a rare inflected match fall back
to its kanji alone. It mended one development line (損 read そこ, from 損ぬ) and cost two correct readings on the
test lines.

**The audit's follow-up** adds one rule. The longest match can take an unranked phrase entry over a word and its
particle: 通りに, read どおりに, which is right after a noun and wrong after a verb. Now the word of such an entry
(one that ends in a particle after kana) gets no reading when another word of its text, of another Jiten rank and
as frequent within 4 times, reads it otherwise (通り: とおり 1,755, どおり 2,007). Readings of one rank are one
word's (言う: いう and ゆう, both 25: 言うと keeps いうと), and ranked phrases keep theirs (何かと, 未だに). On the
development lines it changes nothing; on the test lines it takes the reading off the three places of 通りに: the
two wrong ones after a verb and one right one after a noun (the 1.91 row). A wider version, the same check for
every such entry, ranked or not, cost 15 right readings on the development lines and mended none (言うと, 何かと,
未だに lose theirs), so it is not kept. Nor is a check for a match the next kana make another word of (声高 of
声高らか, 高らか): it cost 22 right readings on the development lines, and does not catch 声高らか (高らか, 9,696, is
not 4 times as frequent as 声高, 21,269), which stays wrong.

### Export time

The export looks up each card's sentence again. On the 3,000 test lines a sentence took 17.3 lookups and 98.5
file reads (median 16 and 90, 95th percentile 37 and 216). The console fit of
[lookup-performance.md](../docs/lookup-performance.md) is 13.1 ms per lookup and 2.98 ms per read. By it, a card
takes about 0.52 s (median 0.49 s, 95th percentile 1.11 s), so 100 cards take about 52 s more. This is an
estimate: the app's lookups have not been timed on a console. A cache of 256 KiB of 4 KiB blocks
would have saved 39 % of the reads (295,633 to 180,943 with the follow-up's code, simulated); it is not built.

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
    --sample yomitan=300 --seed 20261027 --sample-out $A/yomitan-sample.tsv -o $A/report.json --patterns $A/patterns.tsv
```

The adjudication's place lines (`@line-id:start<TAB>verdict<TAB>rendering`) judge one place for the system that
renders it so: the three corrected places, and the Yomitan sample's units no pattern judges.

`lines.tsv` is `id<TAB>text` from `lines.jsonl` (a line break as `\n`, a backslash as `\\`). The counts
are in [`sentence-readings/results.json`](sentence-readings/results.json). The fleet's copy of every input and output
is `shared/artifacts/rubi-rm1/` (not committed).

## Limits

- Host only: the app's own code, on the host's file reads. Nothing here ran on a console.
- Hooked text, not OCR. A recognition error makes another word, and with it another reading.
- One reviewer (the rubi-rm1 agent) judged the disagreements; where a reading depends on the scene, the
  line's context was read. The audit was a second reader for 40 random verdicts and the patterns with several
  places (three corrected, above), of the same model family. Agreement with Sudachi was not reviewed: where
  both are wrong the same way, the error is not counted.
- A unit read in part (some of its kanji without a reading) is counted missing when its readings agree with
  Sudachi's tokens where they lie; how one token's reading splits between its kanji is not checked (大[だいが]学
  of だいがく would pass). No such unit of the test or development lines changed when that check was added.
- The lines were measured whole (`vjo_sentence_rubies` over each line). A card's own field
  (`vjo_sentence_furigana`) parses the text before and after the mined word apart and gives the mined word the
  card's reading: its readings next to the word are not measured, only its form and memory (host tests).
- Missing readings are many: one kanji token in six has none. The rules choose no reading over a doubtful one.
