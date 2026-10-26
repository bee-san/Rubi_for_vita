# Yomitan's deinflection rules (a supplement)

`deinflect-supplement.json` adds suffix rules of Yomitan's Japanese transforms to the old Yomichan
table (`third_party/yomichan/deinflect.json`), in that table's format. They are from
yomidevs/Yomitan release `26.10.12.0`, commit `e7334a96106a6c74e6fa75bd33a5ea1219d92712`,
`ext/js/language/ja/japanese-transforms.js`:
https://github.com/yomidevs/yomitan/blob/e7334a96106a6c74e6fa75bd33a5ea1219d92712/ext/js/language/ja/japanese-transforms.js

Copyright the Yomitan authors; GPL-3.0-or-later, compatible with this project's GPL-3.0 (see the
repository's LICENSE). `tools/yomitan_check/port_yomitan_rules.mjs` writes the file from a
checkout of that commit:

```sh
node tools/yomitan_check/port_yomitan_rules.mjs --yomitan /path/to/yomitan \
  > third_party/yomitan/deinflect-supplement.json
python3 tools/gen_deinflect.py   # core/deinflect.inc, core/conjugation.inc
```

- `add`: the rules of `-おく` (ておく, でおく, ないでおく; the old `-toku` has とく and どく),
  `-て`'s まして, `-え`'s ぇ forms and てえ, `-ん`, `-ちゃ`, `-ゃ` (composed with `-ば`, so that
  なきゃ gives ない), `-ざる`, `-ねば`, `-やがる` and `-やしない`, under the label Rubi shows.
  Yomitan's conditions become the old rule bits (the script says how).
- `remove`: the old bare imperative negative (な after a dictionary form), which Yomitan no
  longer has.
- `rubi`: Rubi's own, not Yomitan's: each `-え` rule again with ー for its last え or い
  (知らねー, すげー). Yomitan reaches these forms by turning ー into its vowel before the rules
  run; Rubi's key normalization does not.

Left out on purpose, measured on 5,000 hooked visual-novel lines ([the report](../../reports/yomitan-check.md)):
`imperative negative slang` (んな: みんな read as 見る), `-ん`'s さん, たん, はん and なん, and `-ゃ`'s
や and にゃ.
