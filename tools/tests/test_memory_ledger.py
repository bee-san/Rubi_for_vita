"""tools/check_memory_ledger.py: the ledger check must pass on this tree and
fail on each kind of regression it exists for."""
import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/check_memory_ledger.py"
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools/tests"))
import check_memory_ledger as ledger  # noqa: E402
from self_fixture import Segment, fself  # noqa: E402


def run(*args):
    return subprocess.run([sys.executable, str(SCRIPT), *args], capture_output=True, text=True)


def fake_self(memsz_list):
    """A SELF of one PT_LOAD segment per (memsz, flags), each all .bss (no file bytes): only their pages count."""
    return fself(*(Segment(memsz=memsz, flags=flags) for memsz, flags in memsz_list))


class MemoryLedgerCheck(unittest.TestCase):
    def copy_tree(self):
        tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, tmp, True)
        for d in ledger.SCAN_DIRS:
            shutil.copytree(ROOT / d, tmp / d, ignore=shutil.ignore_patterns("*.o", "*.obj"))
        (tmp / "tools").mkdir()
        shutil.copyfile(SCRIPT, tmp / "tools/check_memory_ledger.py")
        return tmp

    def test_this_tree_passes_and_prints_the_table(self):
        result = run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("memory ledger: pass", result.stdout)
        for pool in ("shell pages", "shell paf", "kernel", "game"):
            self.assertIn(pool, result.stdout)
        # 10 since #138's games.ini read for a starting game (kernel/loader.c policy_alloc)
        self.assertIn("allocation sites: 10, all in ledger wrappers", result.stdout)  # includes the file worker stack
        self.assertIn("kernel/loader.c:", result.stdout)
        as_json = json.loads(run("--json").stdout)
        self.assertEqual(as_json["result"], "pass")
        # one configuration since the network paths (Google Lens, the Hachidori relay) were removed:
        # no opt-in (soft) row
        self.assertEqual({p["pool"] for p in as_json["pools"]}, {"shell pages", "shell paf", "kernel", "game"})
        for pool in as_json["pools"]:
            self.assertLessEqual(pool["worst"], pool["limit"])
        self.assertEqual(as_json["warnings"], [])
        self.assertNotIn("WARNING", result.stderr)
        paf = next(p for p in as_json["pools"] if p["pool"] == "shell paf")
        # the Paf plugin, the result arena, an Anki save, the overlay's text (API 46: with the cursor's copy of it and
        # its char map, 4,096 B, out of the Shell's data pages) and a line-history visit
        self.assertEqual(paf["worst"], 8192 + 65536 + 131072 + 24576 + 16384)
        self.assertEqual(paf["items"]["VJO_BUDGET_PAF_HISTORY"], 16384)

    def test_the_kernel_worst_case_is_its_items(self):
        # API 24: the module's pages, the worker's stack, one kernel strip and one games.ini read, both builds;
        # the strip's budget holds the largest strip a rows call can take (include/vjo_game_ocr.h)
        kstrip_max = int(re.search(r"#define VJO_GAME_OCR_KSTRIP_MAX \((\d+)u \* (\d+)u\)",
                                   (ROOT / "include/vjo_game_ocr.h").read_text()).group(1)) * 1024
        for variant in ("debug", "normal"):
            kernel = next(p for p in json.loads(run("--json", "--variant", variant).stdout)["pools"]
                          if p["pool"] == "kernel")
            self.assertEqual(set(kernel["items"]), {"VJO_BUDGET_KERNEL_MODULE_PAGES", "VJO_BUDGET_KERNEL_STACK",
                                                    "VJO_BUDGET_KERNEL_IO_STACK", "VJO_BUDGET_KERNEL_OCR_STRIP", "VJO_BUDGET_KERNEL_POLICY"}, variant)
            self.assertEqual(kernel["worst"], sum(kernel["items"].values()), variant)
            self.assertEqual(kernel["worst"] - kernel["permanent"], kernel["items"]["VJO_BUDGET_KERNEL_OCR_STRIP"] +
                             kernel["items"]["VJO_BUDGET_KERNEL_POLICY"], variant)
            self.assertGreaterEqual(kernel["items"]["VJO_BUDGET_KERNEL_OCR_STRIP"], kstrip_max, variant)

    def test_a_picture_card_fits_the_only_configuration(self):
        # a 384 KiB Anki arena (the removed screenshot card's) put the network configuration over the
        # limit by 8,688 B (a warning) while it existed; beside the one 64 KiB arena it fits: 507,904 B
        # with a line-history visit's 16 KiB and API 46's larger overlay block (487,424 B before line history)
        tmp = self.copy_tree()
        budget = tmp / "include/vjo_ledger_budget.h"
        budget.write_text(re.sub(r"#define VJO_BUDGET_PAF_ANKI \d+u", "#define VJO_BUDGET_PAF_ANKI 393216u",
                                 budget.read_text()))
        result = run("--root", str(tmp), "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        as_json = json.loads(result.stdout)
        self.assertEqual(as_json["warnings"], [])
        paf = next(p for p in as_json["pools"] if p["pool"] == "shell paf")
        self.assertEqual(paf["worst"], 8192 + 65536 + 393216 + 24576 + 16384)

    def test_an_unaccounted_allocation_fails(self):
        tmp = self.copy_tree()
        worker = tmp / "shell/worker.c"
        worker.write_text(worker.read_text() + "\nstatic int sneaky(void) { return sceKernelAllocMemBlock(\"x\", 0, 4096, NULL); }\n")
        result = run("--root", str(tmp))
        self.assertEqual(result.returncode, 1)
        self.assertIn("sceKernelAllocMemBlock at shell/worker.c:", result.stderr)
        self.assertIn("route it through a ledger wrapper", result.stderr)
        # a thread, a PAF allocation and a libc malloc are caught too, also in C++ and in core/
        for rel, call in (("kernel/scene.c", "ksceKernelCreateThread(\"t\", 0, 0, 0, 0, 0, NULL)"),
                          ("shell/overlay.cpp", "sce_paf_malloc(16)"), ("core/arena.c", "malloc(16)"),
                          ("shell/strip.cpp", "new int[4]")):
            tmp = self.copy_tree()
            path = tmp / rel
            path.write_text(path.read_text() + f"\nstatic void *sneaky_{path.stem}(void) {{ return (void *){call}; }}\n")
            result = run("--root", str(tmp))
            self.assertEqual(result.returncode, 1, rel)
            self.assertIn(rel, result.stderr)

    def test_a_call_inside_a_comment_or_string_is_not_a_site(self):
        tmp = self.copy_tree()
        worker = tmp / "shell/worker.c"
        worker.write_text(worker.read_text() + "\n/* sceKernelAllocMemBlock(\"x\", 0, 1, NULL) */\n"
                          "// malloc(3)\nstatic const char *note = \"sceKernelCreateThread(\";\n")
        result = run("--root", str(tmp))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_a_removed_wrapper_fails(self):
        tmp = self.copy_tree()
        path = tmp / "shell/ledger.c"
        path.write_text(path.read_text().replace("sceKernelCreateThread(", "sceKernelCreateThreadX("))
        result = run("--root", str(tmp))
        self.assertEqual(result.returncode, 1)
        self.assertIn("no longer holds its sceKernelCreateThread wrapper", result.stderr)

    def test_a_worst_case_over_its_limit_fails(self):
        tmp = self.copy_tree()
        budget = tmp / "include/vjo_ledger_budget.h"
        text = budget.read_text()
        text = re.sub(r"#define VJO_BUDGET_PAF_ANKI \d+u", "#define VJO_BUDGET_PAF_ANKI 1048576u", text)
        budget.write_text(text)
        result = run("--root", str(tmp))
        self.assertEqual(result.returncode, 1)
        # (API 46's overlay block: 4,096 B more than before)
        self.assertRegex(result.stderr, r"pool shell paf: worst case 1,163,264 B exceeds its limit 985,088 B by 178,176 B")
        # a per-item limit too
        tmp = self.copy_tree()
        budget = tmp / "include/vjo_ledger_budget.h"
        # one page over the Large size's 2.5 MB weights ceiling
        budget.write_text(re.sub(r"#define VJO_BUDGET_GAME_WEIGHTS \d+u", "#define VJO_BUDGET_GAME_WEIGHTS 2506752u", budget.read_text()))
        result = run("--root", str(tmp))
        self.assertEqual(result.returncode, 1)
        self.assertIn("VJO_BUDGET_GAME_WEIGHTS 2,506,752 B exceeds VJO_LIMIT_GAME_WEIGHTS 2,502,656 B", result.stderr)

    def test_a_budget_header_that_is_not_arithmetic_is_rejected(self):
        tmp = self.copy_tree()
        budget = tmp / "include/vjo_ledger_budget.h"
        budget.write_text(budget.read_text() + "\n#define VJO_SNEAKY (sizeof(int))\n")
        result = run("--root", str(tmp))
        self.assertEqual(result.returncode, 2)
        self.assertIn("only integer arithmetic", result.stderr)

    def test_module_pages_over_the_declared_budget_fail(self):
        tmp = self.copy_tree()
        values = ledger.parse_budget(ROOT / "include/vjo_ledger_budget.h")
        shell_budget = values["VJO_BUDGET_SHELL_MODULE_PAGES"]
        rw = 73_728  # the data/bss segment's pages (the packaged module's); the text segment takes the rest
        self.assertGreater(shell_budget - rw, 4096)
        text = shell_budget - rw
        fits = tmp / "shell.suprx"
        fits.write_bytes(fake_self([(text - 40, 5), (rw - 1_000, 6)]))
        result = run("--root", str(tmp), "--shell", str(fits))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"{shell_budget:,} B declared", result.stdout)
        grown = tmp / "shell_grown.suprx"
        grown.write_bytes(fake_self([(text + 1, 5), (rw - 1_000, 6)]))  # one byte into the next page
        result = run("--root", str(tmp), "--shell", str(grown))
        self.assertEqual(result.returncode, 1)
        self.assertIn(f"VitaJPOverlay_Shell: {shell_budget + 4096:,} B of pages, budget VJO_BUDGET_SHELL_MODULE_PAGES is {shell_budget:,} B", result.stderr)
        kernel_budget = values["VJO_BUDGET_KERNEL_MODULE_PAGES"]
        kernel = tmp / "kernel.skprx"
        kernel.write_bytes(fake_self([(32_768, 5), (kernel_budget - 32_768 + 1, 6)]))
        result = run("--root", str(tmp), "--kernel", str(kernel))
        self.assertEqual(result.returncode, 1)
        self.assertIn(f"VitaJPOverlay_Kernel: {kernel_budget + 4096:,} B of pages", result.stderr)
        not_self = tmp / "junk.suprx"
        not_self.write_bytes(b"ELF junk")
        result = run("--root", str(tmp), "--worker", str(not_self))
        self.assertEqual(result.returncode, 2)
        self.assertIn("not a SELF", result.stderr)

    def test_the_real_packaged_modules_when_present(self):
        built = ROOT / "build/vita"
        paths = {k: built / n for k, n in (("kernel", "VitaJPOverlay_Kernel.skprx"), ("shell", "VitaJPOverlay_Shell.suprx"),
                                            ("worker", "VitaJPOverlay_OCR.suprx"))}
        if not all(p.is_file() for p in paths.values()):
            self.skipTest("no Vita build in build/vita")
        result = run(*[a for k, p in paths.items() for a in (f"--{k}", str(p))])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
