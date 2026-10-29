"""Exercise the prototype with Rubi's real external-profile parser and filter VM."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import deltarune_hook as hook


class DeltaruneHook(unittest.TestCase):
    def archive(self):
        blob = hook.profile(bytes.fromhex("ab" * 32))
        return b"vjohkdb1" + struct.pack("<II12sII", 1, 0, b"DELT00001", 36, len(blob)) + blob

    def test_profile_parses_and_filters_with_shipped_worker_code(self):
        source = r'''
#include "hook_profile.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    VjoHookProfile p = {0};
    unsigned char code[16] = {0x70,0xb5,0x04,0x46,0x08,0x46,0x0d,0x46};
    FILE *f; size_t n; const VjoPlaceVerified *v;
    assert(argc == 2); f = fopen(argv[1], "rb"); assert(f);
    n = fread(p.raw, 1, sizeof(p.raw), f); fclose(f);
    assert(vjo_hook_profile_parse(&p, "DELT00001", (unsigned)n));
    assert(vjo_place_verified("DELT00001", &v) == 1);
    assert(v->reg == 1 && v->encoding == VJO_HOOK_ENC_UTF8 && v->app_ver == 0x73);
    assert(v->read == VJO_PLACE_READ_DELTARUNE);
    VjoPlaceVerified local = *v; local.off = 0;
    assert(vjo_place_check_verified(&local, code, sizeof(code), 0x81000000, 0x8d433617) == VJO_PLACE_OK);
    assert(vjo_place_check_verified(&local, code, sizeof(code), 0x81000000, 0x8d433616) == VJO_PLACE_SIGNATURE);
    code[0] ^= 1;
    assert(vjo_place_check_verified(&local, code, sizeof(code), 0x81000000, 0x8d433617) == VJO_PLACE_SIGNATURE);
    assert(!vjo_place_verified_version(v, 0x74));
    const char *raw[] = {"* こんにちは。/", "\\E1* 本文^2&次の行。/%", "メニュー", "* English./", "* 日本語の/区切り。/", "\\E2 ＊ 本文^4&次。/", " ＊ 日本語。/%"};
    const char *want[] = {"こんにちは。", "本文\n次の行。", "", "", "日本語の/区切り。", "本文\n次。", "日本語。"};
    for (unsigned i = 0; i < sizeof(raw) / sizeof(raw[0]); i++) {
        char out[1024];
        size_t got = vjo_luna_text_filter(NULL, v->filter, v->encoding,
            (const uint8_t *)raw[i], (unsigned)strlen(raw[i]), VJO_HOOK_ENC_UTF8, out, sizeof(out));
        if (strcmp(out, want[i])) fprintf(stderr, "case %u: got '%s', want '%s'\n", i, out, want[i]);
        assert(got == strlen(want[i]) && !strcmp(out, want[i]));
    }
    p.raw[72 + 14] = VJO_HOOK_ENC_CP932;
    uint32_t sum = vjo_hook_profile_checksum(p.raw + 72, (unsigned)n - 72);
    for (unsigned i = 0; i < 4; i++) p.raw[68 + i] = (uint8_t)(sum >> (8 * i));
    assert(!vjo_hook_profile_parse(&p, "DELT00001", (unsigned)n));
    p.raw[72] ^= 1;
    assert(!vjo_hook_profile_parse(&p, "DELT00001", (unsigned)n));
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="rubi-deltarune-test-") as temporary:
            directory = Path(temporary)
            (directory / "profile.bin").write_bytes(hook.profile(bytes.fromhex("ab" * 32)))
            (directory / "check.c").write_text(source)
            sources = ["core/hook_profile.c", "core/hook_luna.c", "core/hook_place.c", "core/hook_re.c", "core/hook_text.c"]
            subprocess.run(["cc", "-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-DVJO_EXTERNAL_HOOK_PROFILES=1", '-DVJO_HOOK_DEFINITION_SHA256="' + "ab" * 32 + '"',
                            "-I", str(ROOT / "core"), str(directory / "check.c"),
                            *[str(ROOT / path) for path in sources], "-o", str(directory / "check")], check=True)
            subprocess.run([str(directory / "check"), str(directory / "profile.bin")], check=True)

    def test_catalogue_checks_checksum_and_exact_extent(self):
        archive = self.archive()
        entries, revision = hook.catalogue(archive)
        self.assertEqual(hook.catalogue(bytearray(archive)), (entries, revision))
        self.assertEqual(revision, bytes.fromhex("ab" * 32))
        self.assertEqual(set(entries), {"DELT00001"})
        for invalid in (archive[:-1], archive + b"x", archive[:-1] + bytes([archive[-1] ^ 1])):
            with self.assertRaises(ValueError):
                hook.catalogue(invalid)

    def test_wrong_eboot_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "pinned"):
            hook.prepare(self.archive(), b"different game or build")

    def test_revision_width_is_checked(self):
        with self.assertRaises(ValueError):
            hook.profile(bytes(31))

    def test_receipt_collision_does_not_create_archive(self):
        with tempfile.TemporaryDirectory(prefix="rubi-deltarune-output-") as temporary:
            directory = Path(temporary)
            (directory / "base").write_bytes(self.archive())
            (directory / "eboot").write_bytes(b"not the pinned game")
            receipt = directory / "out.vjhp.json"
            receipt.write_text("existing receipt")
            # Preflight must happen before parsing inputs or creating outputs.
            result = subprocess.run([sys.executable, str(ROOT / "tools/deltarune_hook.py"),
                "--base", str(directory / "base"), "--eboot", str(directory / "eboot"),
                "--output", str(directory / "out.vjhp")], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((directory / "out.vjhp").exists())
            self.assertEqual(receipt.read_text(), "existing receipt")


if __name__ == "__main__":
    unittest.main()
