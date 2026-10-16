# Words split over two lines: text boxes

Text only, for `host/tests/test_word_lookup.cpp` (`split_words_are_looked_up_whole`,
`kept_breaks_stay_breaks`), which runs the Shell's lookup and arrows on them with the bundled
dictionary.

- `split_words.tsv`: 77 text boxes whose wrapping split a word over two lines (お母 / さん, 求 / める,
  プロ / ポーズ): the lookup at the word's first character, on the text with its line breaks, shows
  the whole word, its highlight running across the break, and Right lands after its tail on the
  next line.
- `kept_breaks.tsv`: 54 boxes with a break that must stay one: 34 after a speaker's name line,
  20 at a sentence's end.

Two sources:

- **vita-test and psp-test** (vita-vn-ocr `eval/manifests/vita-test.jsonl`, `psp-test.jsonl`, whose
  text is public there; the `id` column is the box: screenshot sha256 and region): 21 split words,
  each a joined break of these sets whose crossing word was reviewed as a split word or a grammatical
  unit (21 of their 27 joined crossings; the other six are longest matches across a phrase boundary,
  きた / お, が / い, に / と, きか / ず, or doubtful, ん / な, い / ず), and 22 kept breaks (14 after a
  name line, 8 at a sentence's end, the first in the manifests' order). Only read here: no model is
  trained, tuned or scored on them.
- **Synthetic** (`synthetic-NN`): original sentences written for this test, one in the layout of
  each of the 56 split words and 32 kept breaks reviewed in the realplus-v3 training pool: the same
  word split at the same place (大発 / 見, おか / しい, スカー / ト), with or without a name line
  before it, or the same kind of name line or sentence end. The pool's own text stays out of git
  (conductor, 2026-10-17); only its counts are reported (`docs/touch-lookup.md`).

No screenshot, crop or other image is here.
