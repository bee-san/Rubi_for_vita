"""Run the installer's real config migration on configs saved by older releases."""
import contextlib
import difflib
import importlib.util
import io
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "host/tests/fixtures"
TEMPLATE = ROOT / "docs/config.example.ini"


def unescape(record):
    """A record of host/tests/fixtures/config-lines.txt as bytes: its \\n, \\r, \\t, \\\\ and \\xHH unescaped, as
    host/tests/config_golden.h reads it."""
    out, i = bytearray(), 0
    raw = record.encode("utf-8")
    while i < len(raw):
        ch = raw[i:i + 1]
        if ch == b"\\" and i + 1 < len(raw):
            x = raw[i + 1:i + 2]
            if x == b"x" and i + 3 < len(raw):
                out += bytes([int(raw[i + 2:i + 4], 16)])
                i += 4
                continue
            out += {b"n": b"\n", b"r": b"\r", b"t": b"\t"}.get(x, x)
            i += 2
            continue
        out += ch
        i += 1
    return bytes(out)


def escape(data):
    """Bytes as one printable line, as host/tests/config_golden.h golden_escape prints them (a final space too)."""
    names = {0x0A: "\\n", 0x0D: "\\r", 0x09: "\\t", 0x5C: "\\\\"}
    text = "".join(names.get(b) or (chr(b) if 0x20 <= b < 0x7F else "\\x%02X" % b) for b in data)
    return text[:-1] + "\\x20" if text.endswith(" ") else text  # no trailing space in a golden file


