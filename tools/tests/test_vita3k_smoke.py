"""tools/vita3k_smoke, the Vita3K smoke app (the game worker's job path as a Vita homebrew app), builds the game worker
as vita/CMakeLists.txt links it, beside its own stand-ins for the kernel's exports. Only tools/vita3k_smoke.sh builds
it, so when the worker grew (a source file, a runtime file) the app broke unseen: from #396's text spotter until
#493 (audit-rubi-cleanup-6 F5, audit-rubi-cleanup-8 F10). These checks read the build files, so ctest catches that
drift; the script builds the app before it looks for Vita3K or the weights, and tools/preflight.sh runs it with VitaSDK
alone, so the link (the stand-ins) is checked wherever VitaSDK is (audit-rubi-cleanup-8-b F4)."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SMOKE_DIR = "tools/vita3k_smoke"
# the worker's sources the app leaves out, and why
NOT_IN_THE_APP = {
    "core/hook_profile.c": "startup profiles use the real kernel file broker; the smoke app has no native game",
    "shell/libc_shim.c": "the plugins' libc (they link no newlib); the app links newlib",
    "game_ocr/psp_probe.c": "the debug worker's PSP probe; under VJO_GAME_OCR_NO_MODULE_ENTRY its call is nothing "
                            "(game_ocr/psp_probe.h)",
    "core/psp_probe.c": "the PSP probe's reader, as game_ocr/psp_probe.c",
}
# each build file's names for a directory, and what they stand for
PREFIXES = {"${ROOT}/": "", "${VJO_ROOT}/": "", "${RT}/": "runtime:", "${_vocr_rt}/": "runtime:",
            "${SPOT_RT}/": "spotter:", "${_vocr_spot_rt}/": "spotter:"}


def sources(path, command, target):
    """The source files of PATH's `COMMAND(TARGET ...)` calls (every one), each as the tree's path, or
    runtime:/spotter: and the runtime file's name; the smoke's own (no prefix) are under its directory."""
    text = re.sub(r"#[^\n]*", "", (ROOT / path).read_text(encoding="utf-8"))  # (no "#" in these calls' words)
    found = set()
    calls = re.findall(rf"\b{command}\(\s*{re.escape(target)}\s(.*?)\)", text, re.S)
    assert calls, (path, command, target)
    for body in calls:
        for word in body.replace('"', " ").split():
            if not re.search(r"\.(c|cpp|S)$", word):
                continue
            for prefix, name in PREFIXES.items():
                if word.startswith(prefix):
                    found.add(name + word[len(prefix):])
                    break
            else:
                assert "$" not in word, (path, word)
                found.add(f"{SMOKE_DIR}/{word}")
    return found


class SmokeApp(unittest.TestCase):
    def test_the_app_builds_the_workers_sources(self):
        """Every source of the worker (its add_executable and target_sources, and cmake/VocrOCR.cmake's vjo_vocr
        library it links) is one of the app's, but the three it leaves out, which the worker does have."""
        worker = (sources("vita/CMakeLists.txt", "add_executable", "VitaJPOverlay_OCR")
                  | sources("vita/CMakeLists.txt", "target_sources", "VitaJPOverlay_OCR")
                  | sources("cmake/VocrOCR.cmake", "add_library", "vjo_vocr"))
        app = sources(f"{SMOKE_DIR}/CMakeLists.txt", "add_executable", "vjo_smoke")
        for name in NOT_IN_THE_APP:
            self.assertIn(name, worker, f"{name} is not the worker's any more: drop it from NOT_IN_THE_APP")
        self.assertEqual(sorted(worker - set(NOT_IN_THE_APP) - app), [],
                         f"sources the worker builds and {SMOKE_DIR}/CMakeLists.txt does not")
        self.assertEqual(sorted(app & set(NOT_IN_THE_APP)), [])
        self.assertIn(f"{SMOKE_DIR}/main.c", app)  # the kernel's stand-ins

    def test_the_app_builds_the_runtime_as_the_worker_does(self):
        """The app's OCR runtime library is cmake/VocrOCR.cmake's vjo_vocr_rt, file for file (the text spotter's
        from its own pin)."""
        self.assertEqual(sorted(sources(f"{SMOKE_DIR}/CMakeLists.txt", "add_library", "smoke_vocr_rt")),
                         sorted(sources("cmake/VocrOCR.cmake", "add_library", "vjo_vocr_rt")))

    def test_the_app_builds_where_vitasdk_is(self):
        """tools/vita3k_smoke.sh builds the app before it looks for the release weights, Vita3K or xvfb-run, and
        tools/preflight.sh runs it with VITASDK alone: the app builds wherever VitaSDK and the runtimes are."""
        script = (ROOT / "tools/vita3k_smoke.sh").read_text(encoding="utf-8")
        build = script.index('cmake --build "$build"')
        for later in ('skip "(${built}no release weights', 'skip "(${built}no Vita3K build', "command -v xvfb-run"):
            self.assertLess(build, script.index(later), later)
        self.assertLess(script.index('skip "(no VitaSDK: set VITASDK)"'), build)
        preflight = (ROOT / "tools/preflight.sh").read_text(encoding="utf-8")
        self.assertIn('if [ -n "${VITA3K:-}" ] || [ -n "${VITASDK:-}" ]; then', preflight)
        self.assertLess(preflight.index('if [ -n "${VITA3K:-}" ] || [ -n "${VITASDK:-}" ]; then'),
                        preflight.index("bash tools/vita3k_smoke.sh"))


if __name__ == "__main__":
    unittest.main()
