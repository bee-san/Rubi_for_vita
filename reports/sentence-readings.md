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

Filled in after the test measurement.
