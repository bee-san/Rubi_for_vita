"""The Shell's Paf policy for the overlay (#528, docs/memory-ledger.md "shell paf"), where no host test reaches it:
shell/worker.c vjo_worker_start is not run on the host (its threads), so these read the sources. The overlay's text
block must be taken at the end of a successful start, before the boot ledger line (so `ledger boot paf: current=`
shows it), and never given back; the page must be checked for room before PageOpen. The behaviour behind each call
is the host tests' (shell_worker_paf: the owner scenario, overlay_page_needs_its_own_room)."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from check_memory_ledger import strip_comments  # noqa: E402  (comments and string contents out, lines kept)


def source(rel):
    return strip_comments((ROOT / rel).read_text(encoding="utf-8"))


def function_body(text, signature):
    """The body of the C function whose definition starts with `signature`, braces matched."""
    start = text.index(signature)
    i = text.index("{", start)
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[i:j + 1]
    raise AssertionError(f"unbalanced braces after {signature}")


class OverlayPafPolicy(unittest.TestCase):
    def test_the_worker_start_takes_the_text_block_before_its_boot_line(self):
        body = function_body(source("shell/worker.c"), "int vjo_worker_start(void)")
        take = body.find("vjo_overlay_boot();")
        self.assertGreaterEqual(take, 0, "vjo_worker_start no longer takes the overlay's text block (#528)")
        self.assertEqual(body.count("vjo_overlay_boot();"), 1)
        # after both threads started (a failed start takes nothing), before the boot ledger line
        self.assertGreater(take, body.index("threads_started |= 2;"))
        self.assertLess(take, body.index('vjo_ledger_shell_log("");'))  # "boot", its string emptied
        # and the start's last failure exit comes before it
        self.assertLess(body.rfind("return -1;"), take)

    def test_overlay_cpp_routes_the_block_through_the_seam(self):
        overlay = source("shell/overlay.cpp")
        self.assertIn("vjo_overlay_mem_boot(&s_ui_mem);", function_body(overlay, 'extern "" void vjo_overlay_boot(void)'))
        open_page = function_body(overlay, "static void open_page(void)")
        self.assertLess(open_page.index("vjo_overlay_mem_open(&s_ui_mem)"), open_page.index("PageOpen("))
        # the block is the plugin's: no free of it anywhere in the Shell's code
        for path in sorted((ROOT / "shell").glob("*.c*")) + sorted((ROOT / "shell").glob("*.h")):
            text = strip_comments(path.read_text(encoding="utf-8"))
            self.assertIsNone(re.search(r"vjo_shell_heap_free\(\s*VJO_SL_PAF_UI", text), path.name)

    def test_the_seam_keeps_the_block_and_checks_the_page(self):
        seam = source("shell/overlay_mem.h")
        close = function_body(seam, "static inline void vjo_overlay_mem_close(uint8_t **mem)")
        self.assertNotIn("vjo_shell_heap_free", close)
        self.assertIn("vjo_paf_page_fits(vjo_shell_heap_free_size())",
                      function_body(seam, "static inline uint8_t *vjo_overlay_mem_open(uint8_t **mem)"))


if __name__ == "__main__":
    unittest.main()
