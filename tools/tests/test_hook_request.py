"""Hook requests (docs/hook-requests.md, #358): the issue form, tools/hook_request.py and the debug build's hooks.log.

The parser is checked against the C it reads: every format string game_ocr/hooks.c writes to hooks.log must be one
this test renders (so a new or changed line fails here until tools/hook_request.py reads it), the place reasons and
string-copy imports are the C tables', and the paths are the worker's and collect_vita.py's. The form is checked
against GitHub's issue-form schema (its documented keys, types and rules) with a YAML reader of the subset it uses
(the gate's image has no PyYAML; where PyYAML is installed both readings must agree), and a form response, rendered as
GitHub renders one, must reach the tool. No game text anywhere: hooks.log holds none."""
import io
import json
import re
import subprocess
import sys
import tempfile
import unittest
import zipfile
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import collect_vita  # noqa: E402
import hook_request as hr  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
FORM = ROOT / ".github/ISSUE_TEMPLATE/hook-request.yml"
DOC = ROOT / "docs/hook-requests.md"
TOOL = ROOT / "tools/hook_request.py"

# Every format string game_ocr/hooks.c may write to hooks.log, with values to render it with. One is not on main yet
# and is read already: draft PR #318's line for the PSP emulator's process.
REASONS = ["range", "align", "entry", "window", "signature", "unported", "version", "full", "tai", "relocated"]
FORMATS = {
    "profile: %s (%d bytes)\n": [("loaded current title", 533), ("unavailable: CALL/OCR fallback", -123)],
    "%.9s: text source %u%s\n": [("PCSG00628", 1, ""), ("PCSG00628", 2, ", sites off"), ("", 1, "")],
    "nothing: %s\n": [("the PSP emulator's process",), ("no title ID",)],
    "nothing: no main module (eboot.bin) with a segment 0\n": [()],
    "the PSP emulator's process: no hooks; PSP text %s\n": [("on (Every game)",), ("off",)],
    "sites off (hook_sites = off): no table site\n": [()],
    "main module %s NID %08X seg0 %08X+%X APP_VER %04X; %u verified, %u Luna sites\n":
        [("adv", 0x1EE45F02, 0x81000000, 0x2F1A40, 0x0100, 1, 2),
         ("Game Two", 0x0000000F, 0x81200000, 0x1000, 0, 0, 0)],
    "verified +%X %s r%u%s": [(0x3373C, "thumb", 5, ""), (0x2CA70, "arm", 15, ", Luna's text")],
    "luna %08X r%u": [(0x8003773C, 5)],
    "luna %08X r%u +%X": [(0x8119ADDA, 7, 0x19ADDA)],
    " tai %08X\n": [(0x8002D013,)],
    " hooked, stream %u\n": [(1,), (8,)],
    " skipped: the address is hooked already\n": [()],
    " refused: %s\n": [(r,) for r in REASONS],
    " bytes %s\n": [("F46200240C6F8B490A14695F9000018B",), ("",)],
    "import %08X/%08X: %s\n": [(0xBE43BB07, 0x7205BFDB, "hooked"), (0xCAE9ACE6, 0xAC595E68, "not hooked")],
    "sites off; CALL imports hooked %u of %u\n": [(8, 8)],
    "sites hooked %u, refused %u; CALL imports hooked %u of %u\n": [(1, 2, 7, 8)],
}


def c_formats(path):
    """The format strings of every DLOG/dlog_line call in a C file, unescaped."""
    text = path.read_text(encoding="utf-8")
    return {bytes(m.group(1), "utf-8").decode("unicode_escape")
            for m in re.finditer(r'\b(?:DLOG|dlog_line)\(\s*"((?:[^"\\]|\\.)*)"', text)}


def imports_lines(hooked=8):
    """The eight import lines, in game_ocr/hooks.c's order, the first `hooked` of them hooked."""
    return "".join("import %08X/%08X: %s\n" % (lib, nid, "hooked" if k < hooked else "not hooked")
                   for k, (lib, nid) in enumerate(hr.IMPORTS))


def a_log(**kw):
    """A hooks.log in game_ocr/hooks.c's order: a verified entry hooked, a Luna site at its address skipped, a Luna
    site taiHEN refused, one the placement check refused (its bytes), one past segment 0 (no bytes)."""
    return ("PCSG00628: text source 1\n"
            "main module adv NID 1EE45F02 seg0 81000000+2F1A40 APP_VER 0100; 1 verified, 4 Luna sites\n"
            "verified +3373C thumb r5 hooked, stream 1\n"
            "luna 8003773C r5 skipped: the address is hooked already\n"
            "luna 80012344 r0 tai 8002D013\n refused: tai\n bytes 2DE9F04F85B000210091\n"
            "luna 8001A39A r0 refused: window\n bytes F46200240C6F8B490A14695F9000018B\n"
            "luna 8119ADDA r7 refused: range\n bytes \n"
            + imports_lines(kw.get("hooked", 7)) +
            "sites hooked 1, refused 3; CALL imports hooked %u of 8\n" % kw.get("hooked", 7))


# ---- the issue form: GitHub's schema, read without PyYAML ----

