# Rubi's lookups beside Yomitan's on 5,000 real visual-novel lines

The owner (2026-10-26): "Dictionary quality checks against Yomitan: deinflection, frequency and
pitch-accent display, compared on a few thousand real lines from your hooked text".

**Result.** On 64,579 taps (every word start of 5,000 hooked lines from 46 games), Rubi's shown
entry differed from Yomitan's first entry on **69.19 per 1,000 taps on main `213fd60`, 65.76 with
this change**. This change fixes two lookup bugs found this way (a wrong homograph for kana verbs,
and Yomichan's old bare `な` rule, which Yomitan dropped) and adds Yomitan's rules for spoken forms
that Rubi lacked (しておく, 変わらん, しちゃ, 戻らなきゃ, 認めざる, 返さねば, しやがって, 聞きやしねえ,
いたしまして, 知らねー). Most of the remaining difference is by design or in the frozen dictionary:
Rubi's grammar-point entries, which spelling of a word heads the entry, and reading ties in Jiten's
ranks ([remaining](#what-remains)). Rubi shows no pitch accents at all, where Yomitan with a
pitch dictionary shows one on 549 of every 1,000 taps. All of this is host measurement; nothing here
has run on a console.

## How it was measured

- **Lines.** The dialogue of vita-vn-ocr's vocr6-ingest page pool (`pagepool/v1`, manifest sha256
  `eab8f07b…`, `splits_v1.json` `417df3d7…`): the text each page's hook gave, speaker line removed and
  screen lines joined, train pages of the 46 train titles, 114-115 lines a title (fewer where a title
  has fewer: 6 titles), seeded (`tools/yomitan_check/build_corpus.py`, seed 20261026). Lines ids sha256 `7ea5cc35…`,
  texts `664b02ba…`. The texts are the games' scripts and are not committed; short excerpts below.
- **Taps.** Every word start: SudachiPy 0.7.0 with sudachidict_core 20261015, split mode C, less
  punctuation and the inner parts of inflected words: 64,579 taps, 12.9 a line.
- **Rubi.** `build/host/vjo-dict-taps` (new, `host/dict_taps.c`): the Shell's `vjo_overlay_lookup_at` at
  each tap, set up as `shell/worker.c` sets it up (one 64 KiB block, the session at its start, the line's
  text generation, Dictionaries shown 2), with the shipped `plaintext.vjdict` (Bee's Ultimate Dictionary,
  plaintext-dicts `e9b6e76`) and `vndb-names.vjdict`. Same C code as the console; host file reads.
- **Yomitan.** Release 26.10.12.0 (`e7334a9`), headless in Node 22.23.1 (`tools/yomitan_check/
  yomitan_lookup.mjs`): its own importer, database (fake-indexeddb, as its test suite) and
  `Translator.findTerms('group', …)` with the profile's defaults, on the tap's next 16 characters
  (`scanning.length`). Dictionaries: Rubi's sources, Daijirin4, Sankoku8 and JMdict Plain (the files the
  bundle is built from, in its priority order), the VNDB names ZIP Rubi converts, Jiten's global
  frequency list as the sort dictionary, and Kanjium's pitch accents (`mifunetoshiro/kanjium` `9ebca45`,
  CC BY-SA 4.0) as a Yomitan pitch dictionary (`prepare_yomitan_dicts.py`). Two runs gave identical
  output for every tap.
- **Compared** (`compare.py`): Rubi's shown entry against Yomitan's first one. A mismatch is a different
  highlighted span or headword (spelling, or reading with katakana as hiragana); each gets one cause.
  For a third opinion, SudachiPy's analysis of the tapped token says which side, if either, read the
  text as it does ("Sudachi agrees with"); it is a heuristic, not ground truth.

## Results, per 1,000 taps

