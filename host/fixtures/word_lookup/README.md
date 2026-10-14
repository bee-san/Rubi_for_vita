# Word lookup fixtures

`vocr_lines.tsv` holds 48 lines of real VOCR output: what the overlay's own
local OCR adapter (`vjo-ocr --backend vocr`, the line finder and the pinned
H15/FL10 recognizer the game worker runs) recognized in 32 dialogue regions of
vita-vn-ocr's line-finder development manifests (`eval/linefind/manifests`,
vita_dev and psp_dev, at a5d5aad). FL10's line is kept where it differs from
H15's, so real recognition errors (美味しいつて, 室息死, 無遠作) are included.
Only text is committed, with the model and screenshot checksums; no screenshot
or crop. The protected test sets (vita-test, psp-test, fresh native), CLANNAD,
every VN in eval/manifests/test_games.json ("all") and the owner's reserved
test titles are excluded; the 25 VNs are listed in the PR that added them.

`expect.tsv` names what a lookup at a character of those lines must show with
the bundled `plaintext.vjdict` (docs/plaintext-dictionaries.json): the
highlighted span and the entry's spelling and reading, for inflected verbs, a
na-adjective, kana-only words, katakana loanwords, names, common readings and
words broken by OCR errors (the longest prefix, then the character).

`host/tests/test_word_lookup.cpp` runs both through the production lookup,
the Shell's word navigation and the touch hit test; the Shell worker test
`word_lookup_with_the_bundled_dictionary_first_word_right_and_tap` runs one
line through the actual worker (Auto OCR, open, Right, tap, log lines).

Regenerate the candidate lines (the selection is by id):

```sh
python3 tools/collect_word_lookup_lines.py --ocr build/host/vjo-ocr --models MODELS \
    --vocr-repo /path/to/vita-vn-ocr --shots SHOTS_DIR [--shots ...] > lines.jsonl
```
