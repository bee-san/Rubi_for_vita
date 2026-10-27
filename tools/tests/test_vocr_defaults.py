"""The device default is the Normal model size, VOCR5_N_w8g2.vocr from bee-san/vita-vn-ocr v0.3.6-head2level.1
(v0.3.0's recognizer with its head gate and two-level head), with Small (VOCR5_S_w8g.vocr) and Large
(VOCR5_L_w8g2.vocr) the other sizes (tools/model_tiers.json;
tools/tests/test_model_tiers.py checks the C tiers against it); VOCR3's, VOCR4-W20 and v0.2.0's
models stay pinned (host tools) but are not shipped; every pinned model fits the Large size's
ceilings and the game worker's weights block (VJO_VOCR_MAX_BYTES, the largest pin); and the game
worker's, the kernel's, the model metadata's (core/vocr_models.c, with each model's generation) and
the Python pins agree. Each C table is read entry by entry, with a macro or a literal size; an entry
the parsers cannot read fails."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import prepare_ocr  # noqa: E402

# The sha256s in the v0.2.0 release notes and in v0.2.5-vocr3.1's SHA256SUMS (the VOCR3_H15 and
# VOCR3_FL10 arms of vita-vn-ocr eval/vocr3_report_spec_20261013.json).
H15_V020_SHA256 = "316ebc377ec34520440991c5085199d8e74031f01a867938be90a60c1e3f03cd"
VOCR3_H15_SHA256 = "cb3c61a8f91f9aeeb3efcc0e79313f664112f25c4bd55865bae9df52522b360c"
VOCR3_FL10_SHA256 = "87fb4d0c18416f5f3771cecd274c8db3cf69b8b4a4ee9f204edebb4b82079154"
# The model sizes' pins, the one place a test states them: per tier its file, exact size, SHA-256
# (the release's SHA256SUMS), W8I arena, release and generation. VOCR5's are vita-vn-ocr
# eval/vocr5_protocol.json's "tiers" (exact sizes and arenas), released as v0.3.0, with their head gate since
# API 42 (vita-vn-ocr #117: v0.3.0's bytes + a 1,792 B section; the arena with the gate on, +48 B), released as
# v0.3.5-headgate.1, and Normal's and Large's with their two-level head too (#117 item 2: the output rows in
# frequency order, the gate, a 1,984 B VOC2 trailer; the arena with it on, +1,456 B more), released with Small's
# gated file as v0.3.6-head2level.1; a tier whose VOCR5 candidate did not beat its incumbent keeps that model, the
# VOCR4 one of v0.2.5-vocr4.1.
# The other tests read the tiers from here, tools/model_tiers.json or the C tier macros.
TIER_PINS = {
    "small": ("VOCR5_S_w8g.vocr", 821584, "76a1bc5de5cd0419a7edad31bc198af8016bda5592a7e595de75697671d3e928", 124880,
              "v0.3.6-head2level.1", "VOCR5"),
    "normal": ("VOCR5_N_w8g2.vocr", 1189408, "f0c8e52c9c1d9f2a714d5ca3ecfba9c0b031ee7ac51a522a2c29fd558d51761b", 184448,
               "v0.3.6-head2level.1", "VOCR5"),
    "large": ("VOCR5_L_w8g2.vocr", 2477472, "41c3543ff8dddd45dc953a6a5f5d579ef9ca779f0e43196406be59c1d985c5b5", 262528,
              "v0.3.6-head2level.1", "VOCR5"),
}
TIER_FILES = [TIER_PINS[t][0] for t in ("small", "normal", "large")]


def macros(*more):
    """#define NAME value (number or string) of include/vjo_vocr_sizes.h (and of the files `more`)."""
    text = "\n".join((ROOT / path).read_text() for path in ("include/vjo_vocr_sizes.h", *more))
    out = {m.group(1): int(m.group(2)) for m in re.finditer(r"#define (\w+) (\d+)u", text)}
    out.update({m.group(1): m.group(2) for m in re.finditer(r'#define (\w+) "([^"]+)"', text)})
    for m in re.finditer(r"#define (\w+) (VJO_\w+)\s*$", text, re.M):  # an alias of another macro
        if m.group(2) in out:
            out[m.group(1)] = out[m.group(2)]
    return out


def number(token, sizes):
    """A size or an arena: a macro of include/vjo_vocr_sizes.h (or of the file read), or a decimal
    literal (1155968u)."""
    return sizes[token] if not token.isdigit() else int(token)


def without_comments(source):
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)


def array_body(source, declaration):
    """The initialisers of the C array declared by `declaration`, between its outer braces."""
    body = source[source.index(declaration):]
    body = body[body.index("{") + 1:]
    return without_comments(body[:body.index("\n};")])


def entries(body):
    """How many initialisers an array body holds, counted by brace depth: every entry, however it is
    written, so that one the patterns below do not read is noticed."""
    depth = count = 0
    for ch in body:
        if ch == "{":
            count += depth == 0
            depth += 1
        elif ch == "}":
            depth -= 1
    return count


def every_pin(where, pins, expected):
    """name -> value of the pins read from `where`, which must be all `expected` of its entries, each
    name once: a pin in a form the pattern misses fails here instead of escaping the comparisons."""
    names = [name for name, _ in pins]
    if len(pins) != expected or len(set(names)) != len(names):
        raise AssertionError(f"{where}: read {len(pins)} pins with {len(set(names))} names of {expected} entries")
    return dict(pins)


SIZE = r"(VJO_\w+|\d+)[uU]?"  # a macro or a decimal literal, as either table may spell it
ESTIMATE = r"([A-Z][A-Z0-9_]+|\d+)[uU]?"  # an estimate: a macro of core/vocr_models.c, or a literal


def c_table(source=None):
    """name -> (bytes, sha256 hex, arena) from core/vocr_ocr.c's release table."""
    if source is None:
        source = (ROOT / "core/vocr_ocr.c").read_text()
    sizes = macros()
    body = array_body(source, "#else\nstatic const VjoVocrModel models[]")
    pins = []
    # the finder: a runtime mode, or core/vocr_ocr.c's VJO_VOCR_FINDER (#194: every pinned model's of rows),
    # or its VJO_VOCR_COLUMNS (#99: the vertical model's column finder)
    for m in re.finditer(r'\{"?([\w.]+)"?, ' + SIZE + r',\s*\{([^}]*)\},\s*' + SIZE +
                         r', (?:VOCR_LINES_\w+|VJO_VOCR_FINDER|VJO_VOCR_COLUMNS)\}',
                         body):
        name = sizes.get(m.group(1), m.group(1))  # a name macro (VJO_VOCR3_H15_NAME) or a literal
        digest = "".join("%02x" % int(b, 16) for b in re.findall(r"0x([0-9a-f]{2})", m.group(3)))
        pins.append((name, (number(m.group(2), sizes), digest, number(m.group(4), sizes))))
    return every_pin("core/vocr_ocr.c models[]", pins, entries(body))


