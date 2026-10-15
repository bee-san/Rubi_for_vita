"""Real converter -> on-disk format -> C runtime -> overlay integration tests."""
import contextlib
import ftplib
import io
import json
import os
from pathlib import Path
import random
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import convert_dictionary as convert
import gen_deinflect
import install_dictionary as install

CLI = Path(os.environ.get("VJO_CLI", ROOT / "build/host/vjo-cli"))


def term(word, reading="", gloss=None, rules="", score=1):
    return [word, reading, "", rules, score, gloss or ["definition of " + word], 1, ""]


class LocalDictionaryTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def archive(self, rows, name="input.zip", title="Test"):
        path = self.directory / name
        with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as z:
            z.writestr("index.json", json.dumps({"format": 3, "title": title}))
            z.writestr("term_bank_1.json", json.dumps(rows, ensure_ascii=False))
        return path

    def build(self, rows, name="main.vjdict", title="Test", format_version=2):
        path = self.directory / name
        stats = convert.convert([self.archive(rows, title=title)], path, format_version=format_version)
        return path, stats

    def lookup(self, text, names="main.vjdict", ok=True):
        result = subprocess.run([str(CLI), "--dict", "local", "--local-dir", str(self.directory),
                                 "--local-dicts", names, "--text", text, "--nav", "--stats"],
                                capture_output=True, text=True, timeout=15)
        if ok:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertGreater(result.returncode, 0, "runtime crashed: " + result.stderr)
        return result

    def test_inflection_reading_structured_content_and_highlights(self):
        self.build([term("食べる", "たべる", ["to eat"], "v1"),
                    term("猫", "ねこ", [{"type": "structured-content", "content": [
                        {"tag": "div", "content": ["cat", {"tag": "br"}, "feline"]},
                        {"tag": "img", "alt": "cat illustration"}]}]),
                    term("行く", "いく", ["go"], "v5"), term("高い", "たかい", ["expensive"], "adj-i"),
                    term("する", "する", ["do"], "vs"), term("来る", "くる", ["come"], "vk")])
        result = self.lookup("😀猫は食べたくなかった。高くない。行った。しました。来ました。ネコ。")
        for expected in ("【猫】", "【食べたくなかった】", "【高くない】", "【行った】", "【しました】", "【来ました】",
                         "cat\nfeline", "cat illustration"):
            self.assertIn(expected, result.stdout)
        self.assertIn("猫 (ねこ)", self.lookup("ネコ").stdout)
        self.assertIn("【猫】", self.lookup("😀\n猫\nを見た").stdout)

    def test_local_is_the_only_dictionary(self):
        # The removed dictionaries are no --dict choice: the online ones (jpdb.io, jiten.moe) and the
        # Hachidori relay on a computer; --hachidori is no option any more. A config.ini that chose one
        # loads as local with one warning and looks words up in the local files.
        self.build([term("猫", "ねこ", ["cat"])])
        for args in (["--dict", "jpdb"], ["--dict", "jiten"], ["--dict", "hachidori"], ["--dict", "relay"],
                     ["--hachidori", "127.0.0.1:19633"]):
            result = subprocess.run([str(CLI), *args, "--text", "猫"], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 2, args)
            self.assertIn("usage: vjo-cli", result.stderr)
            self.assertNotRegex(result.stderr, r"(?i)achidori|jpdb|jiten|relay|API key")
        config = self.directory / "config.ini"
        config.write_text("dictionary = hachidori\nhachidori_host = 127.0.0.1:19633\n", encoding="utf-8")
        result = subprocess.run([str(CLI), "--config", str(config), "--local-dir", str(self.directory),
                                 "--local-dicts", "main.vjdict", "--text", "猫", "--nav"],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stderr, "config: dictionary: the Hachidori relay was removed; using local\n")
        self.assertIn("cat", result.stdout)
        self.assertIn("【猫】", result.stdout)

    def test_longest_match_and_rule_filter(self):
        self.build([term("猫"), term("猫舌", "ねこじた", ["sensitive to hot food"]),
                    term("食べる", "たべる", ["wrong part of speech"], ""),
                    term("食べる", "たべる", ["correct verb"], "v1")])
        result = self.lookup("猫舌。食べました。")
        self.assertIn("【猫舌】", result.stdout)
        self.assertNotIn("【猫】", result.stdout)
        self.assertIn("correct verb", result.stdout)
        self.assertNotIn("wrong part of speech", result.stdout)

    def test_multiple_dictionaries_and_priority(self):
        self.build([term("猫", "ねこ", ["first dictionary"])], title="First")
        self.build([term("猫", "ねこ", ["second dictionary"])], name="extra.vjdict", title="Second")
        output = self.lookup("猫", "main.vjdict,extra.vjdict").stdout
        self.assertIn("[First]", output)
        self.assertIn("[Second]", output)
        self.assertLess(output.index("first dictionary"), output.index("second dictionary"))

    def test_combined_archive_and_html(self):
        one = self.archive([term("猫", gloss=["<p>cat &amp; kitten</p><script>hidden()</script>"])], "one.zip", "One")
        two = self.archive([term("猫", gloss=["second"])], "two.zip", "Two")
        stats = convert.convert([one, two], self.directory / "main.vjdict")
        self.assertEqual(stats["entries"], 2)
        output = self.lookup("猫").stdout
        self.assertIn("cat & kitten", output)
        self.assertNotIn("hidden()", output)
        self.assertIn("[Two]", output)

    def test_deduplication_prefers_source_and_preserves_readings_senses_and_rules(self):
        first = self.archive([term("食べる", "たべる", ["preferred verb"], "v1"),
                              term("食べる", "たべる", ["another preferred sense"], "v1"),
                              term("仮", "かり", ["first reading"]),
                              term("かな", gloss=["preferred kana"])], "first.zip", "First")
        second = self.archive([term("食べる", "タベル", ["discarded fallback"], "v1", 100),
                               term("仮", "かり", ["discarded same reading"]),
                               term("仮", "け", ["distinct reading"]),
                               term("かな", "カナ", ["discarded explicit reading"]),
                               term("猫", "ねこ", ["second source fallback"])], "second.zip", "Second")
        third = self.archive([term("猫", "ねこ", ["discarded third source"]),
                              term("犬", "いぬ", ["third source fallback"])], "third.zip", "Third")
        for version in (1, 2):
            stats = convert.convert([first, second, third], self.directory / "main.vjdict",
                                    format_version=version, deduplicate=True)
            self.assertEqual(stats["source_entries"], [4, 2, 1])
            self.assertEqual(stats["deduplicated_rows"], 4)
            payload = self.payload_region(self.directory / "main.vjdict")
            self.assertNotIn(b"discarded", payload)
            self.assertIn(b"another preferred sense", payload)
            self.assertIn(b"distinct reading", payload)
            self.assertIn("preferred verb", self.lookup("食べました").stdout)
            self.assertIn("[Second]", self.lookup("猫").stdout)
            self.assertIn("[Third]", self.lookup("犬").stdout)

    def test_bilingual_structured_content_and_empty_image_labels(self):
        self.build([term("猫", "ねこ", [{"type": "structured-content", "content": [
            {"tag": "div", "content": ["cat; a feline", {"tag": "br"}, "猫科の動物。"]},
            {"tag": "img", "title": ""},
            {"type": "image", "alt": "", "title": "diagram caption"},
            {"tag": "img", "alt": "cat illustration", "title": "unused title"},
        ]}, '<p>English 日本語</p><img alt=""><img alt title="HTML caption">'])])
        output = self.lookup("猫").stdout
        for expected in ("cat; a feline\n猫科の動物。", "English 日本語", "diagram caption",
                         "cat illustration", "HTML caption"):
            self.assertIn(expected, output)
        self.assertEqual(output.count("[image omitted]"), 2)
        self.assertNotIn("unused title", output)

    def test_nested_bilingual_revision_keeps_plain_text_without_markup(self):
        # Invented content in the nested span/div/ruby/link shape of the
        # bilingual format-3 export. No dictionary definitions are fixtures.
        gloss = [{"type": "structured-content", "content": [
            {"tag": "span", "data": {"name": "heading"}, "content": {
                "tag": "span", "style": {"fontWeight": "bold"}, "content": "Synthetic heading"}},
            {"tag": "div", "content": {"tag": "div", "content": {"tag": "div", "content": [
                {"tag": "div", "content": ["English test: ", {"tag": "a", "href": "example.invalid", "content": "plain link"}]},
                {"tag": "div", "content": ["日本語テスト：", {"tag": "ruby", "content": [
                    "漢字", {"tag": "rt", "content": "suppressed reading"}]}]},
                {"tag": "img", "path": "private.svg", "alt": "image label"},
                {"tag": "style", "content": "hidden style"},
            ]}}},
        ]}]
        self.build([term("猫", "ねこ", gloss)])
        output = self.lookup("猫").stdout
        for text in ("Synthetic heading", "English test: plain link", "日本語テスト：漢字", "image label"):
            self.assertIn(text, output)
        for absent in ("suppressed reading", "hidden style", "private.svg", "example.invalid", "<div", "fontWeight"):
            self.assertNotIn(absent, output)

    def payload_region(self, path):
        """Bytes after the index: every stored spelling/reading/definition."""
        header = convert.HEADER.unpack_from(path.read_bytes())
        return path.read_bytes()[header[6]:]

    def test_payloads_are_plain_text_without_markup_or_css(self):
        # Jitendex-shaped structured content plus HTML strings with CSS,
        # scripts, links and ruby. The Vita reads payloads verbatim, so the
        # converter must leave no markup to parse or style to apply.
        sense = lambda marker, gloss: {"tag": "li", "style": {"listStyleType": marker, "fontWeight": "bold"},
                                       "data": {"content": "sense"}, "content": gloss}
        structured = {"type": "structured-content", "content": [
            {"tag": "span", "title": "noun", "data": {"class": "tag"}, "content": "noun"},
            {"tag": "span", "title": "kana", "data": {"class": "tag"}, "content": "kana"},
            {"tag": "ol", "content": [sense('"①"', "cat"), sense('"②"', "shamisen"), sense("disc", "unnumbered")]},
            {"tag": "div", "content": [{"tag": "span", "content": "See also"},
                                       {"tag": "a", "href": "?query=猫車", "content": "猫車"}]},
            {"tag": "div", "content": [{"tag": "ruby", "content": ["興", {"tag": "rt", "content": "きょう"}]},
                                       {"tag": "ruby", "content": ["味", {"tag": "rt", "content": "み"}]}, "がある"]},
            {"tag": "img", "path": "assets/logo.png", "alt": "logo"},
        ]}
        html = ('<style>.x{color:red}</style><div style="font-size:2em" class="g"><b>bold</b> text '
                '<a href="https://example.invalid/">link</a></div><ruby>漢字<rp>(</rp><rt>かんじ</rt><rp>)</rp></ruby>'
                '<script>alert(1)</script><img src="x.png" alt="pic">&lt;literal&gt;')
        for version in (1, 2):
            path, _ = self.build([term("猫", "ねこ", [structured, html])], format_version=version)
            data = self.payload_region(path)
            self.assertIsNone(re.search(rb"<(/?[A-Za-z][^>]*)>", data.replace(b"<literal>", b"")))
            for absent in (b"style", b"color", b"fontWeight", b"listStyleType", b"font-size", b"href",
                           b"example.invalid", b"assets/", b"x.png", b"alert", "きょう".encode(),
                           "かんじ".encode(), b"(", b"{"):
                self.assertNotIn(absent, data, absent)
            output = self.lookup("猫").stdout
            for expected in ("noun kana", "① cat", "② shamisen", "\nunnumbered", "See also 猫車",
                             "興味がある", "logo", "bold text link", "漢字", "pic", "<literal>"):
                self.assertIn(expected, output)

    def test_existing_payload_format_is_unchanged(self):
        # Plain payloads (spelling, reading, definition) are the same three
        # NUL-terminated strings as earlier files: no reconversion is needed.
        path, _ = self.build([term("猫", "ねこ", ["cat"])])
        self.assertIn("猫\0ねこ\0[Test]\ncat\0".encode(), self.payload_region(path))

    def test_reading_only_term_uses_reading_as_display_headword(self):
        _, stats = self.build([term("", "あて", ["a target; 目標。"]),
                               term("猫", "ねこ", ["cat"])])
        self.assertEqual(stats["entries"], 2)
        self.assertEqual(stats["reading_only_entries"], 1)
        self.assertEqual(stats["index_records"], 3)
        output = self.lookup("あて").stdout
        self.assertIn("【あて】", output)
        self.assertIn("a target; 目標。", output)
        _, stats = self.build([term("", "", ["no usable key"]), term("猫")])
        self.assertEqual(stats["entries"], 1)
        self.assertEqual(stats["skipped_empty_headwords"], 1)
        with self.assertRaisesRegex(ValueError, "no installable term entries"):
            self.build([term("", "", ["no usable key"])])

    def test_missing_partial_and_malformed_files(self):
        self.assertIn("Cannot read local dictionaries", self.lookup("猫", ok=False).stdout)
        path, _ = self.build([term("猫")], format_version=1)
        original = path.read_bytes()
        for change in (b"bad", original[:-1], b"WRONG!!!" + original[8:],
                       original[:16] + struct.pack("<I", 0xFFFFFFFF) + original[20:]):
            path.write_bytes(change)
            self.assertIn("local dictionary", self.lookup("猫", ok=False).stdout)
        # A well-sized header with a record pointing outside the file is rejected.
        bad = bytearray(original)
        struct.pack_into("<Q", bad, convert.HEADER.size, 0xFFFFFFFFFFFFFFFF)
        path.write_bytes(bad)
        self.assertIn("Invalid or incomplete", self.lookup("猫", ok=False).stdout)

    def test_config_does_not_allow_traversal_or_unbounded_files(self):
        self.build([term("猫")])
        for names in ("../main.vjdict", "main.vjdict,", "main.vjdict,,main.vjdict", "a/b.vjdict",
                      ",".join(["main.vjdict"] * 9)):
            self.assertIn("Cannot read local dictionaries", self.lookup("猫", names, ok=False).stdout)
        # A missing second file must not silently appear as a successful partial lookup.
        self.assertIn("Cannot read local dictionaries", self.lookup("猫", "main.vjdict,missing.vjdict", ok=False).stdout)

    def test_glossary_limit_is_reported_and_utf8_safe(self):
        _, stats = self.build([term("猫", gloss=["説明" * 10000])])
        self.assertEqual(stats["truncated_definitions"], 1)
        result = self.lookup("猫")
        self.assertIn("[definition truncated]", result.stdout)
        self.assertNotIn("\ufffd", result.stdout)

    def test_size_limit_and_exhausted_results_are_actionable(self):
        self.build([term("猫")])
        self.assertIn("smaller OCR region", self.lookup("猫" * 1400, ok=False).stdout)
        self.assertIn("smaller OCR region", self.lookup("猫。" * 129, ok=False).stdout)

    def test_128_repeated_occurrences_keep_individual_navigation(self):
        self.build([term("猫", "ねこ", ["cat; 猫科の動物。"])])
        result = self.lookup("猫。" * 128)
        navigation = result.stdout.split("[navigation]\n", 1)[1].splitlines()
        self.assertEqual(len(navigation), 128)
        for index, line in enumerate(navigation):
            self.assertTrue(line.startswith(f"{index + 1}: "))
            self.assertEqual(line.count("【猫】"), 1)
            self.assertTrue(line.split(": ", 1)[1].startswith("猫。" * index + "【猫】。"))
        self.assertNotIn("[error]", result.stdout)

    def test_large_disk_dictionary_keeps_lookup_memory_fixed(self):
        self.build([term("猫", "ねこ")])
        small = self.lookup("猫")
        rows = [term(f"項目{i:06d}", gloss=["synthetic large-dictionary definition " * 3]) for i in range(30000)]
        rows.append(term("猫", "ねこ"))
        path, _ = self.build(rows)
        self.assertGreater(path.stat().st_size, 4 * 1024 * 1024)
        large = self.lookup("猫")
        peak = lambda result: int(re.search(r"arena peak: (\d+)", result.stderr).group(1))
        self.assertEqual(peak(small), peak(large))
        self.assertLess(peak(large), 112 * 1024)

    def test_converter_atomic_failure_preserves_previous_output(self):
        path, _ = self.build([term("猫")], format_version=1)
        original = path.read_bytes()
        bad = self.archive([term("犬"), ["malformed"]])
        with self.assertRaisesRegex(ValueError, "row 2"):
            convert.convert([bad], path)
        self.assertEqual(path.read_bytes(), original)
        self.assertEqual(list(self.directory.glob("*.tmp")), [])

    def test_converter_rejects_non_term_dictionary(self):
        path = self.archive([])
        with zipfile.ZipFile(path, "w") as z:
            z.writestr("index.json", '{"format": 3, "title": "Frequency"}')
            z.writestr("term_meta_bank_1.json", '[]')
        with self.assertRaisesRegex(ValueError, "frequency/pitch/kanji-only"):
            convert.convert([path], self.directory / "main.vjdict")

    def test_streaming_json_and_invalid_separators(self):
        source = json.dumps([term("猫"), term("😀"), term("犬")], ensure_ascii=False)
        self.assertEqual(list(convert.stream_array(io.StringIO(source), 7)), json.loads(source))
        for source in ("[", "[[]", "[[],]", "[[] []]", "[] false", "{}"):
            with self.assertRaises(ValueError):
                list(convert.stream_array(io.StringIO(source), 2))

    def test_generated_table_is_reproducible(self):
        self.assertEqual(gen_deinflect.generate(), (ROOT / "core/deinflect.inc").read_text())

    def test_corrupt_index_does_not_crash_runtime(self):
        path, _ = self.build([term("猫", "ねこ")], format_version=1)
        original = path.read_bytes()
        rng = random.Random(17)
        for _ in range(24):
            bad = bytearray(original)
            offset = rng.randrange(convert.HEADER.size, convert.HEADER.size + 2 * convert.RECORD.size)
            bad[offset] ^= 0xFF
            path.write_bytes(bad)
            result = subprocess.run([str(CLI), "--dict", "local", "--local-dir", str(self.directory),
                                     "--text", "猫ネコ"], capture_output=True, timeout=10)
            self.assertIn(result.returncode, (0, 1), result.stderr)

    def test_legacy_and_hash_formats_return_identical_entries(self):
        rows = [term("猫", "ねこ", ["cat"]), term("食べる", "たべる", ["eat"], "v1"),
                term("猫", "ねこ", ["second sense"], score=0)]
        self.build(rows, "legacy.vjdict", format_version=1)
        self.build(rows, "hash.vjdict", format_version=2)
        text = "😀猫は食べませんでした。ネコ。"
        self.assertEqual(self.lookup(text, "legacy.vjdict").stdout, self.lookup(text, "hash.vjdict").stdout)
        mixed = self.lookup("猫", "legacy.vjdict,hash.vjdict").stdout
        self.assertEqual(mixed.count("second sense"), 2)
        self.assertTrue(install.inspect(self.directory / "legacy.vjdict").endswith(".vjdict"))
        self.assertTrue(install.inspect(self.directory / "hash.vjdict").endswith(".vjdict"))

    def test_hash_collision_requires_full_key_comparison(self):
        # Force a collision in the converter while retaining the real hash of
        # the query. Both groups occupy one hash range; only 猫 may match.
        with mock.patch.object(convert, "key_hash", return_value=convert.key_hash("猫".encode())):
            self.build([term("犬", gloss=["wrong dog"]), term("猫", gloss=["correct cat"])])
        result = self.lookup("猫").stdout
        self.assertIn("correct cat", result)
        self.assertNotIn("wrong dog", result)

    def test_overflow_bucket_and_invalid_cycle(self):
        # 200 keys share the real low hash bit, requiring an overflow page in
        # a two-bucket index. Find a key sorted into the overflow page.
        keys, i = [], 0
        while len(keys) < 200:
            key = f"語{i:06d}"
            if convert.key_hash(key.encode()) & 1 == 0:
                keys.append(key)
            i += 1
        path, stats = self.build([term(k) for k in keys])
        self.assertEqual(stats["hash_buckets"], 2)
        self.assertEqual(stats["index_pages"], 3)
        query = max(keys, key=lambda k: convert.key_hash(k.encode()))
        self.assertIn("definition of " + query, self.lookup(query).stdout)
        bad = bytearray(path.read_bytes())
        struct.pack_into("<Q", bad, convert.PAGE_SIZE + 8, convert.PAGE_SIZE)
        path.write_bytes(bad)
        self.assertIn("Invalid or incomplete", self.lookup(query, ok=False).stdout)

    def test_hash_format_rejects_invalid_pages_groups_and_postings(self):
        path, _ = self.build([term("猫")])
        original = path.read_bytes()
        slot = convert.PAGE_SIZE + convert.PAGE_HEADER.size
        group = struct.unpack_from("<Q", original, slot + 8)[0]
        modifications = [
            (48, "<Q", 3),  # bucket count must be a power of two
            (convert.PAGE_SIZE + 4, "<I", 171),
            (slot + 8, "<Q", 0xFFFFFFFFFFFFFFFF),
            (slot + 16, "<I", 0xFFFFFFFF),
            (slot + 20, "<H", 193),
            (group + len("猫".encode()), "<Q", len(original) + 1),
        ]
        for offset, fmt, value in modifications:
            bad = bytearray(original)
            struct.pack_into(fmt, bad, offset, value)
            path.write_bytes(bad)
            self.assertIn("Invalid or incomplete", self.lookup("猫", ok=False).stdout)
        path.write_bytes(original[:-1])
        with self.assertRaises(ValueError):
            install.inspect(path)

    def test_hash_index_reduces_reads_with_constant_memory(self):
        bench = os.environ.get("VJO_BENCH")
        if not bench:
            self.skipTest("VJO_BENCH is set by CTest")
        rows = [term(f"項目{i:06d}") for i in range(4000)] + [term("猫", "ねこ")]
        self.build(rows, "legacy.vjdict", format_version=1)
        self.build(rows, "hash.vjdict", format_version=2)
        def run(name):
            proc = subprocess.run([bench, str(self.directory), name, "猫。霻霻霻霻。猫。", "1"],
                                  capture_output=True, text=True, check=True)
            return json.loads(proc.stdout)
        before, after = run("legacy.vjdict"), run("hash.vjdict")
        self.assertEqual(before["digest"], after["digest"])
        self.assertLess(after["read_calls"], before["read_calls"] // 3)
        self.assertLess(after["arena_peak"], 112 * 1024)


    FILTER_ROWS = (term("食べる", "たべる", ["eat"], "v1"), term("猫", "ねこ", ["cat"]), term("猫舌", "ねこじた", ["tongue"]),
                   term("する", "", ["do"], "vs"), term("見る", "みる", ["see"], "v1"), term("行く", "いく", ["go"], "v5"),
                   term("高い", "たかい", ["high"], "adj-i"), term("話す", "はなす", ["talk"], "v5"),
                   term("学校", "がっこう", ["school"]), term("今日", "きょう", ["today"]))
    FILTER_TEXT = "今日は学校に行って、猫と話しました。高くない猫舌を見ませんでした。"

    def filler(self, n):
        return [term(f"語彙{i:05d}", gloss=[f"filler {i}"]) for i in range(n)]

    def shown(self, name, text=None):
        result = subprocess.run([str(CLI), "--dict", "local", "--local-dir", str(self.directory), "--local-dicts", name,
                                 "--text", text or self.FILTER_TEXT, "--nav"], capture_output=True, text=True, timeout=15,
                                check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_key_filter_layout_is_deterministic_and_shows_the_same_entries(self):
        rows = [*self.FILTER_ROWS, *self.filler(1200)]
        filtered, stats = self.build(rows, "filtered.vjdict")
        convert.convert([self.archive(rows)], self.directory / "nofilter.vjdict", key_filter=False)
        self.assertGreaterEqual(stats["hash_buckets"], convert.FILTER_MIN_BUCKETS)
        self.assertEqual(stats["filter_keys"], stats["unique_keys"])
        data = filtered.read_bytes()
        self.assertEqual(len(data) % convert.PAGE_SIZE, 0)
        self.assertEqual(struct.unpack_from("<Q", data, 40)[0], len(data))
        magic, version, blocks, hashes, offset, keys, zero = convert.FILTER_DESCRIPTOR.unpack_from(
            data, convert.FILTER_DESCRIPTOR_OFFSET)
        self.assertEqual((magic, version, hashes, keys, zero), (b"VJKF", 1, 7, stats["unique_keys"], 0))
        self.assertEqual(offset + blocks * convert.PAGE_SIZE, len(data))
        directory = struct.unpack_from(f"<{blocks}Q", data, convert.FILTER_DIRECTORY_OFFSET)
        self.assertEqual(directory[0], 0)
        self.assertEqual(list(directory), sorted(set(directory)))
        # Without the filter the file is the earlier layout, byte for byte up to the filter.
        nofilter = (self.directory / "nofilter.vjdict").read_bytes()
        self.assertFalse(any(nofilter[convert.FILTER_DESCRIPTOR_OFFSET:convert.PAGE_SIZE]))
        self.assertEqual(data[convert.PAGE_SIZE:len(nofilter)], nofilter[convert.PAGE_SIZE:])
        # Adding it to an existing file gives exactly the converter's file.
        added = convert.add_key_filter(self.directory / "nofilter.vjdict", self.directory / "added.vjdict")
        self.assertEqual((self.directory / "added.vjdict").read_bytes(), data)
        self.assertEqual(added["bytes"], len(data))
        expected = self.shown("nofilter.vjdict")
        self.assertIn("【話しました】", expected)
        self.assertIn("【見ませんでした】", expected)
        for name in ("filtered.vjdict", "added.vjdict"):
            self.assertEqual(self.shown(name), expected)
        # A filtered and a plain file together: each file keeps its own filter state.
        self.assertEqual(self.shown("filtered.vjdict,nofilter.vjdict"), self.shown("nofilter.vjdict,nofilter.vjdict"))
        self.assertTrue(install.inspect(filtered).endswith(".vjdict"))

    def test_key_filter_saves_index_reads(self):
        bench = os.environ.get("VJO_DICT_BENCH")
        if not bench:
            self.skipTest("VJO_DICT_BENCH is set by CTest")
        rows = [*self.FILTER_ROWS, *self.filler(4000)]
        self.build(rows, "filtered.vjdict")
        convert.convert([self.archive(rows)], self.directory / "plain.vjdict", key_filter=False)
        lines = self.directory / "lines.tsv"
        lines.write_text("# id\tmodel\t-\t-\t-\ttext\nline\tfixture\t-\t-\t-\t" + self.FILTER_TEXT + "\n")
        def run(name):
            proc = subprocess.run([bench, str(self.directory), name, str(lines), "words", "1"],
                                  capture_output=True, text=True, check=True)
            return [json.loads(row) for row in proc.stdout.splitlines() if '"summary"' not in row]
        plain, filtered = run("plain.vjdict"), run("filtered.vjdict")
        self.assertEqual([q["digest"] for q in plain], [q["digest"] for q in filtered])
        index = lambda qs, kind: sum(q["reads_by"][kind] for q in qs)
        self.assertLess(index(filtered, "index"), index(plain, "index") / 2)
        self.assertLess(sum(q["reads"] for q in filtered), sum(q["reads"] for q in plain))
        self.assertGreater(sum(q["filtered"] for q in filtered), 0)
        self.assertEqual(sum(q["filtered"] for q in plain), 0)
        self.assertLessEqual(max(q["arena_peak"] for q in filtered), 64 * 1024)

    def test_key_filter_is_refused_where_it_cannot_help_or_fit(self):
        small, stats = self.build(list(self.FILTER_ROWS), "small.vjdict")
        self.assertNotIn("filter_blocks", stats)  # 10 keys, one bucket: no filter
        self.assertFalse(any(small.read_bytes()[convert.FILTER_DESCRIPTOR_OFFSET:convert.PAGE_SIZE]))
        with self.assertRaisesRegex(ValueError, "hash bucket"):
            convert.add_key_filter(small, self.directory / "out.vjdict")
        legacy, _ = self.build(list(self.FILTER_ROWS), "legacy.vjdict", format_version=1)
        with self.assertRaisesRegex(ValueError, "version 2"):
            convert.add_key_filter(legacy, self.directory / "out.vjdict")
        filtered, _ = self.build([*self.FILTER_ROWS, *self.filler(1200)], "filtered.vjdict")
        with self.assertRaisesRegex(ValueError, "already has"):
            convert.add_key_filter(filtered, self.directory / "out.vjdict")
        self.assertFalse((self.directory / "out.vjdict").exists())
        result = subprocess.run([sys.executable, str(ROOT / "tools/convert_dictionary.py"), "--add-key-filter",
                                 str(filtered)], capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 1)
        self.assertIn("already has a key filter", result.stderr)

    def test_key_filter_keeps_page_zero_bytes_192_to_255(self):
        """Page-0 bytes 192..255 hold a dictionary title elsewhere: added around, kept as they are."""
        rows = [*self.FILTER_ROWS, *self.filler(1200)]
        convert.convert([self.archive(rows)], self.directory / "plain.vjdict", key_filter=False)
        plain = bytearray((self.directory / "plain.vjdict").read_bytes())
        plain[192:207] = b"A fixture title"
        (self.directory / "titled.vjdict").write_bytes(plain)
        convert.add_key_filter(self.directory / "titled.vjdict", self.directory / "out.vjdict")
        out = (self.directory / "out.vjdict").read_bytes()
        self.assertEqual(out[192:256], bytes(plain[192:256]))
        self.assertEqual(out[convert.FILTER_DESCRIPTOR_OFFSET:convert.FILTER_DESCRIPTOR_OFFSET + 4], b"VJKF")
        self.assertEqual(self.shown("out.vjdict"), self.shown("plain.vjdict"))
        for spot in (160, 191, 256, 4095):  # the filter's own bytes must be empty
            bad = bytearray(plain)
            bad[spot] = 1
            (self.directory / "bad.vjdict").write_bytes(bad)
            with self.assertRaisesRegex(ValueError, "not empty"):
                convert.add_key_filter(self.directory / "bad.vjdict", self.directory / "never.vjdict")
        self.assertFalse((self.directory / "never.vjdict").exists())

    SPREAD = "あいうえおかきくけこさしすせそたちつてとなにぬねのはひふへほまみむめもやゆよらりるれろわ今学行猫話高見日校友"

    def spread(self):
        """Keys over many first characters: several filter blocks."""
        return [*self.FILTER_ROWS, *[term(f"{c}語{i:04d}", gloss=["filler"]) for c in self.SPREAD for i in range(400)]]

    def test_key_filter_blocks_hold_every_key_where_the_directory_says(self):
        """Independently of build_key_filter's own bookkeeping: the directory's block for each key's
        prefix (the last start at or below it) has all 7 of the key's bits, block edges included."""
        import bisect
        keys = sorted({convert.normalize(word) for word, *_ in self.spread()}
                      | {convert.normalize(reading) for _, reading, *_ in self.spread() if reading})
        directory, blocks, stats = convert.build_key_filter((k, convert.key_hash(k)) for k in keys)
        self.assertGreaterEqual(stats["filter_blocks"], 5)
        edges = 0
        for key in keys:
            text = key.decode()
            prefix = min(ord(text[0]), 0xFFFF) << 16 | (min(ord(text[1]), 0xFFFF) if len(text) > 1 else 0)
            block = bisect.bisect_right(directory, prefix) - 1
            edges += directory[block] == prefix
            h = convert.key_hash(key)
            h1, h2 = h & 0xFFFFFFFF, ((h >> 32) | 1) & 0xFFFFFFFF
            for k in range(7):
                bit = ((h1 + k * h2) & 0xFFFFFFFF) & 32767
                self.assertTrue(blocks[block][bit >> 3] & (1 << (bit & 7)), (text, block, k))
        self.assertGreater(edges, 0)  # some keys open a block: the edge case is exercised

    def test_key_filter_sentence_lookups_choose_a_block_per_position(self):
        bench = os.environ.get("VJO_BENCH")
        if not bench:
            self.skipTest("VJO_BENCH is set by CTest")
        rows = self.spread()
        _, stats = self.build(rows, "filtered.vjdict")
        self.assertGreaterEqual(stats["filter_blocks"], 5)
        convert.convert([self.archive(rows)], self.directory / "plain.vjdict", key_filter=False)
        def run(name):
            proc = subprocess.run([bench, str(self.directory), name, self.FILTER_TEXT, "1"],
                                  capture_output=True, text=True, check=True)
            return json.loads(proc.stdout)
        plain, filtered = run("plain.vjdict"), run("filtered.vjdict")
        self.assertEqual(plain["digest"], filtered["digest"])
        # Each position of the sentence uses its own block (vjo_local_lookup's filter_restart):
        # with the first position's block kept, most candidates would bypass the filter.
        self.assertLess(filtered["read_calls"], plain["read_calls"] // 2)

    def test_key_filter_with_more_than_960_blocks_is_refused(self):
        """The directory must end inside page 0: 256 + 4 x 960 = 4096 bytes."""
        path, stats = self.build([*self.FILTER_ROWS, *self.filler(1200)])
        data = bytearray(path.read_bytes())
        blocks = stats["filter_blocks"]
        data += bytes((961 - blocks) * convert.PAGE_SIZE)
        offset = len(data) - 961 * convert.PAGE_SIZE
        struct.pack_into("<I", data, convert.FILTER_DESCRIPTOR_OFFSET + 8, 961)
        struct.pack_into("<Q", data, convert.FILTER_DESCRIPTOR_OFFSET + 16, offset)
        struct.pack_into("<Q", data, 40, len(data))
        path.write_bytes(data)
        self.assertIn("Invalid or incomplete", self.lookup(self.FILTER_TEXT, ok=False).stdout)

    def test_corrupt_key_filter_does_not_crash_runtime(self):
        path, _ = self.build([*self.FILTER_ROWS, *self.filler(1200)])
        original = path.read_bytes()
        offset = struct.unpack_from("<Q", original, convert.FILTER_DESCRIPTOR_OFFSET + 16)[0]
        rng = random.Random(23)
        spots = [rng.randrange(convert.FILTER_DESCRIPTOR_OFFSET, convert.FILTER_DESCRIPTOR_OFFSET + 32) for _ in range(12)]
        spots += [rng.randrange(convert.FILTER_DIRECTORY_OFFSET, convert.FILTER_DIRECTORY_OFFSET + 64) for _ in range(6)]
        spots += [rng.randrange(offset, len(original)) for _ in range(6)]
        for spot in spots:
            bad = bytearray(original)
            bad[spot] ^= 0xFF
            path.write_bytes(bad)
            result = subprocess.run([str(CLI), "--dict", "local", "--local-dir", str(self.directory),
                                     "--text", self.FILTER_TEXT], capture_output=True, timeout=10, check=False)
            self.assertIn(result.returncode, (0, 1), result.stderr)


def freq(term_, rank, reading=None, kana=False):
    """A Jiten-shaped rank-based frequency row: kana-form ranks show ㋕."""
    value = {"value": rank, "displayValue": f"{rank}㋕" if kana else str(rank)}
    return [term_, "freq", value if reading is None else {"reading": reading, "frequency": value}]


def v3_entries(path):
    """{(spelling, reading): frequency word} of every entry of a format 3 file."""
    data = path.read_bytes()
    magic, version, _, _, _, _, _, size, _, pages = convert.HEADER.unpack_from(data)
    assert magic == b"VJDICT3\0" and version == 3 and size == len(data)
    out = {}
    for page in range(pages):
        base = convert.PAGE_SIZE * (1 + page)
        count = struct.unpack_from("<I", data, base + 4)[0]
        for slot in range(count):
            _, group, postings, key_len, _ = convert.HASH_RECORD.unpack_from(data, base + 16 + 24 * slot)
            for j in range(postings):
                offset, length, _, _ = convert.POSTING.unpack_from(data, group + key_len + 16 * j)
                spelling, reading, _ = data[offset + 4:offset + length].split(b"\0", 2)
                out[(spelling.decode(), reading.decode())] = struct.unpack_from("<I", data, offset)[0]
    return out


class FrequencyTest(unittest.TestCase):
    """--frequency: a rank in every entry (format 3), matched as Yomitan matches, read by the overlay."""

    setUp, tearDown = LocalDictionaryTest.setUp, LocalDictionaryTest.tearDown
    archive, lookup = LocalDictionaryTest.archive, LocalDictionaryTest.lookup

    def frequency(self, rows, index=None, name="freq.zip"):
        path = self.directory / name
        with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as z:
            z.writestr("index.json", json.dumps(index or {"format": 3, "title": "Jiten", "revision": "test",
                                                          "frequencyMode": "rank-based"}))
            z.writestr("term_meta_bank_1.json", json.dumps(rows, ensure_ascii=False))
        return path

    def build_ranked(self, terms, rows, **kwargs):
        path = self.directory / "main.vjdict"
        stats = convert.convert([self.archive(terms)], path, frequency=self.frequency(rows), **kwargs)
        return path, stats

    def test_every_entry_gets_a_rank_or_the_unranked_zero(self):
        terms = [term("猫", "ねこ", ["cat"]), term("猫", "ネコ", ["cat in katakana reading"]),
                 term("ネコ", "", ["katakana spelling"]), term("から", "", ["particle"]),
                 term("先", "さき", ["ahead"]), term("先", "さっき", ["a while ago"]),
                 term("皆んな", "", ["variant spelling"]), term("凄い", "すごい", ["amazing"]),
                 term("珍品", "ちんぴん", ["rarity"]), term("ＩＤ", "", ["identity"]),
                 term("一", "", ["the character one"])]
        rows = [freq("猫", 1536, "ねこ"), freq("猫", 18226, "ねこ", kana=True),
                freq("ねこ", 18226, kana=True), freq("ネコ", 9209, kana=True), freq("から", 11, kana=True),
                freq("先", 221, "さっき", kana=True), freq("先", 416, "さき"), freq("先", 10533, "さき", kana=True),
                freq("先", 213803, "さっき"), freq("皆んな", 119, "みんな", kana=True),
                freq("凄い", 174, "すごい", kana=True), freq("凄い", 3866, "すごい"), freq("ID", 18980, "アイディー"),
                freq("一", 113075, "ひと"), freq("一", 795, "いち"), freq("一", 4158, "いち", kana=True)]
        path, stats = self.build_ranked(terms, rows)
        words = v3_entries(path)
        rank = {k: w & convert.FREQ_RANK_MAX for k, w in words.items()}
        self.assertEqual(stats["format_version"], 3)
        self.assertEqual(rank[("猫", "ねこ")], 1536)        # the reading's own (kanji-form) row
        self.assertEqual(rank[("猫", "ネコ")], 1536)        # after kana normalisation
        self.assertTrue(words[("猫", "ネコ")] & convert.FREQ_NORMALIZED)
        self.assertEqual(rank[("ネコ", "")], 9209)          # its spelling's own row before ねこ's
        self.assertEqual(rank[("から", "")], 11)            # spelling-only row
        self.assertEqual(rank[("先", "さき")], 416)         # not the kana-form 10533㋕
        self.assertEqual(rank[("先", "さっき")], 213803)    # 先 read さっき, not さっき in kana (221)
        self.assertEqual(rank[("凄い", "すごい")], 3866)
        self.assertEqual(rank[("皆んな", "")], 119)         # no reading: its spelling's reading rows,
        self.assertTrue(words[("皆んな", "")] & convert.FREQ_BORROWED)  # only a kana-form one: borrowed
        self.assertEqual(rank[("ＩＤ", "")], 18980)          # full width as ASCII
        self.assertEqual(rank[("一", "")], 795)             # the lowest of its own (kanji-form) rows
        self.assertFalse(words[("一", "")] & convert.FREQ_BORROWED)
        self.assertEqual(words[("珍品", "ちんぴん")], 0)       # unranked: the explicit zero
        coverage = stats["frequency"]
        self.assertEqual((coverage["entries_ranked"], coverage["entries_unranked"]), (10, 1))
        self.assertEqual(coverage["borrowed_ranks"], 1)
        self.assertEqual(coverage["unranked_value"], 0)
        self.assertEqual(coverage["by_match"]["unranked"], 1)
        self.assertEqual(sum(coverage["by_match"].values()), stats["entries"])
        # format 3's header: the list's name after version 2's 64 bytes
        data = path.read_bytes()
        self.assertEqual(data[:8], b"VJDICT3\0")
        self.assertEqual(data[64:128], b"Jiten".ljust(64, b"\0"))
        # deterministic
        again = self.directory / "again.vjdict"
        convert.convert([self.archive(terms)], again, frequency=self.frequency(rows))
        self.assertEqual(again.read_bytes(), data)

    def test_reading_rows_decide_before_spelling_only_rows(self):
        # Jiten has both kinds for あらら: the reading's row decides, even with a lower spelling-only rank;
        # the spelling-only row ranks the entries its reading rows do not cover.
        terms = [term("あらら", "あらら", ["oh dear"]), term("あらら", "アララ", ["katakana reading"]),
                 term("嗚呼", "ああ", ["ah"]), term("嗚呼", "あー", ["ah, long"])]
        rows = [freq("あらら", 94919, "あらら", kana=True), freq("あらら", 6888, kana=True),
                freq("嗚呼", 69, "ああ", kana=True), freq("嗚呼", 352)]
        path, _ = self.build_ranked(terms, rows)
        rank = {k: w & convert.FREQ_RANK_MAX for k, w in v3_entries(path).items()}
        self.assertEqual(rank[("あらら", "あらら")], 94919)
        self.assertEqual(rank[("あらら", "アララ")], 94919)  # its reading normalises to あらら's
        self.assertEqual(rank[("嗚呼", "ああ")], 69)
        self.assertEqual(rank[("嗚呼", "あー")], 352)       # no row for あー: the spelling-only one

    def test_the_overlay_reads_the_rank_and_breaks_ties_with_it(self):
        terms = [term("解答", "かいとう", ["answer, with the longest article of the two"]),
                 term("回答", "かいとう", ["reply"]), term("珍品", "ちんぴん", ["rarity"]),
                 term("猫", "ねこ", ["cat"])]
        rows = [freq("解答", 6000, "かいとう"), freq("回答", 3000, "かいとう"), freq("猫", 1536, "ねこ")]
        self.build_ranked(terms, rows)
        output = self.lookup("猫").stdout
        self.assertIn("猫 (ねこ) #1536", output)
        self.assertIn("珍品 (ちんぴん) unranked", self.lookup("珍品").stdout)
        # 回答 is the more frequent: it comes first, the longer article second
        both = self.lookup("かいとう").stdout
        self.assertLess(both.index("回答 (かいとう) #3000"), both.index("解答 (かいとう) #6000"))
        # without ranks (format 2) the longer article came first
        convert.convert([self.archive(terms)], self.directory / "main.vjdict")
        plain = self.lookup("かいとう").stdout
        self.assertLess(plain.index("解答 (かいとう)"), plain.index("回答 (かいとう)"))
        self.assertNotIn("#", plain)

    def test_frequency_dictionaries_are_checked(self):
        terms = [term("猫", "ねこ")]
        with self.assertRaisesRegex(ValueError, "rank-based"):
            convert.convert([self.archive(terms)], self.directory / "main.vjdict",
                            frequency=self.frequency([freq("猫", 1)], {"format": 3, "title": "Counts",
                                                                        "frequencyMode": "occurrence-based"}))
        with self.assertRaisesRegex(ValueError, "no usable frequency rows"):
            self.build_ranked(terms, [["猫", "pitch", {"reading": "ねこ", "pitches": []}]])
        with self.assertRaisesRegex(ValueError, "format 3"):
            convert.convert([self.archive(terms)], self.directory / "main.vjdict", format_version=3)
        with self.assertRaisesRegex(ValueError, "format 3"):
            convert.convert([self.archive(terms)], self.directory / "main.vjdict", format_version=2,
                            frequency=self.frequency([freq("猫", 1)]))
        # unusable values are counted and skipped, never guessed
        rows = [freq("猫", 0, "ねこ"), freq("猫", -3, "ねこ"), freq("猫", 1 << 24, "ねこ"), freq("猫", 1.5, "ねこ"),
                ["猫", "freq", {"reading": "ねこ", "frequency": "12㋕"}], ["猫", "freq", True],
                ["猫", "freq", {"reading": "", "frequency": 5}], ["猫", "pitch", {"reading": "ねこ", "pitches": []}],
                ["猫", "freq", {"reading": "ねこ", "frequency": "77"}]]
        path, stats = self.build_ranked(terms, rows)
        self.assertEqual(stats["frequency"]["unusable_rows"], 7)
        self.assertEqual(stats["frequency"]["other_mode_rows"], 1)
        self.assertEqual(v3_entries(path)[("猫", "ねこ")] & convert.FREQ_RANK_MAX, 77)  # a numeric string
        with self.assertRaisesRegex(ValueError, "expected \\[term, mode, data\\]"):
            self.build_ranked(terms, [["猫", "freq"]])
        # the list's name is clipped to 63 bytes of whole characters
        long_title = "長" * 30
        path = self.directory / "main.vjdict"
        convert.convert([self.archive(terms)], path, frequency=self.frequency(
            [freq("猫", 1, "ねこ")], {"format": 3, "title": long_title, "frequencyMode": "rank-based"}))
        title = path.read_bytes()[64:128].rstrip(b"\0").decode()
        self.assertEqual(title, "長" * 21)
        self.assertIn("猫 (ねこ) #1", self.lookup("猫").stdout)


class FakeFTP:
    def __init__(self, files=None, fail_replace=False, vitashell_dele=False):
        self.files = files or {}
        self.fail_replace = fail_replace
        self.vitashell_dele = vitashell_dele

    def retrbinary(self, command, callback):
        name = command[5:]
        if name not in self.files:
            raise ftplib.error_perm("550 missing")
        callback(self.files[name])

    def storbinary(self, command, stream, **kwargs):
        self.files[command[5:]] = stream.read()

    def rename(self, source, dest):
        if self.fail_replace and source.endswith("config.ini.local.tmp"):
            raise ftplib.error_perm("550 rename failed")
        if dest in self.files:
            raise ftplib.error_perm("550 destination exists")
        self.files[dest] = self.files.pop(source)

    def delete(self, name):
        if name not in self.files:
            raise ftplib.error_perm("550 missing")
        del self.files[name]
        if self.vitashell_dele:
            raise ftplib.error_reply("226 File deleted")  # VitaShell's reply to DELE (tools/install_ftp.sh)


class InstallerTest(unittest.TestCase):
    def test_preserve_settings_and_back_up_config(self):
        old = (b"; my config\ndictionary = hachidori\njiten_api_key = secret\nanki_host = 10.0.0.3\ndictionary = jiten\n"
               b"JPDB_API_KEY: secret2\napi_key = secret3\n; jpdb_api_key = a comment\njpdb_api_keysecret4 = x\n"
               b"  jiten_api_key secret5 ; dictionary = local\n")
        key = install.DATA_DIR + "/config.ini"
        ftp = FakeFTP({key: old})
        with contextlib.redirect_stdout(io.StringIO()) as out:
            install.enable(ftp, ["main.vjdict", "extra.vjdict"])
        config = ftp.files[key].decode()
        # the removed online dictionaries' API keys (secrets) go; comments stay
        self.assertNotIn("secret", config + out.getvalue())
        self.assertIn("; jpdb_api_key = a comment", config)
        self.assertIn("; my config\n", config)
        self.assertIn("anki_host = 10.0.0.3", config)
        self.assertEqual(config.count("dictionary = local"), 1)
        self.assertIn("local_dictionaries = main.vjdict,extra.vjdict", config)
        # the backup on the Vita: the old file without its key lines, every other byte kept; the
        # replaced file itself is deleted, so no file on the card holds a key (audit N1)
        backups = [path for path in ftp.files if "before-local" in path]
        self.assertEqual(len(backups), 1, backups)
        self.assertEqual(ftp.files[backups[0]],
                         b"; my config\ndictionary = hachidori\nanki_host = 10.0.0.3\ndictionary = jiten\n"
                         b"; jpdb_api_key = a comment\n")
        self.assertFalse(any(b"secret" in value for value in ftp.files.values()))
        # VitaShell confirms DELE with a reply ftplib raises error_reply for: the file is gone, no warning
        ftp = FakeFTP({key: old}, vitashell_dele=True)
        with contextlib.redirect_stdout(io.StringIO()) as out:
            install.enable(ftp, ["main.vjdict"])
        self.assertFalse(any(path.endswith(".active") for path in ftp.files))
        self.assertNotIn("Could not delete", out.getvalue())
        # a BOM before a key on the first line: the BOM stays, the key goes
        self.assertEqual(install.without_secrets(b"\xef\xbb\xbfjpdb_api_key = k\r\nx = 1\r\n"), b"\xef\xbb\xbfx = 1\r\n")
        self.assertEqual(install.without_secrets(b"x = 1\n"), b"x = 1\n")

    def test_restore_after_failed_config_rename(self):
        key = install.DATA_DIR + "/config.ini"
        ftp = FakeFTP({key: b"dictionary = hachidori\n"}, fail_replace=True)
        with self.assertRaises(ftplib.error_perm):
            install.enable(ftp, ["main.vjdict"])
        self.assertEqual(ftp.files[key], b"dictionary = hachidori\n")

    def test_never_overwrite_an_unreadable_config(self):
        ftp = FakeFTP()
        with self.assertRaises(ftplib.error_perm):
            install.enable(ftp, ["main.vjdict"])
        self.assertEqual(ftp.files, {})


if __name__ == "__main__":
    unittest.main()
