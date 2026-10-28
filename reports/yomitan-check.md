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
ranks ([remaining](#what-remains), [#321](https://github.com/bee-san/Rubi_for_vita/issues/321)). Rubi shows no pitch accents at all, where Yomitan with a
pitch dictionary shows one on 549 of every 1,000 taps. All of this is host measurement; nothing here
has run on a console.

**#321** ([below](#321-what-remained)) fixed what of the rest is the lookup's, measured the same way: a particle
before a form of する or a katakana word is the particle (気がした, のマンション), ranks of another word are hidden,
a frequent word comes before a character's name spelled as the text (わたし: 私), and four of the audit's lookups.
It changes 753 shown entries: SudachiPy calls 661 better and none worse, and 619 of them move away from
Yomitan's first entry, which reads those particles into the next word too, so the mismatch with Yomitan rises to
75.27 per 1,000 while SudachiPy's disagreement with the shown entry falls from 285.28 to 275.04 (Yomitan's:
286.47).

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
   1 worse (70 show the same entry either way, spelling and reading: 捨てるな, 捨てる). Yomitan
   no longer has it; nor does Rubi (するな now highlights する, the next word な). Read one by one, 20 of
   the 101 are prohibitives (捨てるな！, 言うなよ), whose verb now shows without the "imperative negative"
   label, as in Yomitan; most others are なら, なんて, なのに, なり or a sentence-final な, which the rule
   misread (audit of #320).
3. **Yomitan's rules for spoken forms** (`third_party/yomitan/`, ported by `port_yomitan_rules.mjs`):
   `-おく` (しておく, 聞いておいた, 言わないでおく), `-ん` (変わらん, 構わん, 言えん), `-ちゃ` (しちゃ駄目,
   近づけちゃ), `-ゃ` (戻らなきゃ, 目指しゃ, なりゃ), `-ざる`, `-ねば`, `-やがる`, `-やしない`, `-て`'s
   まして (いたしまして, 改めまして) and `-え`'s ぇ and ー forms (知らねー, できねー, いねぇ, やべぇ). Each
   rule family was kept only where the corpus showed it helps: dropped are `imperative negative slang`
   (んな: みんな read as 見る, 53 taps), `-ん`'s さん, たん, はん, なん (the honorific, りったん, ごはん,
   なん "what") and `-ゃ`'s や and にゃ (the particle, a cat's ニャ).