class FormYaml:
    """The YAML subset the form uses: block mappings and sequences, plain scalars, double-quoted one-line scalars,
    flow sequences of double-quoted scalars, | and >- block scalars, # comments. Anything else is a ValueError, so a
    form edit that leaves the subset fails here (keep to it: GitHub reads full YAML, this test does not)."""

    def __init__(self, text):
        if "\t" in text:
            raise ValueError("a tab")
        self.lines, self.i = text.split("\n"), 0

    def load(self):
        value = self.node(0)
        self.skip()
        if self.i < len(self.lines):
            raise ValueError(f"line {self.i + 1}: not read")
        return value

    def skip(self):
        while self.i < len(self.lines) and (not self.lines[self.i].strip()
                                            or self.lines[self.i].lstrip().startswith("#")):
            self.i += 1

    @staticmethod
    def indent(line):
        return len(line) - len(line.lstrip(" "))

    def node(self, ind):
        self.skip()
        line = self.lines[self.i]
        return self.seq(self.indent(line)) if line.lstrip().startswith("- ") else self.map(self.indent(line))

    def seq(self, ind):
        out = []
        while True:
            self.skip()
            if self.i >= len(self.lines) or self.indent(self.lines[self.i]) < ind:
                return out
            line = self.lines[self.i]
            if self.indent(line) != ind or not line[ind:].startswith("- "):
                raise ValueError(f"line {self.i + 1}: a sequence item")
            rest = line[ind + 2:]
            if re.match(r"[A-Za-z_][\w-]*:( |$)", rest):
                self.lines[self.i] = " " * (ind + 2) + rest  # "- key: value" opens a mapping at ind + 2
                out.append(self.map(ind + 2))
            else:
                self.i += 1
                out.append(self.scalar(rest))

    def map(self, ind):
        out = {}
        while True:
            self.skip()
            if self.i >= len(self.lines) or self.indent(self.lines[self.i]) < ind:
                return out
            line = self.lines[self.i]
            if self.indent(line) > ind:
                raise ValueError(f"line {self.i + 1}: indentation")
            if line[ind:].startswith("- "):
                return out  # the parent's sequence goes on
            m = re.fullmatch(r"([A-Za-z_][\w-]*):(?: (.*))?", line[ind:])
            if not m or m.group(1) in out:
                raise ValueError(f"line {self.i + 1}: a mapping key (or a repeated one)")
            key, value = m.group(1), (m.group(2) or "").strip()
            self.i += 1
            if value in ("|", ">-"):
                out[key] = self.block(ind, value)
            elif value:
                out[key] = self.scalar(value)
            else:
                self.skip()
                nxt = self.lines[self.i] if self.i < len(self.lines) else ""
                if nxt.lstrip().startswith("- ") and self.indent(nxt) >= ind:
                    out[key] = self.seq(self.indent(nxt))
                elif self.indent(nxt) > ind:
                    out[key] = self.map(self.indent(nxt))
                else:
                    raise ValueError(f"line {self.i}: an empty value")

    def block(self, ind, style):
        body, inner = [], None
        while self.i < len(self.lines):
            line = self.lines[self.i]
            if line.strip():
                if self.indent(line) <= ind:
                    break
                inner = self.indent(line) if inner is None else inner
                if self.indent(line) < inner:
                    raise ValueError(f"line {self.i + 1}: a block scalar's indentation")
                body.append(line[inner:])
            else:
                body.append("")
            self.i += 1
        while body and not body[-1]:
            body.pop()
        if style == "|":
            return "\n".join(body) + "\n"
        if any(x.startswith(" ") for x in body):
            raise ValueError("a more-indented line in a folded scalar")
        return "\n".join(" ".join(p.split("\n")) for p in "\n".join(body).split("\n\n"))

    # Plain scalars YAML 1.1 (PyYAML, and GitHub's parser) reads as something other than a string: a boolean, null, a
    # number in any of its forms (01.00 is the float 1.0), a time (1:30) or a date. The subset reads true, false and a
    # decimal integer; any other of these is refused, so it can never pass here as a string while GitHub sees a
    # boolean or a number (dropdown options Yes / No, placeholder 01.00, label on: audit-rubi-358-hooktemplate F5).
    YAML11 = re.compile(r"(?i:y|n|yes|no|on|off|true|false|~|null)|[-+]?\.(?i:inf|nan)"
                        r"|[-+]?(?:[0-9][0-9_]*(?:[.:][0-9_.:]*)?|\.[0-9][0-9_]*)(?:[eE][-+]?[0-9]+)?"
                        r"|[-+]?0[xXoObB][0-9a-fA-F_]+|\d{4}-\d\d?-\d\d?(?:[Tt ].*)?")

    @classmethod
    def scalar(cls, s):
        s = s.strip()
        if s.startswith('"'):
            return json.loads(s)  # one line; the form's escapes are JSON's
        if s.startswith("["):
            if not s.endswith("]"):
                raise ValueError(s)
            return [json.loads(x) for x in s[1:-1].split(",")] if s[1:-1].strip() else []
        if s in ("true", "false"):
            return s == "true"
        if re.fullmatch(r"[1-9][0-9]*|0", s):
            return int(s)
        if cls.YAML11.fullmatch(s):
            raise ValueError(f"YAML reads {s!r} as a boolean, null, a number or a date: quote it")
        if re.match(r"[-'{&*!|>%@`?:]", s) or ": " in s or " #" in s or s.endswith(":"):
            raise ValueError(f"not a plain scalar of the subset: {s!r}")
        return s


TOP_KEYS = {"name", "description", "title", "labels", "assignees", "body", "projects", "type"}
# The type GitHub wants for each attribute and validation (its form schema): a boolean where one is meant, an integer
# where one is meant, every other value a string (options: strings, or checkbox options' mappings).
BOOLEANS, INTEGERS = {"required", "multiple"}, {"default", "min_length"}
ATTRIBUTES = {"markdown": {"value"}, "textarea": {"label", "description", "placeholder", "value", "render"},
              "input": {"label", "description", "placeholder", "value"},
              "dropdown": {"label", "description", "multiple", "options", "default"},
              "checkboxes": {"label", "description", "options"}, "upload": {"label", "description"}}
VALIDATIONS = {"markdown": set(), "textarea": {"required", "min_length"}, "input": {"required", "min_length"},
               "dropdown": {"required"}, "checkboxes": {"required"}, "upload": {"required", "accept"}}


def load_form():
    text = FORM.read_text(encoding="utf-8")
    form = FormYaml(text).load()
    try:
        import yaml
    except ImportError:
        return form, text
    assert yaml.safe_load(text) == form, "the subset reader and PyYAML read the form differently"
    return form, text