def metadata_table(source=None, estimates=False):
    """name -> (generation, shape, bytes) from core/vocr_models.c, the Rubi app's model metadata;
    with estimates, name -> (generation, shape, bytes, box ms, job bytes)."""
    if source is None:
        source = (ROOT / "core/vocr_models.c").read_text()
    sizes = macros()
    sizes.update({m.group(1): int(m.group(2)) for m in re.finditer(r"#define (\w+) (\d+)u", source)})
    body = array_body(source, "#else\nstatic const VjoVocrModelInfo infos[]")
    pins = [(sizes.get(m.group(1), m.group(1)),
             (m.group(2), m.group(3), number(m.group(4), sizes))
             + ((number(m.group(5), sizes), number(m.group(6), sizes)) if estimates else ()))
            for m in re.finditer(r'\{"?([\w.]+)"?, "(\w+)", "(\w+)", ' + SIZE + ", " + ESTIMATE + ", " + ESTIMATE
                                 + r'\}', body)]
    return every_pin("core/vocr_models.c infos[]", pins, entries(body))


def kernel_names(source=None):
    """name -> bytes the kernel's model broker accepts (kernel/game_ocr.c model_size). Every return but
    the last (return 0) and every name comparison there must be one of the pins read."""
    if source is None:
        source = (ROOT / "kernel/game_ocr.c").read_text()
    body = source[source.index("static uint32_t model_size(const char *name)"):]
    body = without_comments(body[:body.index("\n}\n")])
    sizes = macros()
    pins = [(sizes.get(n, n.strip('"')), number(b, sizes))
            for n, b in re.findall(r'!strcmp\(name, ("[\w.]+"|VJO_\w+)\)\) return ' + SIZE + ';', body)]
    if not re.search(r"\breturn 0;\s*$", body) or body.count("strcmp(") != len(pins):
        raise AssertionError(f"kernel/game_ocr.c model_size: {body.count('strcmp(')} comparisons, "
                             f"{len(pins)} pins read, last return {body.split('return')[-1].strip()!r}")
    return every_pin("kernel/game_ocr.c model_size", pins, len(re.findall(r"\breturn\b", body)) - 1)


def python_pins():
    """name -> (url, bytes, sha256, release tag) of every release's asset table in tools/prepare_ocr.py:
    VOCR_ASSETS (VOCR_TAG), VOCR3_ASSETS (VOCR3_TAG) and any later VOCRn_ASSETS (VOCRn_TAG), the vertical
    model's VOCR5V_ASSETS (VOCR5V_TAG) too, each name in one table only."""
    out = {}
    for key, assets in sorted(vars(prepare_ocr).items()):
        release = re.fullmatch(r"(VOCR\d*V?)_ASSETS", key)
        if not release:
            continue
        tag = getattr(prepare_ocr, release.group(1) + "_TAG")
        for name, (url, size, sha) in assets.items():
            if name in out:
                raise AssertionError(f"tools/prepare_ocr.py: {name} is in two release tables")
            out[name] = (url, size, sha, tag)
    return out


def c_lm_table():
    """name -> (bytes, sha256 hex, tier) from core/vocr_ocr.c's LM table."""
    source = (ROOT / "core/vocr_ocr.c").read_text()
    sizes = macros()
    table = source[source.index("static const VjoVocrLm lms[VJO_VOCR_LM_TIERS]"):]
    table = table[:table.index("};")]
    out = {}
    for m in re.finditer(r"\{(VJO_VOCR_LM\d_NAME), (VJO_VOCR_LM\d_BYTES),\s*\{([^}]*)\},\s*(\d+), (\d), (\d+), "
                         r"(VOCR_LM_BOS|VOCR_LM_EOS)\}", table):
        digest = "".join("%02x" % int(b, 16) for b in re.findall(r"0x([0-9a-f]{2})", m.group(3)))
        out[sizes[m.group(1)]] = (sizes[m.group(2)], digest, int(m.group(5)))
    return out


