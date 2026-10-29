"""The Shell's Paf policy for the overlay (#528, docs/memory-ledger.md "shell paf"), where no host test reaches it:
shell/worker.c vjo_worker_start is not run on the host (its threads) and shell/overlay.cpp is not built there, so these
read the sources' statements. The overlay's text block must be taken at the end of a successful start, before the boot
ledger line (so `ledger boot paf: current=` shows it), and never given back; the page must be checked for room, and
not opened when the check refuses, before PageOpen. The behaviour behind each call is the host tests' (shell_worker_paf:
the owner scenario, overlay_page_needs_its_own_room). audit-mem-paf-2: statements, not text, so a call under `if (0)`
or `#if 0`, or a check whose answer is ignored, fails, and spacing or a declaration does not."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from check_memory_ledger import strip_comments  # noqa: E402  (comments and string contents out, lines kept)


def source(rel):
    return strip_comments((ROOT / rel).read_text(encoding="utf-8"))


def definition(text, name):
    """The body of the C function `name`'s definition (a declaration is skipped), braces matched, with its offset."""
    for m in re.finditer(r"\b" + re.escape(name) + r"\s*\([^;{}()]*\)\s*\{", text):
        start = m.end() - 1
        depth = 0
        for j in range(start, len(text)):
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth == 0:
                    return text[start:j + 1]
    raise AssertionError(f"no definition of {name}")


def statements(body):
    """The statements directly in a function body, in order, whitespace removed: none inside a nested block or a
    preprocessor conditional (#if, #ifdef, #ifndef ... #endif), and an `if (...) x;` stays one statement."""
    out, cur = [], []
    depth = parens = conditional = 0
    for line in body[1:-1].split("\n"):
        directive = line.strip()
        if directive.startswith("#"):
            word = directive[1:].strip()
            if word.startswith(("if", "ifdef", "ifndef")):
                conditional += 1
            elif word.startswith("endif"):
                conditional -= 1
            continue
        for ch in line + "\n":
            if conditional:
                continue
            if ch == "{":
                depth += 1
                if depth == 1 and parens == 0:
                    cur.append("{")
                continue
            if ch == "}":
                depth -= 1
                if depth == 0 and parens == 0:
                    out.append("".join(cur) + "}")
                    cur = []
                continue
            if depth:
                continue
            if ch == "(":
                parens += 1
            elif ch == ")":
                parens -= 1
            if ch == ";" and parens == 0:
                out.append("".join(cur))
                cur = []
            elif not ch.isspace():
                cur.append(ch)
    return [s for s in out if s]


def index_of(stmts, pattern):
    """The index of the one statement matching `pattern` (a regex on the whitespace-free text), else a failure."""
    found = [i for i, s in enumerate(stmts) if re.fullmatch(pattern, s)]
    if len(found) != 1:
        raise AssertionError(f"{len(found)} statements match {pattern!r}: {stmts}")
    return found[0]


class OverlayPafPolicy(unittest.TestCase):
    def test_the_worker_start_takes_the_text_block_before_its_boot_line(self):
        body = definition(source("shell/worker.c"), "vjo_worker_start")
        stmts = statements(body)
        take = index_of(stmts, r"vjo_overlay_boot\(\)")
        # after both threads started (a failed start takes nothing), before the boot ledger line ("boot", emptied)
        self.assertGreater(take, index_of(stmts, r"threads_started\|=2"))
        self.assertLess(take, index_of(stmts, r'vjo_ledger_shell_log\(""\)'))
        # and after every failure exit of the start, wherever it is nested
        flat = re.sub(r"\s+", "", body)
        call = flat.index("vjo_overlay_boot();")
        self.assertTrue(all(m.start() < call for m in re.finditer(r"return-1;", flat)), "a failure exit after the take")

    def test_overlay_cpp_routes_the_block_through_the_seam(self):
        overlay = source("shell/overlay.cpp")
        self.assertIn("vjo_overlay_mem_boot(&s_ui_mem)", statements(definition(overlay, "vjo_overlay_boot")))
        stmts = statements(definition(overlay, "open_page"))
        refused = index_of(stmts, r"if\(!vjo_overlay_mem_open\(&s_ui_mem\)\)return")
        opened = index_of(stmts, r'.*PageOpen\("",param\)')
        self.assertLess(refused, opened, "open_page must return when the seam refuses, before PageOpen")
        # the block is the plugin's: no free of it anywhere in the Shell's code
        for path in sorted((ROOT / "shell").glob("*.c*")) + sorted((ROOT / "shell").glob("*.h")):
            text = strip_comments(path.read_text(encoding="utf-8"))
            self.assertIsNone(re.search(r"vjo_shell_heap_free\s*\(\s*VJO_SL_PAF_UI\b", text), path.name)

    def test_the_seam_keeps_the_block_and_checks_the_page(self):
        seam = source("shell/overlay_mem.h")
        self.assertNotIn("vjo_shell_heap_free(", re.sub(r"\s+", "", definition(seam, "vjo_overlay_mem_close")))
        opening = re.sub(r"\s+", "", definition(seam, "vjo_overlay_mem_open"))
        self.assertIn("vjo_paf_page_fits(", opening)
        self.assertIn("vjo_shell_heap_free_size()", opening)

    def test_the_reader_sees_statements(self):
        # the parser itself, on the forms the mutants and harmless edits of audit-mem-paf-2 used
        body = "{\n  a( );\n  if (0) b();\n#if 0\n  c();\n#endif\n  if (x) { d(); }\n  for (;;) e();\n  f(\"\");\n}"
        self.assertEqual(statements(body), ["a()", "if(0)b()", "if(x){}", "for(;;)e()", 'f("")'])
        text = "int\nvjo_worker_start(void);\nint\nvjo_worker_start (void)\n{\n  g();\n}\n"
        self.assertEqual(statements(definition(text, "vjo_worker_start")), ["g()"])


if __name__ == "__main__":
    unittest.main()