def rendered_issue(form, answers):
    """A form response as GitHub turns it into the issue's body: each field's label as a ### heading, then its value
    (a textarea with render in a fenced block of that language; an empty one _No response_; checkboxes as a task
    list)."""
    out = []
    for el in form["body"]:
        if el["type"] == "markdown":
            continue
        a = el["attributes"]
        value = answers.get(el["id"], "")
        if el["type"] == "checkboxes":
            value = "\n".join(f"- [X] {o['label']}" for o in a["options"])
        elif not value:
            value = "_No response_"
        elif a.get("render"):
            value = f"```{a['render']}\n{value}\n```"
        out.append(f"### {a['label']}\n\n{value}\n")
    return "\n".join(out)


class IssueForm(unittest.TestCase):
    def test_the_form_is_a_valid_issue_form(self):
        form, text = load_form()
        self.assertTrue(text.endswith("\n") and not text.endswith("\n\n"))
        self.assertLessEqual(set(form), TOP_KEYS)
        for key in ("name", "description"):
            self.assertIsInstance(form[key], str)
            self.assertTrue(form[key].strip(), key)
        self.assertEqual(form["labels"], ["hook request"])  # the label exists on bee-san/Rubi_for_vita (gh label list)
        body = form["body"]
        self.assertIsInstance(body, list)
        ids, labels = set(), set()
        for k, el in enumerate(body):
            self.assertLessEqual(set(el), {"type", "id", "attributes", "validations"}, k)
            t = el["type"]
            self.assertIn(t, ATTRIBUTES, k)
            a = el["attributes"]
            self.assertLessEqual(set(a), ATTRIBUTES[t], k)
            self.assertLessEqual(set(el.get("validations") or {}), VALIDATIONS[t], k)
            if t == "markdown":
                self.assertNotIn("id", el, "a markdown element has no id")
                self.assertTrue(a["value"].strip())
                continue
            self.assertRegex(el["id"], r"^[A-Za-z0-9_-]+$")
            self.assertNotIn(el["id"], ids)
            ids.add(el["id"])
            self.assertTrue(a["label"].strip(), k)
            self.assertNotIn(a["label"].lower(), labels)
            labels.add(a["label"].lower())
            self.assertNotIn("password", a["label"].lower())  # GitHub refuses it in a label
            if t == "dropdown":
                self.assertTrue(a["options"])
                self.assertEqual(len(a["options"]), len(set(a["options"])))
                self.assertFalse({o.lower() for o in a["options"]} & {"none"})
                self.assertTrue(all(isinstance(o, str) and o.strip() for o in a["options"]))
            if t == "checkboxes":
                for o in a["options"]:
                    self.assertLessEqual(set(o), {"label", "required"})
                    self.assertNotIn(o["label"].lower(), labels)
                    labels.add(o["label"].lower())
        self.assertTrue(ids, "at least one field that is not markdown")
        for el in body:  # every value the type GitHub wants
            for key, value in list(el["attributes"].items()) + list((el.get("validations") or {}).items()):
                want = bool if key in BOOLEANS else int if key in INTEGERS else (list if key == "options" else str)
                self.assertIs(type(value), want, (el.get("id"), key, value))
                if key == "options":
                    for o in value:
                        if isinstance(o, dict):
                            self.assertIs(type(o["label"]), str, o)
                            self.assertIs(type(o.get("required", False)), bool, o)
                        else:
                            self.assertIs(type(o), str, o)
        log = next(el for el in body if el.get("id") == "hooks-log")
        self.assertEqual((log["type"], log["attributes"]["render"], log["validations"]["required"]),
                         ("textarea", "text", True), "the log comes as a fenced block, which the tool reads")

    def test_what_yaml_would_type_is_refused(self):
        """The edits GitHub would read as booleans, null or numbers fail here too, without PyYAML (the gate's image)."""
        form = FORM.read_text(encoding="utf-8")
        edits = [form.replace('        - "I can\'t tell"\n', "        - Yes\n        - No\n"),
                 form.replace('      placeholder: "01.00"', "      placeholder: 01.00"),
                 form.replace("      label: Game\n", "      label: on\n"),
                 form.replace("      label: Game\n", "      label: ~\n"),
                 form.replace('      placeholder: "PCSG00628"', "      placeholder: 2026-10-27"),
                 form.replace('      placeholder: "PCSG00628"', "      placeholder: 0x1F")]
        for k, text in enumerate(edits):
            self.assertNotEqual(text, form, k)
            with self.assertRaises(ValueError, msg=k):
                FormYaml(text).load()
        for word in ("plain text", "Rubi v0.9.8", "PCSG00628", "1.0.2 (Default)", "x 1"):
            self.assertEqual(FormYaml.scalar(word), word)
        self.assertEqual((FormYaml.scalar("true"), FormYaml.scalar("12"), FormYaml.scalar('"01.00"')),
                         (True, 12, "01.00"))

    def test_it_names_the_paths_the_code_writes_and_fetches(self):
        form, text = load_form()
        intro = form["body"][0]["attributes"]["value"]
        hooks_c = (ROOT / "game_ocr/hooks.c").read_text(encoding="utf-8")
        self.assertIn(f'sceIoOpen("{hr.DEVICE_PATH}"', hooks_c)
        ftp = f"{collect_vita.DATA}/hooks.log"
        self.assertIn(ftp, collect_vita.FILES)
        for doc in (intro, DOC.read_text(encoding="utf-8")):
            self.assertIn(hr.DEVICE_PATH, doc)
            self.assertIn(f"ftp://VITA_IP:1337/{ftp}", doc)  # curl's URL, as collect_vita.py builds it
            usb = ftp.split(":/", 1)[1]  # the card's own path: on the computer, its drive's (VitaShell's USB)
            self.assertRegex(doc, rf"drive[^\n]*`{re.escape(usb)}`|`{re.escape(usb)}`[^\n]*drive")
            self.assertIn("bash tools/install_ftp.sh --collect VITA_IP", doc)
            self.assertIn("hook-request.md", doc)
            self.assertIn("-debug.vpk", doc)
            self.assertIn("Automatic", doc)
        self.assertIn("https://github.com/bee-san/Rubi_for_vita/blob/main/docs/hook-requests.md", intro)
        self.assertEqual(hr.DOC, "https://github.com/bee-san/Rubi_for_vita/blob/main/docs/hook-requests.md")
        self.assertEqual(hr.FORM, f"https://github.com/bee-san/Rubi_for_vita/issues/new?template={FORM.name}")
        self.assertIn(hr.FORM, DOC.read_text(encoding="utf-8"))

    def test_a_filled_in_form_reaches_the_tool(self):
        form, _ = load_form()
        issue = rendered_issue(form, {"game": "a game", "title-id": "PCSG00628", "rubi-version": "Rubi v0.9.8 debug",
                                      "text-now": "OCR's reading: no hook text", "hooks-log": a_log().rstrip("\n")})
        self.assertIn("### hooks.log\n\n```text\nPCSG00628: text source 1\n", issue)
        r = subprocess.run([sys.executable, str(TOOL), "-", "--json"], input=issue, capture_output=True, text=True,
                           timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        j = json.loads(r.stdout)
        self.assertEqual((j["title"], j["module"]["nid"], j["module"]["app_ver"]), ("PCSG00628", "0x1EE45F02", "01.00"))
        self.assertEqual((j["unknown"], j["warnings"]), ([], []))
        self.assertEqual([s["outcome"] for s in j["sites"]], ["hooked", "skipped", "refused", "refused", "refused"])
        # a reply that quotes the log (GitHub's Quote reply: every line "> "), once or twice
        for quote in ("> ", "> > "):
            reply = "Same here:\n\n" + "".join(quote + x + "\n" for x in a_log().splitlines())
            r = subprocess.run([sys.executable, str(TOOL), "-", "--json"], input=reply, capture_output=True, text=True,
                               timeout=60)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(json.loads(r.stdout)["module"]["nid"], "0x1EE45F02")
        # an empty box: no log, a message, exit 2
        issue = rendered_issue(form, {"game": "a game", "title-id": "PCSG00628"})
        r = subprocess.run([sys.executable, str(TOOL), "-"], input=issue, capture_output=True, text=True, timeout=60)
        self.assertEqual((r.returncode, r.stdout), (2, ""))
        self.assertIn("no hooks.log in standard input", r.stderr)


# ---- hooks.log: the C formats ----

class LogGrammar(unittest.TestCase):
    def test_every_line_the_worker_writes_is_one_the_tool_reads(self):
        written = c_formats(ROOT / "game_ocr/hooks.c")
        self.assertGreaterEqual(len(written), 14)
        new = written - set(FORMATS)
        self.assertFalse(new, f"game_ocr/hooks.c writes hooks.log lines this test (and tools/hook_request.py) does not "
                              f"know: {sorted(new)}")
        # each rendered line, alone after a first line, is read (a site's outcome after its site's line)
        for fmt, cases in FORMATS.items():
            for args in cases:
                line = fmt % args
                if fmt.startswith("%.9s"):
                    text = line
                elif fmt.startswith((" tai", " hooked", " skipped", " refused")) and "\n" in line:
                    text = "T0000001: text source 1\nluna 80001000 r0" + line
                elif fmt.startswith(" bytes"):
                    text = "T0000001: text source 1\nluna 80001000 r0 refused: entry\n" + line
                else:
                    text = "T0000001: text source 1\n" + line + ("\n" if not line.endswith("\n") else "")
                log = hr.parse(text)
                self.assertIsNotNone(log, line)
                self.assertEqual(log["unknown"], [], f"{fmt!r} % {args!r}")

    def test_the_tables_are_the_c_ones(self):
        place = (ROOT / "core/hook_place.c").read_text(encoding="utf-8")
        names = re.search(r"names\[VJO_PLACE_COUNT\] = \{\s*(.*?)\};", place, re.S).group(1)
        self.assertEqual(re.findall(r'"(\w+)"', names), ["ok"] + REASONS)
        self.assertEqual(set(hr.REASONS), set(REASONS))
        hooks_c = (ROOT / "game_ocr/hooks.c").read_text(encoding="utf-8")
        libs = {k: int(v, 16) for k, v in re.findall(r"#define (NID_SCE_\w+) 0x([0-9A-F]+)u", hooks_c)}
        table = [(libs[lib], int(nid, 16), name) for name, lib, nid in
                 re.findall(r"\[CALL_(\w+)\] = \{(NID_SCE_\w+), 0x([0-9A-F]+)u,", hooks_c)]
        self.assertEqual(len(table), 8)
        self.assertEqual([(lib, nid) for lib, nid, _ in table], list(hr.IMPORTS), "the same imports, in order")
        for lib, nid, name in table:  # MEMCPY -> memcpy, CLIB_STRNLEN -> sceClibStrnlen
            want = name.lower() if not name.startswith("CLIB_") else "sceClib" + name[5:].capitalize()
            self.assertEqual(hr.IMPORTS[(lib, nid)], want)
        streams = (ROOT / "include/vjo_game_ocr.h").read_text(encoding="utf-8")
        self.assertRegex(streams, rf"#define VJO_HOOK_SITE_STREAMS {hr.SITE_STREAMS}u\b")
        self.assertIn(f"#define VJO_LUNA_SEG0 0x{hr.LUNA_SEG0:08X}u", (ROOT / "core/hook_luna.h").read_text())
        api = (ROOT / "include/vjo_api.h").read_text(encoding="utf-8")
        for code, name in hr.MODES.items():
            self.assertRegex(api, rf"#define VJO_TEXT_SOURCE_{name.upper()} {code}u")

    def test_a_whole_log(self):
        log = hr.parse(a_log())
        self.assertEqual((log["unknown"], log["warnings"]), ([], []))
        self.assertEqual((log["title"], log["text_source"], log["sites_off"], log["nothing"]),
                         ("PCSG00628", 1, False, None))
        self.assertEqual(log["module"], {"name": "adv", "nid": 0x1EE45F02, "seg0": 0x81000000, "size": 0x2F1A40,
                                         "app_ver": 0x0100, "verified": 1, "luna": 4})
        s = log["sites"]
        self.assertEqual([(x["kind"], x["outcome"], x["reason"]) for x in s],
                         [("verified", "hooked", None), ("luna", "skipped", None), ("luna", "refused", "tai"),
                          ("luna", "refused", "window"), ("luna", "refused", "range")])
        self.assertEqual((s[0]["off"], s[0]["isa"], s[0]["reg"], s[0]["stream"]), (0x3373C, "thumb", 5, 1))
        self.assertEqual((s[2]["tai"], s[2]["bytes"], s[3]["bytes"], s[4]["bytes"]),
                         (0x8002D013, "2DE9F04F85B000210091", "F46200240C6F8B490A14695F9000018B", ""))
        self.assertEqual([hr.site_offset(x) for x in s], [0x3373C, 0x3373C, 0xE344, 0x1639A, 0x1196DDA])
        self.assertEqual(sum(i["hooked"] for i in log["imports"]), 7)
        self.assertEqual(log["totals"], {"hooked": 1, "refused": 3, "calls": 7, "imports": 8})
        verdict, steps = hr.assess(log)
        self.assertEqual(verdict, "Rubi's verified entry is installed in this game; 7 string-copy imports hooked")

    def test_what_each_kind_of_log_asks_for(self):
        head = "T0000001: text source 1\n"
        mod = "main module g NID 00000001 seg0 81000000+1000 APP_VER %s; %u verified, %u Luna sites\n"
        tail = imports_lines(8) + "sites hooked %u, refused %u; CALL imports hooked 8 of 8\n"
        cases = [
            (head + mod % ("0101", 0, 0) + tail % (0, 0), "Rubi has no entry for this game", "find a hook"),
            (head + mod % ("0000", 0, 0) + tail % (0, 0), "Rubi has no entry", "give the game's version"),
            (head + mod % ("0101", 1, 0) + "verified +100 thumb r6 refused: signature\n bytes 00112233\n"
             + tail % (0, 1), "refused here (signature): this console runs another build", "needs this build"),
            (head + mod % ("0101", 1, 0) + "verified +100 thumb r6 refused: version\n bytes 00112233\n"
             + tail % (0, 1), "refused here (version)", "needs this build"),
            (head + mod % ("0101", 0, 1) + "luna 80001000 r0 hooked, stream 1\n" + tail % (1, 0),
             "a LunaTranslator site is installed", "Say whether the popup shows the dialogue"),
            (head + mod % ("0101", 0, 2) + "luna 80001000 r0 refused: entry\n bytes 4FF00003\nluna 80002000 r1 "
             "refused: unported\n bytes 00BF00BF\n" + tail % (0, 2), "2 site(s) for this game are refused (entry, "
             "unported)", "bytes this console has"),
            (head + mod % ("0101", 0, 1) + "luna 8119ADDA r7 refused: range\n bytes \n" + tail % (0, 1),
             "refused (range)", "No site is inside this build's segment 0"),
            ("T0000001: text source 1, sites off\nsites off (hook_sites = off): no table site\n" + imports_lines(8)
             + "sites off; CALL imports hooked 8 of 8\n", "table sites were off", "Delete the line"),
            ("PSPEMU004: text source 1\nnothing: the PSP emulator's process\npsp probe 1: start: text source 1\n",
             "a PSP game", "no hook path for PSP games"),
            ("PSPEMU004: text source 1\nthe PSP emulator's process: no hooks; PSP text off\n", "a PSP game", "PSP"),
            (head + mod % ("0101", 1, 0) + "verified +100 thumb r6 tai 80010002\n refused: tai\n bytes 00112233\n"
             + tail % (0, 1), "matches this build but was not installed (tai", "a bug of Rubi"),
            (head + mod % ("0101", 1, 0) + "verified +100 thumb r6 refused: full\n bytes 00112233\n" + tail % (0, 1),
             "was not installed (full", "a bug of Rubi"),
            (head + mod.replace("seg0 81000000", "seg0 82000000") % ("0100", 1, 0)
             + "verified +4DE9E thumb r15 refused: relocated\n bytes 2A94C8F29D100768\n" + tail % (0, 1),
             "segment 0 is not at 0x81000000, where the farm proved it (relocated)", "another way to find its text"),
            (": text source 1\nnothing: no title ID\n", "no game to hook (no title ID)", "PS Vita game"),
        ]
        for text, verdict, step in cases:
            log = hr.parse(text)
            self.assertEqual((log["unknown"], log["warnings"]), ([], []), text)
            v, steps = hr.assess(log)
            self.assertIn(verdict, v, text)
            self.assertTrue(any(step in s for s in steps), (text, steps))
        self.assertEqual(hr.parse(cases[-1][0])["title"], None)
        self.assertEqual(len(hr.parse(cases[8][0])["psp_probe"]), 1)
        # a release that is not Japanese: the farm's entry (from the Japanese release) would not reach it
        for title, said in (("PCSE00001", True), ("PCSB00001", True), ("PCSG00001", False), ("VLJM00001", False)):
            _, steps = hr.assess(hr.parse(cases[0][0].replace("T0000001", title)))
            self.assertEqual(any("is not a Japanese release" in x for x in steps), said, title)
        _, steps = hr.assess(hr.parse(cases[0][0]))
        self.assertTrue(any("not every request gets an entry" in x for x in steps), steps)

    def test_a_log_that_stops_or_does_not_add_up(self):
        cut = a_log().split("luna 8001A39A")[0]  # the game stopped while the worker installed
        log = hr.parse(cut)
        self.assertIn("the log stops before the worker finished", " ".join(log["warnings"]))
        log = hr.parse("T0000001: text source 1\nmain module g NID 00000001 seg0 81000000+1000 APP_VER 0101; "
                       "0 verified, 1 Luna sites\nluna 80001000 r0 tai 80010002\n")
        self.assertIn("has no verdict: the log ends there", " ".join(log["warnings"]))
        log = hr.parse(a_log().replace("sites hooked 1, refused 3", "sites hooked 2, refused 3"))
        self.assertIn("counts 2 sites hooked and 3 refused, its site lines 1 and 3", " ".join(log["warnings"]))
        log = hr.parse(a_log() + "a line from somewhere else\n")
        self.assertEqual(log["unknown"], ["a line from somewhere else"])
        log = hr.parse(a_log() + a_log())
        self.assertIn("a second hooks.log", " ".join(log["warnings"]))
        self.assertIsNone(hr.parse("not a log\n"))
        # taiHEN without the import hook (game_ocr/hooks.c install_calls returns first): no import line, the total
        # still 0 of 8 (the engine test's own log, test_main_module); not a log that stops or does not add up
        log = hr.parse("T0000001: text source 1\nmain module g NID 00000001 seg0 81000000+1000 APP_VER 0000; "
                       "0 verified, 1 Luna sites\nluna 80004201 r0 refused: tai\n bytes 10B56F46\n"
                       "sites hooked 0, refused 1; CALL imports hooked 0 of 8\n")
        self.assertEqual((log["unknown"], log["warnings"], log["imports"]), ([], [], []))
        log = hr.parse(a_log().replace("CALL imports hooked 7 of 8", "CALL imports hooked 6 of 8"))
        self.assertIn("counts 6 of 8 imports hooked, its import lines 7 of 8", " ".join(log["warnings"]))

    def test_the_docs_example_is_a_log_the_worker_writes(self):
        text = DOC.read_text(encoding="utf-8")
        body, strict = hr.extract(text)
        self.assertTrue(strict, "the example is in a fenced block")
        log = hr.parse(body, strict)
        self.assertEqual((log["unknown"], log["warnings"]), ([], []))
        self.assertTrue(log["module"] and log["sites"] and log["totals"])
        for line in body.split("\n"):  # every line of it is a rendering of one of the C formats
            if line:
                self.assertTrue(any(re.fullmatch(fmt_regex(f), line) for f in FORMATS) or
                                any(re.fullmatch(fmt_regex(a) + fmt_regex(b), line) for a in FORMATS for b in FORMATS
                                    if not a.endswith("\n")), line)


def fmt_regex(fmt):
    """A printf format as a regex of what it renders (the specifiers hooks.c uses)."""
    out, i = "", 0
    fmt = fmt.rstrip("\n")
    for m in re.finditer(r"%(\.9s|0[48]X|X|u|s)", fmt):
        out += re.escape(fmt[i:m.start()]) + {"s": ".*", ".9s": "[^\n]{0,9}", "08X": "[0-9A-F]{8}",
                                               "04X": "[0-9A-F]{4}", "X": "[0-9A-F]+", "u": "[0-9]+"}[m.group(1)]
        i = m.end()
    return out + re.escape(fmt[i:])


# ---- the tables and the farm ----

class Tables(unittest.TestCase):
    def test_every_verified_row_is_read(self):
        source = (ROOT / "core/hook_place.c").read_text(encoding="utf-8")
        rows = hr.verified_rows(source)
        array = re.sub(r"/\*.*?\*/", "", source.split("VjoPlaceVerified verified[]")[1].split("\n};")[0], flags=re.S)
        self.assertEqual(len(rows), len(re.findall(r'\{"[A-Z]{4}\d{5}"', array)))
        self.assertGreaterEqual(len(rows), 30)
        clannad = [r for r in rows if r["title"] == "PCSG00415"]
        self.assertEqual(clannad, [{"title": "PCSG00415", "nid": 0xC5187D60, "off": 0x2CA70, "thumb": True, "reg": 6,
                                    "encoding": "CP932", "sig": "ABF82C2314F55370", "app_ver": 0}])
        foaf4 = next(r for r in rows if r["title"] == "PCSG00380")
        self.assertEqual((foaf4["reg"], foaf4["app_ver"], len(foaf4["sig"])), (15, 0x0100, 32))
        # the tool's own comparison: this checkout's entries for CLANNAD, and a console with another build
        tables = hr.rubi_tables("PCSG00415")
        self.assertEqual(len(tables["verified"]), 1)
        log = hr.parse("PCSG00415: text source 1\nmain module c NID 00000001 seg0 81000000+100000 APP_VER 0100; "
                       "1 verified, 0 Luna sites\nverified +2CA70 thumb r6 refused: signature\n bytes 00\n"
                       + imports_lines(8) + "sites hooked 0, refused 1; CALL imports hooked 8 of 8\n")
        _, steps = hr.assess(log, tables)
        self.assertIn("This checkout's entry +0x2CA70 is for module NID 0xC5187D60, this console's is 0x00000001: "
                      "another build.", steps)
        self.assertIsNone(hr.rubi_tables("PCSG00415", ROOT / "tools"))  # no tree: the release zip's tools/

    def test_every_luna_site_is_read(self):
        source = (ROOT / "core/hook_luna_table.inc").read_text(encoding="utf-8")
        sites, ported = map(int, re.search(r"(\d+) sites, (\d+) ported", source).groups())
        rows = hr.luna_rows(source)
        self.assertEqual((len(rows), sum(r["ported"] for r in rows)), (sites, ported))
        tt = [(r["addr"], r["reg"]) for r in rows if r["title"] == "PCSG00062"]
        self.assertEqual(tt, [(0x8119ADDA, 7), (0x811A6052, 8)])

    def test_the_farm_bucket(self):
        log = hr.parse(a_log())
        with tempfile.TemporaryDirectory() as tmp:
            bucket = Path(tmp)
            (bucket / "vita").mkdir()
            self.assertEqual(hr.farm_hook(bucket, "PCSG00628"), False)
            self.assertIn("no folder for this title", " ".join(hr.farm_steps(False, log)))
            hook = bucket / "vita/PCSG00628 a title/hook"
            hook.mkdir(parents=True)
            self.assertEqual(hr.farm_hook(bucket, "PCSG00628"), ("PCSG00628 a title", None))
            farm = {"module_nid": "0x1ee45f02", "app_ver": "01.00", "segment0_offset": "0x1639a", "register": 0,
                    "encoding": "cp932", "signature": "f46200240c6f8b490a14695f9000018b", "status": "emulator-proven",
                    "evidence": {"approved_pairs": 30}}
            (hook / "rubi-hook.json").write_text(json.dumps(farm))
            got = hr.farm_hook(bucket, "PCSG00628")
            self.assertEqual(got[1]["module_nid"], "0x1ee45f02")
            steps = " ".join(hr.farm_steps(got, log))
            self.assertIn("emulator-proven, 30 approved pages", steps)
            self.assertIn("The farm proved this console's build (the same module NID)", steps)
            self.assertIn("At the farm's site this console has F46200240C6F8B490A14695F9000018B: the farm's bytes.",
                          steps)
            farm.update(module_nid="0x12345678", signature="00" * 16)
            (hook / "rubi-hook.json").write_text(json.dumps(farm))
            steps = " ".join(hr.farm_steps(hr.farm_hook(bucket, "PCSG00628"), log))
            self.assertIn("another build (module NID 0x12345678, this console's 0x1EE45F02)", steps)
            self.assertIn("not the farm's 000000", steps)
            del farm["module_nid"]
            (hook / "rubi-hook.json").write_text(json.dumps(farm))
            self.assertIn("recorded no module NID", " ".join(hr.farm_steps(hr.farm_hook(bucket, "PCSG00628"), log)))
            r = subprocess.run([sys.executable, str(TOOL), "--bucket", tmp, "-"], input=a_log(), capture_output=True,
                               text=True, timeout=60)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("The farm recorded no module NID", r.stdout)
            r = subprocess.run([sys.executable, str(TOOL), "--bucket", str(hook), "-"], input=a_log(),
                               capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 2)
            self.assertIn("is not a farm bucket", r.stderr)


# ---- the sources: a file, a collect folder and zip, and the command line ----

LOG_TXT = ("Rubi shell started (kernel API 50)\n"
           "game PCSG00628 started: dictionary local, trigger l+r, subtitles select+r, ocr_mode auto\n"
           "game vocr source: ocr 0, flags 0x00, score 0, streams 0x00000, pick 0\n"
           "game PCSG00999 started: dictionary local, trigger l+r, subtitles select+r, ocr_mode auto\n"
           "game PCSG00628 started: dictionary local, trigger l+r, subtitles select+r, ocr_mode auto\n"
           "hooks in PCSG00628: text source auto, 1 table sites and 7 string copies hooked, 3 sites refused\n"
           "game vocr source: ocr 0, flags 0x00, score 0, streams 0x00201, pick 0\n"
           "game vocr text 0: (the debug build's recognized text: never read)\n"
           "game vocr source: hook 1, flags 0x118, score 0, streams 0x00201, pick 0\n"
           "game vocr source: hook 10, flags 0x03, score 1000, streams 0x00201, pick 0\n")


class Sources(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = Path(tmp.name)
        self.collect = self.dir / "vjo-collect-20261027-101500Z"
        data = self.collect / hr.COLLECT_DIR
        data.mkdir(parents=True)
        (data / "hooks.log").write_text(a_log())
        (data / "log.txt").write_text(LOG_TXT, encoding="utf-8")

    def run_tool(self, *args, stdin=None):
        return subprocess.run([sys.executable, str(TOOL), *map(str, args)], input=stdin, capture_output=True, text=True,
                              timeout=60)

    def test_a_collect_folder_and_its_zip(self):
        r = self.run_tool(self.collect)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("**Verdict:** Rubi's verified entry is installed in this game; a hook answered 2 of the readings "
                      "log.txt names.", r.stdout)
        self.assertIn("job sources: 1 OCR, 1 table site, 1 string copy", r.stdout)  # the last session only
        self.assertNotIn("recognized text", r.stdout)
        zpath = Path(str(self.collect) + ".zip")
        with zipfile.ZipFile(zpath, "w") as z:
            for p in sorted(self.collect.rglob("*")):
                if p.is_file():
                    z.write(p, str(Path(self.collect.name) / p.relative_to(self.collect)))
        j = json.loads(self.run_tool(zpath, "--json").stdout)
        self.assertEqual(j["log_txt"]["streams"], {"1": 1, "10": 1})
        self.assertEqual((j["log_txt"]["sessions"], j["log_txt"]["ocr"]), (2, 1))
        self.assertTrue(j["source"].endswith(".zip:vjo-collect-20261027-101500Z/ux0/data/VitaJPOverlay/hooks.log"))
        # the folder the user copied off the card, hooks.log in it; and the file itself
        self.assertEqual(self.run_tool(self.collect / hr.COLLECT_DIR).returncode, 0)
        self.assertEqual(self.run_tool(self.collect / hr.COLLECT_DIR / "hooks.log").returncode, 0)

    def test_out_writes_the_report_and_prints_one_line(self):
        out = self.dir / "hook-request.md"
        r = self.run_tool(self.collect, "--out", out)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.count("\n"), 1)
        self.assertTrue(r.stdout.startswith("hooks.log: PCSG00628, APP_VER 01.00, main module adv (NID 0x1EE45F02): "
                                            "Rubi's verified entry is installed in this game"), r.stdout)
        self.assertIn(f"Report: {out}. To ask for a hook for this game: {hr.DOC}", r.stdout)
        self.assertTrue(out.read_text(encoding="utf-8").startswith("# Hook request: PCSG00628\n"))
        r = self.run_tool(self.collect, "--out", out, "--json")
        self.assertEqual(json.loads(out.read_text(encoding="utf-8"))["module"]["name"], "adv")

    def test_what_has_no_log(self):
        normal = self.dir / "normal"
        (normal / hr.COLLECT_DIR).mkdir(parents=True)  # the normal build's collect: no hooks.log
        empty = self.dir / "empty.log"
        empty.write_bytes(b"")
        binary = self.dir / "a.png"
        binary.write_bytes(bytes(range(256)) * 4)
        for source in (normal, empty, binary, self.dir / "missing.log"):
            r = self.run_tool(source)
            self.assertEqual((r.returncode, r.stdout), (2, ""), source)
            self.assertTrue(r.stderr.startswith("hook_request: "), r.stderr)
            self.assertNotIn("Traceback", r.stderr)
        zpath = self.dir / "other.zip"
        with zipfile.ZipFile(zpath, "w") as z:
            z.writestr("x/log.txt", "nothing")
        self.assertEqual(self.run_tool(zpath).returncode, 2)

    def test_what_cannot_be_read_exits_2_with_a_message(self):
        """A collect zip whose hooks.log fails its CRC, --out into a folder that is not there, and a farm bucket whose
        rubi-hook.json is not one: a message and exit 2, never a traceback (audit-rubi-358-hooktemplate F7)."""
        damaged = self.dir / "damaged.zip"
        with zipfile.ZipFile(damaged, "w", zipfile.ZIP_STORED) as z:
            z.writestr(f"c/{hr.COLLECT_DIR}/hooks.log", a_log())
        data = bytearray(damaged.read_bytes())
        at = data.index(b"PCSG00628: text source 1")
        data[at] ^= 0x01  # its bytes, not its header: the CRC check fails as it is read
        damaged.write_bytes(bytes(data))
        bucket = self.dir / "bucket"
        hook = bucket / "vita/PCSG00628 a title/hook"
        hook.mkdir(parents=True)
        cases = [(None, (damaged,), "a damaged zip"),
                 (None, (self.collect, "--out", self.dir / "no/such/folder/hook-request.md"), "cannot write"),
                 ("{not json", (self.collect, "--bucket", bucket), "not a rubi-hook.json (Expecting"),
                 ("[1, 2]", (self.collect, "--bucket", bucket), "not a rubi-hook.json (a JSON list")]
        for body, args, said in cases:
            if body is not None:
                (hook / "rubi-hook.json").write_text(body)
            r = self.run_tool(*args)
            self.assertEqual((r.returncode, r.stdout), (2, ""), (args, r.stderr))
            self.assertTrue(r.stderr.startswith("hook_request: ") and said in r.stderr, r.stderr)
            self.assertNotIn("Traceback", r.stderr)

    def test_a_hooks_log_older_than_the_last_game_start(self):
        """hooks.log has no time, and the normal build (or a game that is OFF, or OCR only) writes none and removes
        none: log.txt says when it may be an earlier session's (audit-rubi-358-hooktemplate N9)."""
        log = self.collect / hr.COLLECT_DIR / "log.txt"
        for text, said in ((LOG_TXT, None),
                           (LOG_TXT + "game PCSG00999 started: dictionary local, trigger l+r\n",
                            "log.txt's last game start is PCSG00999's, not PCSG00628's"),
                           (LOG_TXT + "Rubi shell started (kernel API 51, the normal build: a short log)\n",
                            "log.txt's last boot is the normal build's"),
                           (LOG_TXT.replace("PCSG00628", "PCSG00777"), "log.txt has no start of PCSG00628")):
            log.write_text(text, encoding="utf-8")
            steps = json.loads(self.run_tool(self.collect, "--json").stdout)["next"]
            stale = [x for x in steps if x.startswith("log.txt")]
            if said is None:
                self.assertEqual(stale, [], steps)
            else:
                self.assertTrue(stale and said in stale[0], steps)

    def test_the_main_function_in_process(self):
        with redirect_stdout(io.StringIO()) as out, redirect_stderr(io.StringIO()):
            self.assertEqual(hr.main([str(self.collect / hr.COLLECT_DIR / "hooks.log")]), 0)
        self.assertIn("| Luna site 0x8001A39A (r0) | refused: window, an instruction taiHEN would move is not on the "
                      "placement check's allow-list | F46200240C6F8B490A14695F9000018B |", out.getvalue())


class Package(unittest.TestCase):
    def test_the_release_ships_the_tool_and_its_doc(self):
        """install_ftp.sh --collect runs tools/hook_request.py from the package: the release zip has it, and the doc
        the guide links (vita/CMakeLists.txt's release target)."""
        cmake = (ROOT / "vita/CMakeLists.txt").read_text(encoding="utf-8")
        tools = [x for x in cmake.splitlines() if " ${ROOT}/tools/collect_vita.py " in x]
        self.assertEqual(len(tools), 1)
        self.assertIn(" ${ROOT}/tools/hook_request.py ", tools[0])
        self.assertTrue(tools[0].rstrip().endswith(" ${STAGE}/tools/"))
        docs = cmake.split("Keep the guide's feature and recovery links usable")[1].split("${STAGE}/docs/")[0]
        self.assertIn("${ROOT}/docs/hook-requests.md", docs)
        # the packaged tool needs nothing the package lacks: no import of the repository's other tools
        imports = set(re.findall(r"^import (\w+)|^from (\w+)", TOOL.read_text(encoding="utf-8"), re.M))
        self.assertEqual({a or b for a, b in imports}, {"argparse", "json", "re", "sys", "zipfile", "zlib", "pathlib"})
        script = (ROOT / "tools/install_ftp.sh").read_text(encoding="utf-8")
        self.assertIn('python3 tools/hook_request.py "$OUT" --out "$OUT/hook-request.md"', script)


if __name__ == "__main__":
    unittest.main()