| Cause | main | this change | Sudachi agrees with (after): Rubi / Yomitan / both / neither |
|---|---:|---:|---|
| **All mismatches** | **69.19** | **65.76** | |
| deinflection missing (Yomitan's longer inflected match) | 7.26 | 5.16 | 155 / 3 / 45 / 130 |
| deinflection extra (Rubi's longer inflected match) | 1.50 | 0.00 | |
| deinflection choice (same span, inflected or not, or two forms) | 11.30 | 11.38 | 400 / 58 / 65 / 212 |
| text normalization (Yomitan's text processors) | 2.86 | 2.86 | 63 / 7 / 5 / 110 |
| Rubi-only entry (none of Yomitan's dictionaries has it) | 15.36 | 15.36 | 0 / 434 / 0 / 558 |
| longest match, otherwise | 0.71 | 0.71 | 1 / 16 / 6 / 23 |
| reading (same word, another reading) | 9.86 | 9.86 | 230 / 259 / 93 / 55 |
| kana or kanji spelling (same word and reading) | 16.58 | 16.72 | 0 / 0 / 523 / 557 |
| another spelling of the reading | 2.76 | 2.76 | 0 / 0 / 50 / 128 |
| another word | 0.15 | 0.15 | |
| a name where the other shows a word | 0.82 | 0.79 | 8 / 3 / 22 / 18 |
| missing entry (a word Rubi's files lack) | 0.00 | 0.00 | |

Shown either way, per 1,000 taps (main = after unless noted):

| | per 1,000 | |
|---|---:|---|
| Yomitan's first entry has a pitch accent; Rubi shows none | 548.68 | no pitch data in Rubi's files or popup |
| Same headword, Rubi's rank is none of Jiten's values Yomitan shows | 9.60 (9.54) | 5.62 names (Rubi's names file has no ranks), 3.98 a rank Yomitan has none for |
| A name first, the word of its span second (Rubi / Yomitan) | 4.78 / 5.99 | わたし, みちる, なぎさ: both do it |
| A name of the span listed besides the word in one only | 0.68 | |

Per title the mismatch rate after this change runs from 39.8 to 100.5 per 1,000.

## Fixed in this change

1. **A homograph for kana verbs.** With no entry spelled as the text (kana text of a verb written in
   kanji), Rubi took the first matching candidate's best entry, so the rule table's order decided:
   わかった → 分かつ (rank 19,788) over 分かる (278), both one step away. Now the best-ranked of the
   candidates with the fewest steps wins, as among entries spelled so (`take_postings`, one byte of
   padding and a score slot): 28 taps of 分かつ → 分かる, and まわった 回る, たたった 祟る, つかんだ 掴む.
2. **The bare imperative negative `な`.** Yomichan's rule turning any dictionary form + な into the form
   ("don't …") matched every な after a word: 失礼な → a verb 失礼 read しつらい, はずなのに → 恥づ,
   こうなって → 請う, しそうな → 試奏; 101 taps on main. Removing it, Sudachi reads 6 of them better and
   1 worse (72 have the same dictionary form either way: 捨てるな, 捨てる). Yomitan
   no longer has it; nor does Rubi (するな now highlights する, the next word な).
3. **Yomitan's rules for spoken forms** (`third_party/yomitan/`, ported by `port_yomitan_rules.mjs`):
   `-おく` (しておく, 聞いておいた, 言わないでおく), `-ん` (変わらん, 構わん, 言えん), `-ちゃ` (しちゃ駄目,
   近づけちゃ), `-ゃ` (戻らなきゃ, 目指しゃ, なりゃ), `-ざる`, `-ねば`, `-やがる`, `-やしない`, `-て`'s
   まして (いたしまして, 改めまして) and `-え`'s ぇ and ー forms (知らねー, できねー, いねぇ, やべぇ). Each
   rule family was kept only where the corpus showed it helps: dropped are `imperative negative slang`
   (んな: みんな read as 見る, 53 taps), `-ん`'s さん, たん, はん, なん (the honorific, りったん, ごはん,
   なん "what") and `-ゃ`'s や and にゃ (the particle, a cat's ニャ).

Over all taps the change alters 341 shown entries: Sudachi calls 72 of them better and 19 worse (the
rest it cannot tell apart); 264 move to Yomitan's entry and 43 away from it. Read one by one, the
19 "worse" are mostly Sudachi splitting a phrase that both dictionaries have whole (聞き捨てならねー,
役に立たねぇ) and two ties of equal-depth candidates (つかまれた 捕まる, かわされた 買う: Yomichan's
one-step causative passive, which Yomitan does not have). File reads per lookup on the 50 committed
lines are unchanged (6.12 first word, 5.50 word by word, 5.53 letter by letter; `dict_lookup_bench.py`),
so the console estimate is too. The table grows from 569 rules to 724 and the Shell's pages do not:
the table is packed ([memory ledger](../docs/memory-ledger.md#yomitans-deinflection-rules-a-packed-table)).

## What remains

None of these is a clear bug in the lookup code; they are design choices, the frozen dictionary's data,
or missing data, for the owner to decide ([issue](#)):

- **Pitch accents (548.68 / 1,000).** Rubi's files have no pitch data and the popup has no pitch line;
  `VjoVocab.pitch` and the Anki card's pitch fields stay empty. Kanjium's accents (124,137 rows, 3.2 MB
  text) would cover them; adding them needs a format field or a separate file, and popup space.
- **Reading ties (9.86).** Jiten gives every reading of a spelling one rank (恐怖 きょうふ and くふ both
  1,692), so the longer article decides: 恐怖 くふ, 化粧 けそう, 一言 いちげん, 真実 しんじち, 雰囲気 ふいんき.
  Of the 637 taps, 354 are such ties; Jiten's kana-form ranks (㋕) of the two readings would choose
  Yomitan's reading in 206 (Sudachi agreeing in 173) and keep Rubi's in 148. Breaking ties by source
  order instead is worse (68.55 overall). A fix needs those ranks in the dictionary (the bundle is
  frozen).
- **Rubi-only entries (15.36).** Bee's Ultimate Dictionary's grammar points match longer spans than any
  of Yomitan's dictionaries: のこと, 前に, がある, になる, んだけど, と思う. Sudachi splits 434 of the 992,
  agrees with neither in the rest. By design; whether 前に and の姿 should win over 前 and の is a content
  question.
- **Kana or kanji spelling (16.72).** The same word headed by another spelling: Rubi's common-readings
  table (そう → 然う, ここ → 此処, いい → 良い) and the bundle's deduplication choose the kanji spelling where
  Yomitan's JMdict shows the kana one. Same word and definition; cosmetic.
- **Kana names before words (4.78).** A name spelled as the text beats a word spelled otherwise, in any
  dictionary (the cross-dictionary rule), so わたし shows the character わたし with 私 below it (89 taps),
  and みちる, なぎさ, いのり likewise. Yomitan does the same (5.99), but Rubi's docs say the names come
  after the word ("2: a word, then the same word as a character's name").
- **Ranks of a different word (3.98).** The bundle's converter matches a spelling-only Jiten row after
  kana normalization, so だと shows ダト's #73,969, もか モカ's #45,140, にす ニス's #34,211, where Yomitan
  shows no rank. The bundle records the match kind (bits 25-27); a display rule could hide these.
- **Names have no rank (5.62).** vndb-names.vjdict is format 2: わたし, なぎさ show no Jiten rank.
- **Yomitan rules not taken.** `-げ` (気 after a verb: していた気), kansai-ben (作られへん, but also 言うたび
  as 言うた), `-まい` (あるまい), `-ふ` and emphatic-sequence collapsing (なーんて): each matches more
  wrongly than rightly here, or too rarely to measure.
- **Equal-depth ties.** つかまれた (掴む passive or 捕まる potential), かわされた (交わす or Yomichan's 買う
  causative passive), なれる (なる potential or 慣れる): the rank decides, as in Yomitan.

## Reproduce

```sh
A=/path/to/work   # nothing here is committed: the corpus, the dictionaries, Yomitan
python3 tools/yomitan_check/build_corpus.py --pagepool POOL/pagepool/v1 --splits POOL/splits_v1.json \
    -o $A/corpus.jsonl --manifest $A/corpus-manifest.json           # needs sudachipy, sudachidict_core
python3 tools/yomitan_check/prepare_yomitan_dicts.py --sources PLAINTEXT_DICTS/sources+vndb \
    --accents kanjium/accents.txt -o $A/yomitan-stored
git clone --branch 26.10.12.0 https://github.com/yomidevs/yomitan $A/yomitan
(cd $A/yomitan && npm ci --ignore-scripts && npm run build:libs)
node tools/yomitan_check/yomitan_lookup.mjs --yomitan $A/yomitan --dicts $A/yomitan-stored \
    --taps $A/corpus.jsonl --out $A/yomitan.jsonl                    # ~15 min in one process; --shard I/N
build/host/vjo-dict-taps DICTS plaintext.vjdict,vndb-names.vjdict < $A/corpus.tsv > $A/rubi.tsv
python3 tools/yomitan_check/compare.py --corpus $A/corpus.jsonl --rubi $A/rubi.tsv \
    --yomitan $A/yomitan.jsonl --rubi-dict DICTS/plaintext.vjdict --names-dict DICTS/vndb-names.vjdict \
    --sources $A/yomitan-dicts -o $A/report.json
```

`corpus.tsv` is `id\ttext\ttaps` from `corpus.jsonl`. The fleet's copy of every input and output is
`shared/artifacts/rubi-dict-check/` (not committed).

## Limits

- Host only: the same lookup code, the host's file reads. Nothing here ran on a console.
- Hooked text, not OCR: real recognition errors would add mismatches of their own.
- Yomitan's defaults and these dictionaries: other settings (scan length, frequency sorting, a
  different JMdict) change its first entry. Yomitan's first entry is not ground truth either; where
  the two disagree, Sudachi's analysis is a third opinion, not an answer.
- The frequency check behind docs/local-dictionaries.md's table (Jiten's deck exports) was not run
  again: the exports are not on this machine.