def fnv1a(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def escape_short(data):
    """escape, but data longer than 240 bytes as its first 80, its length and its hash (golden_escape_short)."""
    if len(data) <= 240:
        return escape(data)
    return "%s...[%d bytes, fnv1a %016x]" % (escape(data[:80]), len(data), fnv1a(data))


def records():
    """The records of host/tests/fixtures/config-lines.txt (empty lines and // comments are none)."""
    for line in (FIXTURES / "config-lines.txt").read_text(encoding="utf-8").split("\n"):
        if line and not line.startswith("//"):
            yield unescape(line)


def long_records():
    """host/tests/config_golden.h golden_long_records' records."""
    longer = (("jpdb_api_key = ", "S", 1500), ("font_size_ja = ", "9", 1500), ("frequency_filter = ", "x", 600),
              ("anki_audio_url = https://", "x", 1500), ("dictionary = ", "x", 600), ("ocr_backend = ", "x", 600),
              ("log_host = ", "x", 600), ("vocr_model = ", "x", 600), ("bogus = ", "x", 1500),
              ("local_dictionaries = ", "a", 510), ("local_dictionaries = ", "a", 511),
              ("local_dictionaries = ", "a", 512), ("local_dictionary_dir = ux0:", "d", 155),
              ("local_dictionary_dir = ux0:", "d", 156), ("local_dictionary_dir = ux0:", "d", 157),
              ("ocr_model_dir = ux0:", "d", 156), ("", "k", 63), ("", "k", 64), ("", "k", 70), ("", "k", 1100))
    for head, fill, n in longer:
        yield (head + fill * n + ("" if head else " = 1")).encode("ascii")


def load_migrate():
    spec = importlib.util.spec_from_file_location("migrate_config_golden", ROOT / "tools/migrate_config.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def migrate_in_process(module, folder, old_bytes, *sets):
    """main() as the script runs it (exit 2 on any exception, as its __main__ does): (status, new text, report)."""
    old, out = Path(folder) / "old.ini", Path(folder) / "new.ini"
    for p in (old, out):
        if p.exists():
            p.unlink()
    if old_bytes is not None:
        old.write_bytes(old_bytes)
    report = io.StringIO()
    with mock.patch.object(sys, "argv", ["migrate_config.py", str(old), str(TEMPLATE), str(out), *sets]), \
            contextlib.redirect_stdout(report), contextlib.redirect_stderr(io.StringIO()):
        try:
            status = module.main()
        except Exception:  # pylint: disable=broad-except
            status = 2
    return status, out.read_bytes() if out.exists() else None, report.getvalue()


def golden_migration(module, folder, old_bytes, lines, first=None, sets=()):
    """One migration in the golden file: its exit status, the template's lines it changed (- and +, each hunk's line
    numbers on its @@ line) and its report, or (first: a first install's report lines) only the lines its report has
    that a first install's has not (+) and the other way round (-): a first install reports every setting it writes.
    sets: key=value overrides, as install_ftp.sh passes them. Returns the report's lines."""
    status, new, report = migrate_in_process(module, folder, old_bytes, *sets)
    lines.append("  exit %d" % status)
    if new is not None:
        before = TEMPLATE.read_bytes().split(b"\n")
        diff = list(difflib.diff_bytes(difflib.unified_diff, before, new.split(b"\n"), lineterm=b"", n=0))
        for d in diff[2:]:  # after the two file headers
            if d.startswith(b"@@"):
                lines.append("  " + d.decode("ascii"))
            else:
                lines.append(("  %s %s" % (d[:1].decode("ascii"), escape_short(d[1:]))).rstrip(" "))
    said = report.splitlines()
    if first is None:
        lines.extend("  report: " + escape_short(line.encode("utf-8")) for line in said)
    else:
        lines.extend("  report+ " + escape_short(line.encode("utf-8")) for line in said if line not in first)
        lines.extend("  report- " + escape_short(line.encode("utf-8")) for line in first if line not in said)
    return said


class MigrateConfig(unittest.TestCase):
    def test_golden(self):
        """#418 (cleanup package P2): every migration of the fixtures, a missing file and each record of
        host/tests/fixtures/config-lines.txt, against host/tests/fixtures/config-migrate.golden: a change to the
        migration shows as that file's diff (VJO_UPDATE_GOLDEN=1 rewrites it)."""
        module = load_migrate()
        lines = ["# tools/migrate_config.py on the config fixtures against docs/config.example.ini "
                 "(tools/tests/test_migrate_config.py test_golden; VJO_UPDATE_GOLDEN=1 rewrites this file)"]
        with tempfile.TemporaryDirectory() as folder:
            lines.append("= no config.ini (a first install)")
            first = golden_migration(module, folder, None, lines)
            for name in ("host/tests/fixtures/config-old-app-test2.ini", "host/tests/fixtures/config-test3-owner.ini",
                         "docs/config.example.ini"):
                lines.append("= " + name)
                golden_migration(module, folder, (ROOT / name).read_bytes(), lines, first)
            count = 0
            for record in list(records()) + list(long_records()):
                lines.append("> " + escape_short(record))
                golden_migration(module, folder, record, lines, first)
                count += 1
            # key=value overrides (install_ftp.sh --set), over a first install and over a file (P2's audit F4)
            for old, sets in ((None, ("log_file=on",)), (None, ("vocr_tier=large", "log_file=off")),
                              (b"vocr_tier = small\n", ("vocr_tier=huge",)), (b"", ("vocr_model=FL10_w8.vocr",)),
                              (b"vocr_tier = large\n", ("vocr_model=H15_w8.vocr", "vocr_fallback_model=none")),
                              (b"", ("vocr_model=FL10_w8.vocr", "vocr_tier=large")), (b"", ("dictionary=jpdb",)),
                              (b"", ("dictionary=Hachidori", "hachidori_host=10.0.0.1")),
                              (b"", ("jpdb_api_key=secret-50", "API_KEY=secret-51", "jiten_api_keyx=secret-52")),
                              (b"", ("anki_host=auto", "anki_audio_url=https://a.invalid/?k=secret-53",
                                     "anki_field_word=Word")),
                              (b"", ("ocr_backend=lens", "log_host=10.0.0.1", "frequency_filter=3", "font_size=20")),
                              (b"", ("my_debug_switch=1",)), (b"my_debug_switch = 2\n", ()),
                              (b"", ("Font_Size_JA=20", " log_file = off ")), (b"", ("local_dictionaries=mine.vjdict",)),
                              (b"local_dictionaries = plaintext.vjdict\nlocal_dictionary_results = 1\n",
                               ("local_dictionary_results=1",))):
                lines.append("> %s --set %s" % ("(none)" if old is None else escape_short(old) or "(empty)",
                                                 " ".join(escape(v.encode("utf-8")) for v in sets) or "(nothing)"))
                golden_migration(module, folder, old, lines, first, sets)
        self.assertGreater(count, 300)
        text = "\n".join(lines) + "\n"
        golden = FIXTURES / "config-migrate.golden"
        if os.environ.get("VJO_UPDATE_GOLDEN"):
            golden.write_text(text, encoding="utf-8")
            return
        want = golden.read_text(encoding="utf-8")
        if want != text:
            diff = list(difflib.unified_diff(want.splitlines(), text.splitlines(), "golden", "now", lineterm="", n=1))
            self.fail("%s differs (VJO_UPDATE_GOLDEN=1 rewrites it):\n%s" % (golden, "\n".join(diff[:40])))

    def migrate(self, old_text, *sets):
        with tempfile.TemporaryDirectory() as folder:
            old = Path(folder) / "old.ini"
            out = Path(folder) / "new.ini"
            old.write_text(old_text, encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(ROOT / "tools/migrate_config.py"), str(old),
                 str(ROOT / "docs/config.example.ini"), str(out), *sets],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return out.read_text(encoding="utf-8"), result.stdout

    def test_a_bom_led_copy_is_the_setting_the_installer_sets(self):
        """audit-pr185 B2: core/config.c reads a line behind a mid-file BOM (a hand-joined file) as its setting, so the
        installer does too: a BOM-led copy of a key it sets is that key, replaced, not kept as an extra line that the
        plugins would read last; a BOM-led API key goes, by name."""
        bom = "\ufeff"
        cases = (
            ("log_file = on\nanki_mining = on\n" + bom + "log_file = off\n", ("log_file=on",), "log_file", "on"),  # M1
            ("local_dictionary_dir = ux0:data/VitaJPOverlay/dictionaries\n" + bom + "local_dictionary_dir = ux0:Other\n",
             ("local_dictionary_dir=ux0:data/VitaJPOverlay/dictionaries",), "local_dictionary_dir",
             "ux0:data/VitaJPOverlay/dictionaries"),  # M2
            ("anki_mining = on\n" + bom + "ocr_model_dir = ux0:models\n", ("ocr_model_dir=ux0:data/VitaJPOverlay/ocr",),
             "ocr_model_dir", "ux0:data/VitaJPOverlay/ocr"),  # M5
            (" " + bom + "\tlog_file = off\n", ("log_file=on",), "log_file", "on"),  # a space before the BOM, a tab after
            # audit-pr185-b N1 (P3, P4): spaces and BOMs in any order, and several BOMs
            (" " + bom + " " + bom + "ocr_model_dir = ux0:models\n", ("ocr_model_dir=ux0:data/VitaJPOverlay/ocr",),
             "ocr_model_dir", "ux0:data/VitaJPOverlay/ocr"),  # S3
            (bom + bom + "log_file = off\n", ("log_file=on",), "log_file", "on"),  # S4
        )
        for old, sets, key, want in cases:
            with self.subTest(key=key, old=old):
                text, report = self.migrate(old, *sets)
                self.assertNotIn(bom, text)
                lines = [l for l in text.splitlines() if l.split("=")[0].strip() == key]
                self.assertEqual(lines, [f"{key} = {want}"], text)
        text, report = self.migrate("anki_mining = on\n" + bom + "jpdb_api_key = SECRETm4\n")  # M4
        self.assertNotIn("SECRETm4", text + report)
        self.assertNotIn("jpdb_api_key", text)
        self.assertIn("config.ini: jpdb_api_key removed", report)

    def test_removed_meiki_backend_goes_and_keeps_other_values(self):
        text, report = self.migrate("ocr_backend = Meiki\nocr_mode = on_press\n"
                                    "vocr_model = H15_w8.vocr\njpdb_api_key = secret-key\n")
        self.assertNotIn("ocr_backend =", text)
        self.assertNotIn("meiki", text.lower())
        self.assertIn("ocr_mode = on_press\n", text)
        self.assertIn("vocr_tier = normal\n", text)  # H15: Normal
        self.assertNotIn("vocr_model", text)
        self.assertEqual(report.count("config.ini: ocr_backend removed\n"), 1)
        # jpdb.io was removed: its API key goes, named, never its value
        self.assertNotIn("secret-key", text + report)
        self.assertIn("config.ini: jpdb_api_key removed", report)

    def test_removed_online_dictionaries_become_local_and_their_keys_go(self):
        # jpdb.io and jiten.moe (online, with an API key) were removed: the dictionary becomes
        # local and every key line is dropped (api_key: jpdb_api_key's older name), said by name
        # only; the template keeps no key either.
        for old in ("jpdb", "JPDB", "jiten", "Jiten"):
            text, report = self.migrate("dictionary = %s\njpdb_api_key = secret-1\njiten_api_key = secret-2 ; mine\n"
                                        "api_key = secret-3\nocr_mode = on_press\n" % old)
            self.assertIn("\ndictionary = local\n", text, old)
            self.assertEqual(text.count("dictionary ="), 1)
            self.assertIn("ocr_mode = on_press\n", text)
            self.assertNotIn("secret", text + report)
            self.assertNotIn("api_key", text)
            self.assertNotIn("jpdb", text.lower())
            self.assertNotIn("jiten", text.lower())
            self.assertIn("config.ini: dictionary = local", report)
            for key in ("jpdb_api_key", "jiten_api_key", "api_key"):
                self.assertIn("config.ini: %s removed" % key, report)

    def test_a_mistyped_key_line_is_dropped_and_never_printed(self):
        # a key glued to its name, or text after it that the parser reads as the key's name ("...; x = y"):
        # a line that starts with a removed key's name goes, and the report names only that name
        text, report = self.migrate("jpdb_api_key secret-1 ; x = y\njpdb_api_keysecret-2 = 1\nJITEN_API_KEY_OLD = secret-3\n"
                                    "api_keys = secret-4\nocr_mode = on_press\n", "jpdb_api_key secret-5=6")
        self.assertIn("ocr_mode = on_press\n", text)
        for secret in ("secret-1", "secret-2", "secret-3", "secret-4", "secret-5"):
            self.assertNotIn(secret, text + report)
        self.assertNotIn("api_key", text)
        for line in ("config.ini: jpdb_api_key removed", "config.ini: jiten_api_key removed",
                     "config.ini: api_key removed", "config.ini: jpdb_api_key is no longer a setting, not set"):
            self.assertEqual(report.count(line + "\n"), 1, line)

    def test_an_update_enables_the_bundled_names(self):
        # The owner's 0.9.4 config, saved before the names: they are added after the user's own and the count
        # of 1 (the old default) becomes 2, so a word that is also a name pages to it.
        text, report = self.migrate("local_dictionaries = plaintext.vjdict\nlocal_dictionary_results = 1\n")
        self.assertIn("\nlocal_dictionaries = plaintext.vjdict,vndb-names.vjdict\n", text)
        self.assertIn("\nlocal_dictionary_results = 2\n", text)
        self.assertIn("config.ini: local_dictionary_results = 2", report)
        text, _ = self.migrate("local_dictionaries = mine.vjdict, jmdict.vjdict\nlocal_dictionary_results = 3\n")
        self.assertIn("\nlocal_dictionaries = mine.vjdict,plaintext.vjdict,vndb-names.vjdict\n", text)
        self.assertIn("\nlocal_dictionary_results = 3\n", text)  # a count the user chose stays
        # both there already: nothing changes, a 1 included (chosen with the names enabled)
        text, _ = self.migrate("local_dictionaries = plaintext.vjdict,vndb-names.vjdict\nlocal_dictionary_results = 1\n")
        self.assertIn("\nlocal_dictionary_results = 1\n", text)
        # no room for the names (eight files): the list and the count stay
        eight = ",".join("d%d.vjdict" % i for i in range(7)) + ",plaintext.vjdict"
        text, _ = self.migrate("local_dictionaries = %s\nlocal_dictionary_results = 1\n" % eight)
        self.assertIn("\nlocal_dictionaries = %s\n" % eight, text)
        self.assertIn("\nlocal_dictionary_results = 1\n", text)
        # an explicit --set wins
        text, _ = self.migrate("local_dictionaries = plaintext.vjdict\n", "local_dictionaries=plaintext.vjdict")
        self.assertIn("\nlocal_dictionaries = plaintext.vjdict\n", text)

    def test_the_local_dictionary_is_unchanged(self):
        text, report = self.migrate("dictionary = local\nocr_mode = on_press\n")
        self.assertIn("\ndictionary = local\n", text)
        self.assertNotIn("config.ini: dictionary", report)

    def test_the_removed_relay_becomes_local_and_its_host_goes(self):
        # The Hachidori relay on a computer was removed: dictionary = hachidori becomes local and
        # hachidori_host, its computer, is dropped (core/config.c ignores it); the template has neither.
        for old in ("hachidori", "Hachidori", "HACHIDORI"):
            text, report = self.migrate("dictionary = %s\nhachidori_host = 192.168.1.20:19633 ; LAN\n"
                                        "ocr_mode = on_press\n" % old)
            self.assertIn("\ndictionary = local\n", text, old)
            self.assertEqual(text.count("dictionary ="), 1)
            self.assertIn("ocr_mode = on_press\n", text)
            self.assertNotIn("achidori", text)
            self.assertNotIn("19633", text)
            self.assertIn("config.ini: dictionary = local", report)
            self.assertIn("config.ini: hachidori_host removed", report)
        # tools/install_ftp.sh --set: the relay is replaced, its host not set
        text, report = self.migrate("ocr_mode = auto\n", "dictionary=hachidori", "hachidori_host=192.168.1.20")
        self.assertIn("\ndictionary = local\n", text)
        self.assertNotIn("achidori", text)
        self.assertIn("config.ini: hachidori_host is no longer a setting, not set", report)

    def test_an_explicit_removed_setting_is_not_set(self):
        # tools/install_ftp.sh --set: a removed online dictionary is replaced, its key not set
        text, report = self.migrate("ocr_mode = auto\n", "dictionary=jiten", "jiten_api_key=secret-9")
        self.assertIn("\ndictionary = local\n", text)
        self.assertNotIn("secret-9", text + report)
        self.assertIn("config.ini: jiten_api_key is no longer a setting, not set", report)

    def test_template_has_no_online_dictionary_or_relay(self):
        template = (ROOT / "docs/config.example.ini").read_text(encoding="utf-8")
        self.assertNotIn("jpdb", template)
        self.assertNotIn("jiten", template)
        self.assertNotIn("api_key", template)
        self.assertNotIn("achidori", template)
        self.assertNotIn("19633", template)
        self.assertIn("\ndictionary = local\n", template)

    def test_the_engine_choice_goes_whatever_its_value(self):
        # VOCR is the only engine: Google Lens was removed (the overlay is offline), as were the
        # VOCR2 trial (v0.7.12 to v0.8.0-test.1), Meiki and ncnn (host tools). The line goes, said
        # once; every other value stays.
        for old in ("vocr", "VOCR", "lens", "Lens", "vocr2", "VOCR2", "ncnn", "meiki", "other"):
            text, report = self.migrate("ocr_backend = %s\nvocr_model = H15_w8.vocr\nocr_mode = on_press\n" % old)
            self.assertNotIn("ocr_backend", text, old)
            self.assertIn("vocr_tier = normal\n", text)
            self.assertIn("ocr_mode = on_press\n", text)
            self.assertEqual(report.count("config.ini: ocr_backend removed\n"), 1, report)
            self.assertNotIn("config.ini: ocr_backend =", report)

    def test_the_template_names_no_engine_setting(self):
        template = (ROOT / "docs/config.example.ini").read_text(encoding="utf-8")
        self.assertNotIn("\nocr_backend", template)
        self.assertNotIn("lens", template.lower())
        text, report = self.migrate("ocr_mode = auto\n")
        self.assertNotIn("ocr_backend", text + report)

    def test_the_clock_is_on_unless_set_off(self):
        # ocr_boost_clock = on is the default (the owner, 2026-10-15): a config that never set it gains it, an
        # explicit off (or on) is kept.
        text, report = self.migrate("ocr_backend = vocr\nocr_mode = on_press\n")
        self.assertIn("\nocr_boost_clock = on\n", text)
        self.assertIn("config.ini: ocr_boost_clock = on", report)
        for value in ("off", "on"):
            text, _ = self.migrate("ocr_backend = vocr\nocr_boost_clock = %s\n" % value)
            self.assertIn("\nocr_boost_clock = %s\n" % value, text)
            self.assertEqual(text.count("ocr_boost_clock ="), 1)

    def test_japanese_characters_only_is_off_unless_set_on(self):
        # japanese_only_selection = off is the default (the owner, 2026-10-20; on before): a config that never set it
        # gains off; an explicit on (the template's line from 0.7.6 until then) or off is kept as it is.
        text, report = self.migrate("ocr_backend = vocr\nhighlight_mode = character\n")
        self.assertIn("\njapanese_only_selection = off\n", text)
        self.assertIn("config.ini: japanese_only_selection = off", report)
        for value in ("on", "off"):
            text, report = self.migrate("ocr_backend = vocr\njapanese_only_selection = %s\n" % value)
            self.assertIn("\njapanese_only_selection = %s\n" % value, text)
            self.assertEqual(text.count("japanese_only_selection ="), 1)
            self.assertNotIn("japanese_only_selection", report)  # unchanged, so not reported

    def test_the_press_wait_cap_is_added_and_kept(self):
        # ocr_settle_press_ms (docs/first-ocr.md): a config from before it gains the default, a value is kept.
        text, report = self.migrate("ocr_backend = vocr\nocr_settle_max_ms = 3000\n")
        self.assertIn("\nocr_settle_press_ms = 1000\n", text)
        self.assertIn("\nocr_settle_max_ms = 3000\n", text)
        self.assertIn("config.ini: ocr_settle_press_ms = 1000", report)
        for value in ("0", "400"):
            text, _ = self.migrate("ocr_backend = vocr\nocr_settle_press_ms = %s\n" % value)
            self.assertIn("\nocr_settle_press_ms = %s\n" % value, text)
            self.assertEqual(text.count("ocr_settle_press_ms ="), 1)

    def test_the_sticks_rest_is_added_and_kept(self):
        # stick_lookup_ms (#138, docs/touch-lookup.md): a config from before it gains the default, a value is kept,
        # also a --set one.
        text, report = self.migrate("highlight_mode = character\n")
        self.assertIn("\nstick_lookup_ms = 200\n", text)
        self.assertIn("\nhighlight_mode = character\n", text)
        self.assertIn("config.ini: stick_lookup_ms = 200", report)
        for value in ("0", "150", "250"):
            text, _ = self.migrate("stick_lookup_ms = %s\n" % value)
            self.assertIn("\nstick_lookup_ms = %s\n" % value, text)
            self.assertEqual(text.count("stick_lookup_ms ="), 1)
        text, _ = self.migrate("stick_lookup_ms = 150\n", "stick_lookup_ms=250")
        self.assertIn("\nstick_lookup_ms = 250\n", text)

    def test_an_explicit_engine_is_not_set(self):
        # an older installer's --set ocr_backend=...: no longer a setting, said once, not written
        for value in ("vocr", "lens"):
            text, report = self.migrate("ocr_backend = meiki\n", "ocr_backend=" + value)
            self.assertNotIn("ocr_backend", text, value)
            self.assertEqual(report.count("config.ini: ocr_backend is no longer a setting, not set\n"), 1, report)
            self.assertEqual(report.count("config.ini: ocr_backend removed\n"), 1, report)

    def test_the_udp_log_host_goes_with_its_reason(self):
        # The UDP log went with the network (the overlay is offline): log_host's line goes, set or
        # empty (the old template wrote it empty), said once with why; log_file stays.
        for old in ("log_host = 192.168.1.5\n", "log_host =\n", "LOG_HOST = 10.0.0.1 ; pc\n"):
            text, report = self.migrate(old + "log_file = off\nocr_mode = on_press\n")
            self.assertNotIn("log_host", text.lower(), old)
            self.assertIn("\nlog_file = off\n", text)
            self.assertIn("\nocr_mode = on_press\n", text)
            self.assertEqual(report.count("config.ini: log_host removed (the UDP log was removed: the overlay is "
                                          "offline; log.txt stays)\n"), 1, report)
        # an older installer's --set log_host=...: no longer a setting, said once, not written
        text, report = self.migrate("ocr_mode = auto\n", "log_host=192.168.1.5")
        self.assertNotIn("log_host", text)
        self.assertEqual(report.count("config.ini: log_host is no longer a setting, not set\n"), 1, report)
        template = (ROOT / "docs/config.example.ini").read_text(encoding="utf-8")
        self.assertNotIn("log_host", template)
        self.assertNotIn("udp_log_listener", template)

    def test_older_model_choices_become_their_model_size(self):
        # API 21: vocr_model and vocr_fallback_model became one model size, vocr_tier. Each older
        # model by its family: FL10 (v0.7.12's installer wrote FL10_w8.vocr, the VOCR2 trial was
        # FL10-sized) Small, H15 Normal, F20 and W20 Large; both lines go.
        for model, tier in (("FL10_w8.vocr", "small"), ("VOCR3_FL10_w8.vocr", "small"),
                            ("VOCR2_FL10_w8.vocr", "small"), ("H15_w8.vocr", "normal"),
                            ("VOCR3_H15_w8.vocr", "normal"), ("F20_w8.vocr", "large"),
                            ("VOCR4_W20_w8.vocr", "large")):
            text, report = self.migrate("ocr_backend = vocr\nvocr_model = %s\nvocr_fallback_model = none\n" % model)
            self.assertIn("\nvocr_tier = %s\n" % tier, text, model)
            self.assertEqual(text.count("vocr_tier ="), 1)
            self.assertNotIn("vocr_model", text)
            self.assertNotIn("vocr_fallback_model", text)
            self.assertIn("config.ini: vocr_model removed", report)
            self.assertIn("config.ini: vocr_fallback_model removed", report)
            self.assertIn("config.ini: vocr_tier = " + tier, report)
        # an unknown name keeps the template's size, Normal
        text, _ = self.migrate("vocr_model = VOCR9_X_w8.vocr\n")
        self.assertIn("\nvocr_tier = normal\n", text)
        self.assertNotIn("vocr_model", text)
        # a file that names its size keeps it, whatever an older line says
        text, _ = self.migrate("vocr_tier = large\nvocr_model = FL10_w8.vocr\n")
        self.assertIn("\nvocr_tier = large\n", text)
        text, report = self.migrate("vocr_tier = small\n")
        self.assertIn("\nvocr_tier = small\n", text)
        self.assertNotIn("vocr_tier", report)  # unchanged
        # a fresh install: the template's Normal, no model line
        text, _ = self.migrate("")
        self.assertIn("\nvocr_tier = normal\n", text)
        self.assertNotIn("vocr_model", text)

    def test_an_older_installers_model_settings_set_the_size(self):
        # tools/install_ftp.sh --set vocr_model=... of an older checkout: its size, explicitly
        text, _ = self.migrate("vocr_tier = normal\n", "vocr_model=FL10_w8.vocr", "vocr_fallback_model=none")
        self.assertIn("\nvocr_tier = small\n", text)
        self.assertNotIn("vocr_model", text)
        self.assertNotIn("vocr_fallback_model", text)
        # a vocr_tier setting wins over a vocr_model one
        text, _ = self.migrate("", "vocr_model=FL10_w8.vocr", "vocr_tier=large")
        self.assertIn("\nvocr_tier = large\n", text)
        self.assertEqual(text.count("vocr_tier ="), 1)

    def test_an_unreadable_model_size_is_ignored_as_core_config_does(self):
        """core/config.c ignores a vocr_tier it cannot read (a warning) and reads vocr_model: the
        installed file names the size the plugin would run, in either order of the lines."""
        for old in ("vocr_tier = huge\nvocr_model = W20_w8.vocr\n", "vocr_model = W20_w8.vocr\nvocr_tier = huge\n"):
            text, report = self.migrate(old)
            self.assertIn("\nvocr_tier = large\n", text, old)
            self.assertNotIn("huge", text)
            self.assertNotIn("vocr_model", text)
            self.assertIn("config.ini: vocr_tier = large", report)
        # without a model line: the template's size, the plugin's default
        text, _ = self.migrate("vocr_tier = huge\n")
        self.assertIn("\nvocr_tier = normal\n", text)
        self.assertNotIn("huge", text)
        # a readable size wins over an older model line, in any case
        text, _ = self.migrate("vocr_model = FL10_w8.vocr\nvocr_tier = Large\n")
        self.assertIn("\nvocr_tier = Large\n", text)
        # a model name core/config.c refuses (valid_vocr_model) gives no size
        for name in ("FL10", "FL10_w8.bin", "dir/FL10_w8.vocr", "x" * 60 + "FL10.vocr"):
            text, _ = self.migrate("vocr_model = %s\n" % name)
            self.assertIn("\nvocr_tier = normal\n", text, name)
        # an unreadable --set vocr_tier is not written, and does not win over a --set vocr_model
        text, report = self.migrate("vocr_tier = large\n", "vocr_tier=huge", "vocr_model=FL10_w8.vocr")
        self.assertIn("\nvocr_tier = small\n", text)
        self.assertNotIn("huge", text)
        self.assertIn("vocr_tier huge is not a model size", report)
        text, _ = self.migrate("vocr_tier = large\n", "vocr_tier=")
        self.assertIn("\nvocr_tier = large\n", text)

    def test_release_installer_keeps_the_chosen_model_size(self):
        # The Rubi app's Model size is the user's: the installer sets no size and no model, so an
        # update keeps the choice; a first install gets the template's Normal.
        script = (ROOT / "tools/install-vita.sh").read_text()
        self.assertNotIn("vocr_model", script)
        self.assertNotIn("vocr_fallback_model", script)
        self.assertNotIn("--set vocr_tier", script)
        self.assertIn("--set log_file=on", script)

    def test_ankiconnect_settings_are_removed_and_mining_is_kept(self):
        # AnkiConnect is gone (the Rubi app exports .apkg files): its address and the word
        # audio source go, each with a line saying why; anki_mining stays as it was.
        text, out = self.migrate("anki_host = 192.168.1.23\nanki_audio_url = http://h/?t={term};k\n")
        self.assertIn("anki_mining = off\n", text)
        self.assertNotIn("anki_host", text)
        self.assertNotIn("anki_audio_url", text)
        self.assertIn("config.ini: anki_host removed (AnkiConnect was removed: the Rubi app exports the cards "
                      "as an .apkg)", out)
        self.assertIn("config.ini: anki_audio_url removed (AnkiConnect was removed", out)
        text, out = self.migrate("anki_mining = on\nANKI_HOST = auto\n")
        self.assertIn("anki_mining = on\n", text)
        self.assertEqual(text.count("anki_mining"), 1)
        self.assertNotIn("anki_host", text.lower())

    def test_a_credential_is_reported_only_by_name(self):
        # no credential is a setting any more: the audio source's URL, which can carry a key
        # (audit-overlay-test3-cleanup B1), went with AnkiConnect and the API keys with the online
        # dictionaries; an old one or a --set one is dropped and named, never its value
        url = "https://audio.invalid/a?term={term}&key=url-secret-99"
        text, report = self.migrate(f"jpdb_api_key = old-key-1\nanki_audio_url = {url}\n", "jiten_api_key=new-key-2")
        self.assertNotIn("api_key", text)
        self.assertNotIn("anki_audio_url", text)
        self.assertIn("config.ini: anki_audio_url removed (AnkiConnect was removed", report)
        self.assertIn("config.ini: jpdb_api_key removed", report)
        self.assertIn("config.ini: jiten_api_key is no longer a setting, not set", report)
        for secret in ("url-secret-99", "audio.invalid", "new-key-2", "old-key-1"):
            self.assertNotIn(secret, report)
            self.assertNotIn(secret, text)

    def test_the_field_mapping_of_earlier_builds_is_removed(self):
        # One fixed note type (Lapis): no deck, note type, tags or fields to set.
        old = ("anki_host = auto\nanki_deck = Mining #1 ; main\nanki_note_type = Lapis\nanki_tags = a b\n"
               + "".join("anki_field_%s = X\n" % f for f in (
                   "word", "reading", "furigana", "definition", "sentence", "picture", "frequency", "audio",
                   "sentence_furigana", "pitch_position", "pitch_categories", "frequency_list", "misc_info"))
               + "anki_field_wrod = Word\n")
        text, out = self.migrate(old)
        self.assertNotIn("anki_deck", text)
        self.assertNotIn("anki_note_type", text)
        self.assertNotIn("anki_tags", text)
        self.assertNotIn("anki_field_", text)
        self.assertNotIn("anki_host", text)
        self.assertIn("config.ini: anki_deck removed", out)
        self.assertIn("config.ini: anki_field_misc_info removed", out)
        self.assertIn("config.ini: anki_field_wrod removed", out)

if __name__ == "__main__":
    unittest.main()