Over all taps the change alters 341 shown entries: Sudachi calls 72 of them better and 19 worse (the
rest it cannot tell apart); 264 move to Yomitan's entry and 43 away from it. Read one by one, 9 of the
19 "worse" are right: Sudachi splits a form that both dictionaries match whole (聞き捨てならねー,
役に立たねぇ, 乗りやしない), and 4 are neither better nor worse, but 6 are worse (4 of them as Yomitan reads them): two
ties of equal-depth candidates (つかまれた 捕まる, かわされた 買う: Yomichan's one-step causative passive,
which Yomitan does not have), 手をつかん(で) as 手をつく (`-ん`'s かん), とっておき as とる (`-おく` on a noun),
ないねぇ as 無い無い (the particle ねぇ read as `-え`'s ない) and ものがねぇ as the phrase 物が無い, whose sense
does not fit. Read by hand,
40 of the 341 at random (seed 20261026) are 23 better, 16 the same and 1 worse (このまま【にしておく】
shows にす, "negation", as Yomitan does, where main showed the grammar entry にして; the word is にする).
Two labels can mislead although the word is right:
the contracted progressive てん (してんの, 言ってん: 9 taps) shows `-n`, the negative, and Rubi's own ー
rules read an elongated て (見てー, 聴かせてー) as `-tai` (audit of #320). File reads per lookup on the 50 committed
lines are unchanged (6.12 first word, 5.50 word by word, 5.53 letter by letter; `dict_lookup_bench.py`),
so the console estimate is too. The table grows from 569 rules to 724 and the Shell's pages do not:
the table is packed ([memory ledger](../docs/memory-ledger-history.md#yomitans-deinflection-rules-a-packed-table)).

## #321: what remained

[#321](https://github.com/bee-san/Rubi_for_vita/issues/321) listed what #320 left: the classes below, and its
audit's lookups. Measured the same way, main `68041a7` against #321's change, on the 64,579 taps and, for what
changes elsewhere, on 330,517 taps of 27,970 lines of the same 46 games (`build_corpus.py --lines 40000 --seed
321`: every train page the pool has, so it holds the 5,000):

| | main | #321 |
|---|---:|---:|
| Shown entry differs from Yomitan's first, per 1,000 taps | 65.76 | **75.27** |
| SudachiPy's token disagrees with the shown entry, per 1,000 (Yomitan's first entry: 286.47) | 285.28 | **275.04** |
| Rubi's rank is none of Jiten's values Yomitan shows (same headword), per 1,000 | 9.60 | 4.89 |
| A name first, the word of its span second, per 1,000 | 4.78 | 3.31 |

The mismatch with Yomitan rises because #321 departs from Yomitan's first entry where both read a particle into
the next word: of the 753 shown entries it changes (spans or headwords), 619 move away from Yomitan's and 5 toward
it, and SudachiPy calls 661 better, 92 the same (わたし: it cannot tell a name from 私) and none worse. On the 330,517
taps, 3,962 shown entries change: 3,494 better, 465 the same, 3 worse (below). By change, on the 64,579 taps (the
330,517 in brackets):

- **A particle before a katakana word**, 457 taps (2,341): one of が も は に の か と で を へ や な in hiragana
  before a katakana letter (not the small ァィゥェォ or ッ) is the particle. Rubi read the word as a homograph or an inflection of a word
  starting with the particle, and so did Yomitan, through its katakana-to-hiragana step: のマンション 飲む's `-ん`,
  のメンバー 飲める, にエントリー 煮える, とセンパイ 渡船, のスタッフ のす, のカバン のか, はデート 派手. Not before ァィゥェォ, ッ
  or ー (はァ, へッ: interjections). A katakana ending of one word loses: でチュ, a baby-talk です (10 taps of one game
  in the 330,517), is で.
- **A particle before a form of する**, 198 taps (1,125): が も は に の か in hiragana and a candidate that is the
  particle and す or する are the particle, unless an entry is spelled as the whole span (がする, もしかして, にしても).
  The audit's 38 (が + する as 賀す or 賀する, も + する as 燃す or 模する) and more: 目にした was the grammar entry にす
  (a classical negation) 25 times, にしておく にす, はしない 馳す, 気のせい のす, どうかした 貸す, ぼくはそう 破窓, もしかして
  at か 貸し手. Another word's inflection does not keep the span: kept, にさせる would be 似る and はせず 馳せる (24
  taps of the 330,517 worse, 2 better); so かすれた (掠れる) is か, and かすれ (カスレ, wrong either way) too: 2 of the 3
  worse.
- **Ranks of a different word hidden**, 223 taps (1,377): a rank from a spelling-only Jiten row matched only after
  kana normalisation shows as none, as in Yomitan (だと ダト's #73,969, はない, のな, にす ニス's, もか, イル いる's #37, ラ ら's).
  It still ranks the entry; the posting hints and the frozen file are unchanged.
- **Names first** (`names_first = auto`, the default): 91 taps (456), わたし 私 89 (449), あたる 当たる 1 (5), イチ 一 1
  (2). [The measurement](../docs/local-dictionaries.md#names-and-words): of 309 taps with a name first and the word below,
  the name is right in 188 and the word in 109, 89 of them わたし, the only word among the 1,023 most frequent.
  `on` keeps Rubi 0.9.8's order, `off` puts every word first.
- **Labels**, 14 taps (108): the contracted progressive (してんの, 言ってんだ, バカにされてん: `-te → progressive or
  perfect`, not `... → -n`, 8), and `-おく` through `-て` as Yomitan (言っとく, 払っとく: `-te → -toku`, 6), which also
  makes 行っとく 行く, not 行う (1 tap of the 330,517).
- **The causative passive as two steps**, 5 taps (25): かわされた 交わす, not 買う; 飛ばされた 飛ばす, 聞かされた 聞かす,
  立たされて 立たす, 思い知らされる 思い知らす, 待たされた 待たす, 悩まされて 悩ます (each the passive of its own word, as
  Yomitan, whose causative passive is two steps).
- **`-ん`'s かん left out**, 2 taps (14): 手をつかんで 手, not 手をつく; なんだかわかんない at か かわ (乾く before: wrong either way);
  宙に浮かんだ 宙. しておかん loses its `-toku → -n` and shows して (the 3rd worse), and 話をしておかん shows 話をする over
  話をして, `-te` only (SudachiPy: the same; #321's audit).

The Shell's pages are unchanged; the rule table moved to the data segment
([memory ledger](../docs/memory-ledger-history.md#lookups-321)).

## What remains

None of these is a bug in the lookup code: they are design choices, which #321 keeps, the frozen dictionary's data,
or missing data:

- **Pitch accents (548.68 / 1,000).** Rubi's files have no pitch data and the popup has no pitch line;
  `VjoVocab.pitch` and the Anki card's pitch fields stay empty. Kanjium's accents (124,137 rows, 3.2 MB
  text, CC BY-SA 4.0) would cover them, as a file of their own: the frozen bundle has no field for them, and the
  Shell has no room for a second file's lookup and a pitch line (after #321 its debug build has 20 B of code
  room). Their likeliest place is the Rubi app's card export, which runs beside no game.
- **Reading ties (9.86).** Jiten gives every reading of a spelling one rank (恐怖 きょうふ and くふ both
  1,692), so the longer article decides: 恐怖 くふ, 化粧 けそう, 一言 いちげん, 真実 しんじち, 雰囲気 ふいんき.
  Of the 637 taps, 354 are such ties; Jiten's kana-form ranks (㋕) of the two readings would choose
  Yomitan's reading in 206 (Sudachi agreeing in 173) and keep Rubi's in 148. Breaking ties by source
  order instead is worse (68.55 overall). A fix needs those ranks in the dictionary (the bundle is
  frozen) or a table of the tied spellings in the Shell (no room).
- **Rubi-only entries (15.36).** Bee's Ultimate Dictionary's grammar points match longer spans than any
  of Yomitan's dictionaries: のこと, 前に, がある, になる, んだけど, と思う. Sudachi splits 434 of the 992,
  agrees with neither in the rest. By design; whether 前に and の姿 should win over 前 and の is a content
  question.
- **Kana or kanji spelling (16.69).** The same word headed by another spelling: Rubi's common-readings
  table (そう → 然う, ここ → 此処, いい → 良い) and the bundle's deduplication choose the kanji spelling where
  Yomitan's JMdict shows the kana one. Same word and definition; cosmetic.
- **Names have no rank.** vndb-names.vjdict is format 2: わたし, なぎさ show no Jiten rank. Yomitan shows the
  word's (わたし #1,901㋕ is the pronoun's) on the name; Rubi does not put a word's rank on a character.
- **The prohibitive な.** 見るな shows 見る without "imperative negative", as in Yomitan: な is the next word, whose
  entry is the prohibition (#14). The bare rule that labelled it misread 81 of its 101 taps (なら, なんて, なのに,
  なり, 失礼な), and limited to verb endings it would still label なら and なんて after a verb.
- **Yomitan rules not taken.** `-げ` (気 after a verb: していた気), kansai-ben (作られへん, but also 言うたび
  as 言うた), `-まい` (あるまい), `-ふ` and emphatic-sequence collapsing (なーんて): each matches more
  wrongly than rightly here, or too rarely to measure. The colloquial ら → ん before ない (知んない, 変わんない,
  たまんない) has no rule in either; one (んない → らない) would also read 案内 written あんない as ある.
- **Equal-depth ties.** つかまれた (掴む's passive or 捕まる's potential) and なれる (なる's potential or 慣れる): the
  rank decides, as in Yomitan.
- **`names_first` with Dictionaries shown 1.** One result stops at the name before the first dictionary's
  inflections are read, so with `off` the first entry can differ from two shown: 8 of the 759,994 character
  positions of the 27,970 lines, none with `auto` or `on`
  ([names and words](../docs/local-dictionaries.md#names-and-words); #321's audit). Reading on for one result costs file reads; leaving out only the inflection's homograph (half of
  it) measured 16 B of the debug Shell's code and 48 B of the normal's.
- **A phrase at a longer span.** ないねぇ shows 無い無い and ものがねぇ 物が無い (`-え`'s ねぇ read as ない after ない
  and が, 2 taps), as in Yomitan.
- **とっておき** shows とる's `-te → -toku → masu stem` (1 tap; 5 of the 330,517): its entries are spelled in kanji
  (取って置き), and at one span a kana inflection of a kana entry (the grammar entry とる) beats a kanji homograph by
  design (kana text finds its kana word or its inflection first: した する, not 下). A row in the lookup tables
  (`tools/lookup_tables.txt`) would fix it; since #371 those tables are also the bundled dictionary's table hints, so a
  row changes the released file's pin. Not worth one tap.

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
    --sources $A/yomitan-dicts --examples 30 -o $A/report.json
# #321's wider check, Rubi only (every train page of the pool: 27,970 lines, 330,517 taps)
python3 tools/yomitan_check/build_corpus.py --pagepool POOL/pagepool/v1 --splits POOL/splits_v1.json \
    --lines 40000 --seed 321 -o $A/big.jsonl --manifest $A/big-manifest.json
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
