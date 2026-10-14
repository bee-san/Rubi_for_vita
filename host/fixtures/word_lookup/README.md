# Word lookup fixtures

`vocr_lines.tsv` holds 50 lines of real VOCR output: what the overlay's own
local OCR adapter (`vjo-ocr --backend vocr`, the line finder and the pinned
H15/FL10 recognizer the game worker runs) recognized in 33 dialogue regions of
vita-vn-ocr's line-finder development manifests (`eval/linefind/manifests`,
vita_dev and psp_dev, at a5d5aad). FL10's line is kept where it differs from
H15's, so real recognition errors (美味しいつて, 脱組んで for 腕組んで, 友違 for
友達, 士 for 上) are included. Only text is committed, with the model and
screenshot checksums; no screenshot or crop.

`tools/collect_word_lookup_lines.py` reads only VNs that vita-vn-ocr's realplus
reservation (`realplus/shots.reserved_vns` over the VNDB dump) leaves free or
reserves only as `development` or `val-bucket` (development and validation
splits), and no title of an owner `*-vNNN-TEST` folder. It skips CLANNAD and its
family and relations, the owner's test series (Memories Off, Aiyoku no Eustia,
428) and their relations, eval's test sets, `is_test_vn`, the fresh native test,
and the family and shared-release links of those: 16 of the manifests' 100 VNs
(v36 v481 v1170 v1267 v1281 v1517 v2063 v3106 v4060 v5690 v7675 v10796 v11301
v12509 v15771 v19716). The 26 VNs here are v728 v1015 v1629 v1970 v6723 v8591
v9885 v10968 v11173 v11879 v12178 v13090 v13766 v14813 v14869 v15231 v15577
v16770 v17641 v18164 v18449 v18457 v18641 v19012 v19141 v24291 (v1015, v13766
and v16770 are development VNs; the others are not reserved).

`expect.tsv` names what a lookup at a character of those lines must show with
the bundled `plaintext.vjdict` (docs/plaintext-dictionaries.json): the
highlighted span and the entry's spelling and reading, for inflected verbs and
adjectives, a na-adjective, kana-only words, katakana loanwords and katakana
inside them, names, common readings and words broken by OCR errors (the longest
prefix, then the character).

`host/tests/test_word_lookup.cpp` runs both through the production lookup,
the Shell's word navigation and the touch hit test; the Shell worker test
`word_lookup_with_the_bundled_dictionary_first_word_right_and_tap` runs one
line through the actual worker (Auto OCR, open, Right, tap, log lines).

Regenerate the candidate lines (the selection is by id), with vita-vn-ocr's
venv and the VNDB dump of `$HERD/shared/artifacts/realplus/vndb/dump`:

```sh
python3 tools/collect_word_lookup_lines.py --ocr build/host/vjo-ocr --models MODELS \
    --vocr-repo /path/to/vita-vn-ocr --shots SHOTS_DIR [--shots ...] > lines.jsonl
```