class VocrDefaults(unittest.TestCase):
    def test_cascade_model_is_the_published_vocr4_w20(self):
        """A request by name's cascade (the host tools) is VOCR4-W20, still pinned but no longer shipped;
        a request by tiers reads again with the next size up, so Normal's cascade is the Large size's."""
        self.assertEqual(prepare_ocr.VOCR4_TAG, "v0.2.5-vocr4.1")
        url, size, sha = prepare_ocr.VOCR4_ASSETS[prepare_ocr.VOCR_CASCADE_MODEL]
        self.assertTrue(url.endswith("/releases/download/v0.2.5-vocr4.1/VOCR4_W20_w8.vocr"), url)
        # v0.2.5-vocr4.1's SHA256SUMS; F20's shape
        self.assertEqual((size, sha), (1558016, "2745e69699ad7b2aba3092f83e5ddc1912f405f54b9df8a2e55be82c385b716e"))
        sizes = macros()
        self.assertEqual(sizes["VJO_VOCR_CASCADE_NAME"], prepare_ocr.VOCR_CASCADE_MODEL)
        self.assertEqual(sizes["VJO_VOCR_CASCADE_BYTES"], size)
        self.assertEqual(c_table()[prepare_ocr.VOCR_CASCADE_MODEL][:2], (size, sha))
        import package_release  # noqa: PLC0415
        roles = {name: role for name, _, _, role in package_release.shipped_models()}
        if prepare_ocr.VOCR_CASCADE_MODEL not in TIER_FILES:  # shipped only as a size's model
            self.assertNotIn(prepare_ocr.VOCR_CASCADE_MODEL, roles)
        self.assertTrue(roles[sizes["VJO_VOCR_TIER3_NAME"]].endswith("Large model size"))  # Normal's cascade, if larger
        self.assertIn("\nocr_cascade_ms = 2000\n", (ROOT / "docs/config.example.ini").read_text())

    def test_the_model_sizes_are_their_releases_assets(self):
        """The three sizes' recognizers, pinned by URL, size and SHA-256 in the packager and by size,
        SHA-256 and arena in the game worker, as their release publishes them, and the C tier macros
        name them; the Large size's is the largest pin and the weights blocks' limit (VJO_VOCR_MAX_BYTES).
        VOCR5's release, v0.3.6-head2level.1, holds exactly the VOCR5 sizes' files."""
        self.assertEqual(prepare_ocr.VOCR5_TAG, "v0.3.6-head2level.1")
        self.assertEqual(prepare_ocr.VOCR_GENERATION[prepare_ocr.VOCR5_TAG], "VOCR5")
        table, sizes = c_table(), macros()
        for n, tier in enumerate(("small", "normal", "large"), 1):
            name, size, sha, arena, tag, generation = TIER_PINS[tier]
            assets, got_tag = prepare_ocr.release_assets(name)
            url, got_size, got_sha = assets[name]
            self.assertEqual((got_tag, prepare_ocr.VOCR_GENERATION[got_tag]), (tag, generation), name)
            self.assertEqual(url, f"https://github.com/bee-san/vita-vn-ocr/releases/download/{tag}/{name}")
            self.assertEqual((got_size, got_sha), (size, sha), name)
            self.assertEqual(table[name], (size, sha, arena), name)
            self.assertEqual((sizes[f"VJO_VOCR_TIER{n}_NAME"], sizes[f"VJO_VOCR_TIER{n}_BYTES"]), (name, size), tier)
        self.assertEqual(set(prepare_ocr.VOCR5_ASSETS), {p[0] for p in TIER_PINS.values() if p[4] == "v0.3.6-head2level.1"})
        self.assertEqual(sizes["VJO_VOCR_MAX_BYTES"], max(size for size, _, _ in table.values()))
        self.assertEqual(sizes["VJO_VOCR_MAX_BYTES"], sizes["VJO_VOCR_TIER3_BYTES"])

    def test_lms_and_their_attribution_come_pinned_from_a_local_copy(self):
        import hashlib  # noqa: PLC0415
        import tempfile  # noqa: PLC0415
        from unittest import mock  # noqa: PLC0415
        with tempfile.TemporaryDirectory() as tmp:
            src, out = Path(tmp) / "src", Path(tmp) / "out"
            src.mkdir()
            pins, data = {}, {}
            for name in prepare_ocr.VOCR_LM_ASSETS:
                data[name] = ("fixture " + name).encode()
                pins[name] = ("https://fixture.invalid/" + name, len(data[name]), hashlib.sha256(data[name]).hexdigest())
                (src / name).write_bytes(data[name])
            att = b"CC BY-SA 4.0 fixture"
            (src / prepare_ocr.VOCR_LM_ATTRIBUTION).write_bytes(att)
            att_pin = {prepare_ocr.VOCR_LM_ATTRIBUTION: ("https://fixture.invalid/a", len(att),
                                                         hashlib.sha256(att).hexdigest())}
            with mock.patch.object(prepare_ocr, "VOCR_LM_ASSETS", pins), \
                 mock.patch.object(prepare_ocr, "VOCR_LM_ATTRIBUTION_PIN", att_pin), \
                 mock.patch.object(prepare_ocr.urllib.request, "urlopen", side_effect=AssertionError("no network")):
                paths = prepare_ocr.fetch_lms(out, src)
                self.assertEqual(sorted(p.name for p in paths), sorted([*pins, prepare_ocr.VOCR_LM_ATTRIBUTION]))
                (src / prepare_ocr.VOCR_LM_ATTRIBUTION).write_bytes(b"something else")
                (out / prepare_ocr.VOCR_LM_ATTRIBUTION).unlink()
                with self.assertRaises(RuntimeError):
                    prepare_ocr.fetch_lms(out, src)
        # the real pin is the release's file
        url, size, sha = prepare_ocr.VOCR_LM_ATTRIBUTION_PIN[prepare_ocr.VOCR_LM_ATTRIBUTION]
        self.assertTrue(url.endswith("/releases/download/v0.3.7-lm6.2/ATTRIBUTION.md"))
        self.assertEqual((size, sha), (6514, "9418ed87e6d9fa196aba2315e05a0cf0dca7056482cce03577ced423e5feb1d3"))

    def test_the_model_sizes_attribution_comes_pinned_with_them(self):
        """fetch_tiers (--backend vocr-tiers, CI's package job) fetches v0.3.0's ATTRIBUTION.md with the
        sizes' models and keeps it as MODEL-ATTRIBUTION.md: the LMs' release names its attribution
        ATTRIBUTION.md too, and both land in one models directory. A --from directory may hold it under
        either name, the release's only when that copy is this one."""
        import hashlib  # noqa: PLC0415
        import tempfile  # noqa: PLC0415
        from unittest import mock  # noqa: PLC0415
        with tempfile.TemporaryDirectory() as tmp:
            src, out = Path(tmp) / "src", Path(tmp) / "out"
            src.mkdir()
            pins = {}
            for tier, (name, *_rest) in TIER_PINS.items():
                blob = ("fixture " + name).encode()
                pins[name] = ("https://fixture.invalid/" + name, len(blob), hashlib.sha256(blob).hexdigest())
                (src / name).write_bytes(blob)
            vblob = b"fixture " + prepare_ocr.VOCR_VERTICAL_MODEL.encode()  # #99: fetched with them
            vpins = {prepare_ocr.VOCR_VERTICAL_MODEL: ("https://fixture.invalid/V", len(vblob), hashlib.sha256(vblob).hexdigest())}
            (src / prepare_ocr.VOCR_VERTICAL_MODEL).write_bytes(vblob)
            vatt = b"the vertical model's attribution fixture"  # its release's ATTRIBUTION.md, kept under its own name
            vatt_pin = {prepare_ocr.VOCR_VERTICAL_ATTRIBUTION: ("https://fixture.invalid/VA", len(vatt),
                                                                hashlib.sha256(vatt).hexdigest())}
            (src / prepare_ocr.VOCR_VERTICAL_ATTRIBUTION).write_bytes(vatt)
            att, lm_att = b"VOCR5 attribution fixture", b"the LMs' attribution"
            att_pin = {prepare_ocr.VOCR_MODEL_ATTRIBUTION: ("https://fixture.invalid/ATTRIBUTION.md", len(att),
                                                            hashlib.sha256(att).hexdigest())}
            (src / "ATTRIBUTION.md").write_bytes(att)  # the release's name, this copy
            # API 26: the text-presence gate's weights and its release's attribution come with them (a --from
            # directory holds that attribution as GATE-ATTRIBUTION.md: the release's name is the models')
            gate, gate_att = b"gate fixture", b"the gate's attribution"
            gate_pin = {prepare_ocr.VOCR_GATE_MODEL: ("https://fixture.invalid/g", len(gate), hashlib.sha256(gate).hexdigest())}
            gate_att_pin = {prepare_ocr.VOCR_GATE_ATTRIBUTION: ("https://fixture.invalid/ga", len(gate_att),
                                                                hashlib.sha256(gate_att).hexdigest())}
            (src / prepare_ocr.VOCR_GATE_MODEL).write_bytes(gate)
            (src / prepare_ocr.VOCR_GATE_ATTRIBUTION).write_bytes(gate_att)
            with mock.patch.dict(prepare_ocr.VOCR5_ASSETS, pins), mock.patch.dict(prepare_ocr.VOCR5V_ASSETS, vpins), \
                 mock.patch.object(prepare_ocr, "VOCR_MODEL_ATTRIBUTION_PIN", att_pin), \
                 mock.patch.object(prepare_ocr, "VOCR_VERTICAL_ATTRIBUTION_PIN", vatt_pin), \
                 mock.patch.object(prepare_ocr, "VOCR_GATE_ASSETS", gate_pin), \
                 mock.patch.object(prepare_ocr, "VOCR_GATE_ATTRIBUTION_PIN", gate_att_pin), \
                 mock.patch.object(prepare_ocr.urllib.request, "urlopen", side_effect=AssertionError("no network")):
                paths = prepare_ocr.fetch_tiers(out, src)
                self.assertEqual(sorted(p.name for p in paths),
                                 sorted([*dict.fromkeys(TIER_FILES), "MODEL-ATTRIBUTION.md", prepare_ocr.VOCR_GATE_MODEL,
                                         "GATE-ATTRIBUTION.md", prepare_ocr.VOCR_VERTICAL_MODEL,
                                         prepare_ocr.VOCR_VERTICAL_ATTRIBUTION]))
                self.assertEqual((out / "GATE-ATTRIBUTION.md").read_bytes(), gate_att)
                self.assertEqual((out / "MODEL-ATTRIBUTION.md").read_bytes(), att)
                self.assertFalse((out / "ATTRIBUTION.md").exists())  # the LMs' name stays theirs
                # the LMs' ATTRIBUTION.md under the release's name is not this file: refused
                (out / "MODEL-ATTRIBUTION.md").unlink()
                (src / "ATTRIBUTION.md").write_bytes(lm_att)
                with self.assertRaises(RuntimeError):
                    prepare_ocr.fetch_tiers(out, src)
                # its own name beside the LMs' file: taken
                (src / "MODEL-ATTRIBUTION.md").write_bytes(att)
                self.assertIn("MODEL-ATTRIBUTION.md", [p.name for p in prepare_ocr.fetch_tiers(out, src)])
        # the real pin is v0.3.6-head2level.1's ATTRIBUTION.md (its SHA256SUMS)
        url, size, sha = prepare_ocr.VOCR_MODEL_ATTRIBUTION_PIN[prepare_ocr.VOCR_MODEL_ATTRIBUTION]
        self.assertTrue(url.endswith("/releases/download/v0.3.6-head2level.1/ATTRIBUTION.md"))
        self.assertEqual((size, sha), (5536, "74b15e8a8ffb01a997ff15bff3ebc137c48a51439732fd838300a9ddff4953c3"))
        # it is VOCR5's release's, which every size with a VOCR5 model comes from (a size may keep an earlier model)
        self.assertEqual(prepare_ocr.VOCR_MODEL_ATTRIBUTION_TAG, prepare_ocr.VOCR5_TAG)
        self.assertLessEqual({pin[4] for pin in TIER_PINS.values() if pin[5] == "VOCR5"}, {prepare_ocr.VOCR5_TAG})
        self.assertIn('X(VJO_PAYLOAD_OCR "MODEL-ATTRIBUTION.md", VJO_PAYLOAD_COPY)',
                      (ROOT / "include/vjo_payload.h").read_text())  # the one-VPK install copies it too

    def test_the_gate_pins_agree_python_and_c(self):
        """API 26: the text-presence gate's weights: one size and hash in tools/prepare_ocr.py, the plugin
        (include/vjo_vocr_sizes.h, core/vocr_ocr.c), the host tests' download (cmake/VocrGate.cmake) and the
        one-VPK payload, with v0.3.2-textgate.1's ATTRIBUTION.md beside them."""
        import re  # noqa: PLC0415
        url, size, sha = prepare_ocr.VOCR_GATE_ASSETS[prepare_ocr.VOCR_GATE_MODEL]
        self.assertEqual((prepare_ocr.VOCR_GATE_MODEL, size, sha),
                         ("textgate_p8_c8_w8.vtd", 1540, "a4e73ad78451c75a19f88a479fc0108daac85beaa88d875b73212ed3c09d85d1"))
        self.assertTrue(url.endswith("/releases/download/v0.3.2-textgate.1/textgate_p8_c8_w8.vtd"))
        sizes = (ROOT / "include/vjo_vocr_sizes.h").read_text()
        self.assertIn('#define VJO_VOCR_GATE_NAME "textgate_p8_c8_w8.vtd"', sizes)
        self.assertIn("#define VJO_VOCR_GATE_BYTES 1540u", sizes)
        c = (ROOT / "core/vocr_ocr.c").read_text()
        block = c[c.index("gate_sha256[32] = {"):]
        block = block[:block.index("};")]
        self.assertEqual("".join(f"{int(b, 16):02x}" for b in re.findall(r"0x([0-9a-fA-F]{2})", block)), sha)
        self.assertIn(f"set(VJO_VOCR_GATE_SHA256 {sha})", (ROOT / "cmake/VocrGate.cmake").read_text())
        payload = (ROOT / "include/vjo_payload.h").read_text()
        self.assertIn("X(VJO_PAYLOAD_OCR VJO_VOCR_GATE_NAME, VJO_PAYLOAD_COPY)", payload)
        self.assertIn('X(VJO_PAYLOAD_OCR "GATE-ATTRIBUTION.md", VJO_PAYLOAD_COPY)', payload)
        url, size, sha = prepare_ocr.VOCR_GATE_ATTRIBUTION_PIN[prepare_ocr.VOCR_GATE_ATTRIBUTION]
        self.assertTrue(url.endswith("/releases/download/v0.3.2-textgate.1/ATTRIBUTION.md"))
        self.assertEqual((size, sha), (3706, "44a353c76e19c6bdabcb9dbd06d14867696dcb59ce0460db22d09749af687f6e"))

    def test_lm_pins_agree_c_kernel_python_and_the_ledger(self):
        table = c_lm_table()
        self.assertEqual(set(table), set(prepare_ocr.VOCR_LM_ASSETS))
        for name, (size, sha, tier) in table.items():
            url, want_size, want_sha = prepare_ocr.VOCR_LM_ASSETS[name]
            self.assertTrue(url.endswith("/releases/download/v0.3.7-lm6.2/" + name), url)
            self.assertEqual((size, sha), (want_size, want_sha), name)
        self.assertEqual(sorted(t for _, _, t in table.values()), [1, 2, 3, 4])
        # the kernel knows the same names and sizes by tier
        kernel = (ROOT / "kernel/game_ocr.c").read_text()
        for n in range(1, 5):
            self.assertIn(f"VJO_VOCR_LM{n}_NAME", kernel)
            self.assertIn(f"VJO_VOCR_LM{n}_BYTES", kernel)
        # each tier's decoder settings are the ones vita-vn-ocr selected it with (v0.3.7-lm6.2's settings.json):
        # VOCR6's LMs at tiers 2 and 4, ctclm.2's at 1 and 3
        source = (ROOT / "core/vocr_ocr.c").read_text()
        block = source[source.index("static const VjoVocrLm lms[VJO_VOCR_LM_TIERS]"):]
        settings = re.findall(r"\},\s*(\d+), (\d), (\d+), (VOCR_LM_BOS|VOCR_LM_EOS)\}", block[:block.index("};")])
        self.assertEqual(settings, [("3000", "1", "500", "VOCR_LM_EOS"), ("2500", "2", "600", "VOCR_LM_BOS"),
                                    ("3000", "3", "500", "VOCR_LM_EOS"), ("2500", "4", "600", "VOCR_LM_BOS")])
        self.assertIn("o->alpha = lm->alpha;", source)
        self.assertIn("o->flags = lm->flags;", source)
        # the ledger's LM block holds the largest, page-rounded
        ledger = (ROOT / "include/vjo_ledger_budget.h").read_text()
        budget = int(re.search(r"#define VJO_BUDGET_GAME_LM (\d+)u", ledger).group(1))
        largest = max(size for size, _, _ in table.values())
        self.assertEqual(budget, (largest + 4095) // 4096 * 4096)
        self.assertIn("\nocr_lm = 1000k\n", (ROOT / "docs/config.example.ini").read_text())


    def test_vocr3_assets_are_the_published_release(self):
        self.assertEqual(prepare_ocr.VOCR3_TAG, "v0.2.5-vocr3.1")
        for name, size, sha in (("VOCR3_H15_w8.vocr", 1155968, VOCR3_H15_SHA256),
                                ("VOCR3_FL10_w8.vocr", 657664, VOCR3_FL10_SHA256)):
            url, got_size, got_sha = prepare_ocr.VOCR3_ASSETS[name]
            self.assertTrue(url.endswith("/releases/download/v0.2.5-vocr3.1/" + name), url)
            self.assertEqual((got_size, got_sha), (size, sha))
        url, size, sha = prepare_ocr.VOCR_ASSETS["H15_w8.vocr"]
        self.assertEqual(prepare_ocr.VOCR_TAG, "v0.2.0")
        self.assertTrue(url.endswith("/releases/download/v0.2.0/H15_w8.vocr"))
        self.assertEqual((size, sha), (1155968, H15_V020_SHA256))

    def test_c_kernel_and_python_pins_agree_and_fit_the_ceiling(self):
        table = c_table()
        python = python_pins()
        self.assertEqual(set(table), set(python))
        # the Large size's ceilings (tools/model_tiers.json: the owner's 2.5 MB of weights, the
        # 1.25 MiB arena of vita-vn-ocr docs/model-tiers.md 1.3) and the weights block's limit
        large = prepare_ocr.model_tiers()["tiers"][-1]
        ceiling = macros()["VJO_VOCR_MAX_BYTES"]
        for name, (size, sha, arena) in table.items():
            _, want_size, want_sha, _ = python[name]
            self.assertEqual((size, sha), (want_size, want_sha), name)
            self.assertLessEqual(size, min(large["weights_max_bytes"], ceiling), name)
            self.assertLessEqual(arena, large["arena_max_bytes"], name)
        # the kernel opens exactly the pinned names, at their pinned sizes
        self.assertEqual(kernel_names(), {name: size for name, (size, _, _) in table.items()})
        # the model metadata names exactly the same files, at the same sizes
        self.assertEqual({name: size for name, (_, _, size) in metadata_table().items()},
                         {name: size for name, (size, _, _) in table.items()})
        # VOCR3 has the shapes of v0.2.0's H15 and FL10: the same bytes and arena, other weights
        for new, old in (("VOCR3_H15_w8.vocr", "H15_w8.vocr"), ("VOCR3_FL10_w8.vocr", "FL10_w8.vocr")):
            self.assertEqual(table[new][0], table[old][0])
            self.assertEqual(table[new][2], table[old][2])
            self.assertNotEqual(table[new][1], table[old][1])

    def test_a_pin_in_any_spelling_is_read_or_fails(self):
        """A model pinned in one table only cannot escape the comparison above: the parsers read it
        (a literal size as well as a macro), and an entry they cannot read fails the entry count."""
        def add(source, after, line):
            self.assertEqual(source.count(after), 1, after)
            return source.replace(after, after + line)
        kernel = (ROOT / "kernel/game_ocr.c").read_text()
        f20 = '    if (!strcmp(name, "F20_w8.vocr")) return VJO_VOCR_F20_BYTES;\n'
        literal = kernel_names(add(kernel, f20, '    if (!strcmp(name, "VOCR6_N_w8.vocr")) return 1155968u;\n'))
        self.assertEqual(literal["VOCR6_N_w8.vocr"], 1155968)
        self.assertNotEqual(literal, kernel_names())
        for line in ('    if (!strcmp(name, "VOCR6_N_w8.vocr")) return 0x11a380u;\n',  # a hex size
                     '    if (!memcmp(name, "VOCR6", 5)) return VJO_VOCR_H15_BYTES;\n',  # another comparison
                     '    if (!strcmp(name, "a.vocr") || !strcmp(name, "VOCR6_N_w8.vocr")) return VJO_VOCR_H15_BYTES;\n',
                     '    if (!strcmp(name, "H15_w8.vocr")) return VJO_VOCR_F20_BYTES;\n'):  # a name twice
            with self.assertRaises(AssertionError, msg=line):
                kernel_names(add(kernel, f20, line))
        worker = (ROOT / "core/vocr_ocr.c").read_text()
        end = ("      0x12,0xf4,0x05,0xf5,0x4b,0x9d,0xf8,0xa2,0xe5,0x5b,0xe8,0x2c,0x38,0x5b,0x71,0x6e},\n"
               "     241056, VJO_VOCR_FINDER},\n")  # VOCR4-W20's, the release table's last entry
        sha = "{" + ",".join(["0x00"] * 32) + "}"
        literal = c_table(add(worker, end, f'    {{"VOCR6_N_w8.vocr", 1155968u, {sha}, 182944u, VOCR_LINES_GRAD}},\n'))
        self.assertEqual(literal["VOCR6_N_w8.vocr"], (1155968, "00" * 32, 182944))
        with self.assertRaises(AssertionError):
            c_table(add(worker, end, f'    {{.name = "VOCR6_N_w8.vocr", .bytes = 1155968u, .sha256 = {sha}}},\n'))
        meta = (ROOT / "core/vocr_models.c").read_text()
        f20 = '    {"F20_w8.vocr", "VOCR1", "F20", VJO_VOCR_F20_BYTES, W20_BOX_MS, W20_JOB_BYTES},\n'
        literal = metadata_table(add(meta, f20, '    {"VOCR6_N_w8.vocr", "VOCR6", "N", 3000000u, 900u, 3500000u},\n'),
                                 estimates=True)
        self.assertEqual(literal["VOCR6_N_w8.vocr"], ("VOCR6", "N", 3000000, 900, 3500000))
        for line in ('    {"VOCR6_N_w8.vocr", "VOCR6", "N", 0x2dc6c0, 900u, 3500000u},\n',  # a hex size
                     '    {"VOCR6_N_w8.vocr", "VOCR6", "N", 3000000u},\n',  # no estimates
                     f20):  # a name twice
            with self.assertRaises(AssertionError, msg=line):
                metadata_table(add(meta, f20, line))

    def test_each_model_generation_is_its_releases(self):
        """The generation the Rubi app names the local engine after: the same in the C metadata
        and in the packager's pins (each release's), VOCR3 for the default, and the VOCRn of a file
        named VOCRn_...; the removed VOCR2 trial is pinned nowhere."""
        meta = metadata_table()
        python = python_pins()
        by_name = {"VOCR3_H15_w8.vocr", "VOCR3_FL10_w8.vocr", "H15_w8.vocr", "FL10_w8.vocr", "F20_w8.vocr",
                   "VOCR4_W20_w8.vocr"}  # the host tools' models; the sizes' own besides them
        self.assertEqual(set(meta), by_name | set(TIER_FILES) | {prepare_ocr.VOCR_VERTICAL_MODEL})
        for name, (generation, shape, _) in meta.items():
            self.assertIn(name, python)
            self.assertEqual(generation, prepare_ocr.VOCR_GENERATION[python[name][3]], name)
            if name.startswith("VOCR"):
                self.assertTrue(name.startswith(generation + "_"), name)
            self.assertIn(shape, name, name)
        self.assertEqual(prepare_ocr.VOCR_GENERATION, {"v0.2.0": "VOCR1", "v0.2.5-vocr3.1": "VOCR3",
                                                       "v0.2.5-vocr4.1": "VOCR4", "v0.3.6-head2level.1": "VOCR5",
                                                       prepare_ocr.VOCR5V_TAG: "VOCR5"})
        self.assertEqual(meta[prepare_ocr.VOCR_DEFAULT_MODEL][0], TIER_PINS["normal"][5])
        tiers = prepare_ocr.model_tiers()["tiers"]
        self.assertEqual([meta[t["model"]][0] for t in tiers], [TIER_PINS[t["id"]][5] for t in tiers])
        for path in ("core/vocr_ocr.c", "core/vocr_models.c", "kernel/game_ocr.c", "include/vjo_vocr_sizes.h",
                     "tools/prepare_ocr.py", "tools/package_release.py"):
            text = (ROOT / path).read_text()
            for pin in ('"VOCR2_FL10_w8.vocr"', "'VOCR2_FL10_w8.vocr'", "VJO_VOCR2", "VOCR2_ASSETS"):
                self.assertNotIn(pin, text, path)

    def test_defaults_everywhere_are_the_normal_size(self):
        tiers = prepare_ocr.model_tiers()
        default = next(t for t in tiers["tiers"] if t["id"] == tiers["default"])
        normal = TIER_PINS["normal"][0]
        self.assertEqual((tiers["default"], default["model"], prepare_ocr.VOCR_DEFAULT_MODEL), ("normal", normal, normal))
        config = (ROOT / "core/config.c").read_text()
        self.assertIn("c->vocr_tier = VJO_VOCR_TIER_DEFAULT;", config)
        self.assertIn("#define VJO_VOCR_TIER_DEFAULT VJO_VOCR_TIER_NORMAL", (ROOT / "include/vjo_vocr_sizes.h").read_text())
        self.assertIn(f'#define VJO_VOCR_DEFAULT_MODEL "{normal}"', (ROOT / "core/vocr_ocr.h").read_text())
        self.assertEqual(macros()["VJO_VOCR_TIER2_NAME"], prepare_ocr.VOCR_DEFAULT_MODEL)
        example = (ROOT / "docs/config.example.ini").read_text()
        self.assertIn("\nvocr_tier = normal\n", example)
        self.assertNotIn("vocr_model", example)
        self.assertNotIn("vocr_fallback_model", example)
        self.assertIn("\nlog_file = on\n", example)
        installer = (ROOT / "tools/install-vita.sh").read_text()
        self.assertNotIn("vocr_model", installer)  # the user's model size is kept on an update
        package = (ROOT / "tools/package_release.py").read_text()
        self.assertNotIn("VOCR_FALLBACK_MODEL", package)
        self.assertFalse(hasattr(prepare_ocr, "VOCR_FALLBACK_MODEL"))
        self.assertNotIn('"vocr_model": "', package)  # no model name of its own: the template's size
        import package_release  # noqa: E402
        generations = {name: {"generation": prepare_ocr.VOCR_GENERATION[tag]}
                       for name, _, tag, _ in package_release.shipped_models() if name.endswith(".vocr")}
        defaults = package_release.manifest_defaults(example, generations)
        self.assertEqual((defaults["vocr_tier"], defaults["ocr_engine"]), ("normal", TIER_PINS["normal"][5]))
        self.assertNotIn("vocr_model", defaults)
        # the three sizes are shipped, each with its own release's pins; the earlier generations' are not
        shipped = {name: (pins[name], tag, role) for name, pins, tag, role in package_release.shipped_models()}
        files = list(dict.fromkeys(TIER_FILES))  # a model two sizes share is shipped once
        self.assertEqual(list(shipped)[:len(files)], files)
        roles = {"small": "Small model size", "normal": "Normal model size (default)", "large": "Large model size"}
        for name in files:
            self.assertEqual(shipped[name][2], " and ".join(roles[t] for t, pin in TIER_PINS.items() if pin[0] == name))
        for tier, (name, size, sha, _, tag, _) in TIER_PINS.items():
            self.assertEqual(shipped[name][1], tag, name)
            self.assertEqual(shipped[name][0][1:], (size, sha), name)
        for name in ("H15_w8.vocr", "FL10_w8.vocr", "F20_w8.vocr", "VOCR3_H15_w8.vocr", "VOCR3_FL10_w8.vocr",
                     "VOCR4_FL10_w8.vocr", "VOCR4_H15_w8.vocr", "VOCR4_W20_w8.vocr"):
            if name not in TIER_FILES:
                self.assertNotIn(name, shipped)
        self.assertEqual(set(shipped) - {t["model"] for t in tiers["tiers"]},
                         {*prepare_ocr.VOCR_LM_ASSETS, prepare_ocr.VOCR_GATE_MODEL,
                          prepare_ocr.VOCR_VERTICAL_MODEL})  # the LMs, the gate (API 26), VOCR5-V (API 47)


class RuntimePinEverywhere(unittest.TestCase):
    """The vita-vn-ocr commit cmake/VocrOCR.cmake pins is the one CI's jobs check out and the docs' Build line names
    (audit-vocr6-finder-h B1: a pin bump that missed the workflow failed every GitHub build job at configure)."""

    def test_the_workflow_and_the_docs_check_out_the_cmake_pin(self):
        cmake = (ROOT / "cmake" / "VocrOCR.cmake").read_text(encoding="utf-8")
        pin = re.search(r"^set\(VJO_VOCR_COMMIT ([0-9a-f]{40})\)$", cmake, re.M).group(1)
        flow = (ROOT / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8")
        refs = re.findall(r"repository: bee-san/vita-vn-ocr\n(?:\s+#.*\n)*\s+ref: ([0-9a-f]{40})", flow)
        self.assertTrue(refs, "no vita-vn-ocr checkout found in release.yml")
        self.assertEqual(set(refs), {pin}, refs)
        docs = (ROOT / "docs" / "vita-vn-ocr.md").read_text(encoding="utf-8")
        self.assertIn(f"git -C /path/to/vita-vn-ocr checkout {pin}", docs)


class TestModels(unittest.TestCase):
    """--backend vocr-tests: what the host tests' VJO_VOCR_MODEL_DIR holds for their real-model checks, the set
    tools/fast_gate.sh's models lane checks (--check) before it runs them."""

    def test_the_set_is_every_pin_the_plugin_reads(self):
        """Every recognizer the plugin pins (shipped or not: the tests run FL10, H15, F20, VOCR3's and VOCR4-W20
        as well as the sizes' and VOCR5-V), its four LMs and the gate's weights, each at the plugin's own size and
        SHA-256, and v0.3.0's three VOCR5 recognizers under v0.3.0/; no attribution and nothing else."""
        models = {name: assets[name][1:] for name, assets, _ in prepare_ocr.test_models()}
        self.assertEqual(len(models), len(prepare_ocr.test_models()))  # each file once
        want = {name: (size, sha) for name, (size, sha, _) in c_table().items()}
        want.update({name: (size, sha) for name, (size, sha, _) in c_lm_table().items()})
        _, size, sha = prepare_ocr.VOCR_GATE_ASSETS[prepare_ocr.VOCR_GATE_MODEL]
        want[prepare_ocr.VOCR_GATE_MODEL] = (size, sha)
        want.update({f"v0.3.0/{name}": (size, sha) for name, (_, size, sha) in prepare_ocr.VOCR5_V030_ASSETS.items()})
        self.assertEqual(models, want)
        sizes = macros()
        for macro in ("VJO_VOCR_TIER1_NAME", "VJO_VOCR_TIER2_NAME", "VJO_VOCR_TIER3_NAME", "VJO_VOCR_CASCADE_NAME",
                      "VJO_VOCR_VERTICAL_NAME", "VJO_VOCR_GATE_NAME"):
            self.assertIn(sizes[macro], models, macro)
        for name in ("FL10_w8.vocr", "H15_w8.vocr"):  # host/tests/test_game_vocr.c's and test_capture_parity.c's
            self.assertIn(name, models)

    def test_the_off_path_blobs_are_what_test_vocr_reads(self):
        """host/tests/test_vocr.c's off-path check reads v0.3.0's blobs as VJO_VOCR_MODEL_DIR/v0.3.0/<name> and skips
        without them (the models lane would pass without running it): the set holds those names there, from
        release v0.3.0, each its sizes' blob without the head gate (a size its gated blob's, less the gate)."""
        source = (ROOT / "host" / "tests" / "test_vocr.c").read_text(encoding="utf-8")
        body = source[source.index("static void test_off_path_reads_v030(void)"):]
        names = re.search(r"v030\[3\] = \{([^}]*)\}", body).group(1)
        self.assertIn('"%s/v0.3.0/%s"', body)
        read = re.findall(r'"([^"]+)"', names)
        self.assertEqual(sorted(read), sorted(prepare_ocr.VOCR5_V030_ASSETS))
        tags = {name: tag for name, _, tag in prepare_ocr.test_models()}
        for name in read:
            self.assertEqual(tags[f"v0.3.0/{name}"], "v0.3.0")
        for old, new in (("VOCR5_S_w8.vocr", "VOCR5_S_w8g.vocr"), ("VOCR5_N_w8.vocr", "VOCR5_N_w8g2.vocr"),
                         ("VOCR5_L_w8.vocr", "VOCR5_L_w8g2.vocr")):
            self.assertLess(prepare_ocr.VOCR5_V030_ASSETS[old][1], prepare_ocr.VOCR5_ASSETS[new][1])
            self.assertTrue(prepare_ocr.VOCR5_V030_ASSETS[old][0].endswith("/releases/download/v0.3.0/" + old))

    def _fixture(self, tmp, names=("A_w8.vocr", "B_w8.vocr", "c.vlm", "d.vtd")):
        """Pins of small fixture files in tmp/src, as test_models() gives them."""
        import hashlib  # noqa: PLC0415
        src = Path(tmp) / "src"
        src.mkdir()
        assets = {}
        for name in names:
            data = ("fixture " + name).encode()
            (src / name).write_bytes(data)
            assets[name] = ("https://fixture.invalid/" + name, len(data), hashlib.sha256(data).hexdigest())
        return src, [(name, assets, "v0.0.0-fixture") for name in names]

    def test_check_names_each_file_missing_or_not_the_pinned_one(self):
        import shutil  # noqa: PLC0415
        import tempfile  # noqa: PLC0415
        with tempfile.TemporaryDirectory() as tmp:
            src, models = self._fixture(tmp)
            self.assertEqual(prepare_ocr.check_models(src, models), [])
            out = Path(tmp) / "out"
            shutil.copytree(src, out)
            (out / "A_w8.vocr").unlink()
            b_size = (out / "B_w8.vocr").stat().st_size
            (out / "B_w8.vocr").write_bytes((out / "B_w8.vocr").read_bytes() + b"!")  # another size
            data = (out / "c.vlm").read_bytes()
            (out / "c.vlm").write_bytes(data[:-1] + b"?")  # the same size, other bytes
            self.assertEqual(prepare_ocr.check_models(out, models),
                             ["A_w8.vocr: missing (v0.0.0-fixture)",
                              f"B_w8.vocr: {b_size + 1} B, not the pinned {b_size} B (v0.0.0-fixture)",
                              "c.vlm: not the pinned SHA-256 (v0.0.0-fixture)"])
            self.assertEqual(len(prepare_ocr.check_models(Path(tmp) / "absent", models)), len(models))

    def test_the_cli_checks_without_a_download_and_fetches_from_a_local_copy(self):
        import contextlib  # noqa: PLC0415
        import io  # noqa: PLC0415
        import tempfile  # noqa: PLC0415
        from unittest import mock  # noqa: PLC0415

        def run(*argv):
            stdout = io.StringIO()
            with mock.patch.object(sys, "argv", ["prepare_ocr.py", *argv]), contextlib.redirect_stdout(stdout):
                try:
                    prepare_ocr.main()
                    code = 0
                except SystemExit as e:
                    code = e.code
            return code, stdout.getvalue()

        with tempfile.TemporaryDirectory() as tmp:
            src, models = self._fixture(tmp)
            out = Path(tmp) / "out"
            with mock.patch.object(prepare_ocr, "test_models", return_value=models), \
                 mock.patch.object(prepare_ocr.urllib.request, "urlopen", side_effect=AssertionError("no network")):
                code, text = run("--backend", "vocr-tests", "--check", "--output", str(out))
                self.assertEqual(code, 1)
                self.assertIn("0 of 4 pinned test models verified in", text)
                self.assertFalse(out.exists())  # the check makes nothing
                code, text = run("--backend", "vocr-tests", "--output", str(out), "--from", str(src))
                self.assertEqual(code, 0, text)
                self.assertEqual(sorted(p.name for p in out.iterdir()), sorted(name for name, _, _ in models))
                code, text = run("--backend", "vocr-tests", "--check", "--output", str(out))
                self.assertEqual((code, text.splitlines()[-1]), (0, f"4 of 4 pinned test models verified in {out}"))
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(run("--backend", "vocr-lm", "--check", "--output", str(out))[0], 2)  # its own set only

    def test_a_file_in_a_subdirectory_is_its_releases_asset_of_its_own_name(self):
        """v0.3.0/ (the off-path check's): a --from directory holds the release's file at its own name, the public
        download is the pinned URL; either way the file lands at its path under --output, and --check finds it
        there (and only there)."""
        import hashlib  # noqa: PLC0415
        import tempfile  # noqa: PLC0415
        from unittest import mock  # noqa: PLC0415
        with tempfile.TemporaryDirectory() as tmp:
            data = b"fixture v0.3.0 blob"
            pin = ("https://fixture.invalid/v0.3.0/E_w8.vocr", len(data), hashlib.sha256(data).hexdigest())
            models = [("v0.3.0/E_w8.vocr", {"v0.3.0/E_w8.vocr": pin}, "v0.3.0")]
            src = Path(tmp) / "src"
            src.mkdir()
            (src / "E_w8.vocr").write_bytes(data)  # as a release's download directory holds it
            (src / "v0.3.0").mkdir()
            (src / "v0.3.0" / "E_w8.vocr").write_bytes(b"not it")
            served = []

            class Response:
                def __enter__(self):
                    return self

                def __exit__(self, *exc):
                    return False

                def read(self, n):
                    return data[:n]

            def urlopen(request, timeout):
                served.append(request.full_url)
                return Response()

            with mock.patch.object(prepare_ocr, "test_models", return_value=models):
                out = Path(tmp) / "from"
                with mock.patch.object(prepare_ocr.urllib.request, "urlopen", side_effect=AssertionError("no network")):
                    paths = prepare_ocr.fetch_test_models(out, src)
                self.assertEqual(paths, [out / "v0.3.0" / "E_w8.vocr"])
                self.assertEqual((out / "v0.3.0" / "E_w8.vocr").read_bytes(), data)
                self.assertEqual(prepare_ocr.check_models(out, models), [])
                out = Path(tmp) / "public"
                with mock.patch.object(prepare_ocr.urllib.request, "urlopen", side_effect=urlopen):
                    self.assertEqual(prepare_ocr.fetch_test_models(out), [out / "v0.3.0" / "E_w8.vocr"])
                self.assertEqual(served, [pin[0]])
                self.assertEqual(prepare_ocr.check_models(out, models), [])
                (out / "E_w8.vocr").write_bytes((out / "v0.3.0" / "E_w8.vocr").read_bytes())
                (out / "v0.3.0" / "E_w8.vocr").unlink()  # beside, not in v0.3.0/: missing
                self.assertEqual(prepare_ocr.check_models(out, models), ["v0.3.0/E_w8.vocr: missing (v0.3.0)"])


if __name__ == "__main__":
    unittest.main()
