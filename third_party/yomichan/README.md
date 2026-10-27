# Japanese deinflection rules

`deinflect.json` is from FooSoft/Yomichan, commit
`2f13cf5d2ad33a627152457a3f120deb76739f60`:
https://github.com/FooSoft/yomichan/blob/2f13cf5d2ad33a627152457a3f120deb76739f60/ext/data/deinflect.json

The file is that one's JSON reformatted one key a line (79,074 B; upstream's is 51,920 B): the same
labels, rules and order (`json.load` of both is equal).

Copyright the Yomichan authors; GPL-3.0-or-later (its `LICENSE` at that commit), used here under
version 3, this project's licence (see the repository's LICENSE). `tools/gen_deinflect.py` generates
`core/deinflect.inc` from it and from Yomitan's supplement
(`third_party/yomitan/deinflect-supplement.json`, which adds rules and removes one).
The source data and generated table are committed so building the plugin does not
require network access or Node.
