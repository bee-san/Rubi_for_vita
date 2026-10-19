"""tools/check_offline_imports.py on minimal ELFs: Rubi has no network code (AGENTS.md rule 11).

A module fails with a network library's import stubs, a network code's symbol or a network module's
name in its allocated sections; one with no import stub, no symbol table or no entry point cannot be
checked; the Rubi app (--app) may define exactly settings/no_net.c's three newlib functions. The
binary mutants of #221's audit (audit-anki-sync-net: a renamed stub section, a stripped module, the
.velf, a run-time resolution by NID, a stub that shadows no_net.c) are rebuilt here as ELFs; and every
module vita_create_self makes in vita/ and compat/ is in the Vita build's check (F5)."""
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import check_offline_imports as c  # noqa: E402

TOOL = str(ROOT / "tools/check_offline_imports.py")
SHF_ALLOC, SHF_EXEC = 2, 4


class Sym:
    """A symbol: its name, the section it is defined in (a name, or None: undefined) and its type."""

    def __init__(self, name, section=".text", kind=2):
        self.name, self.section, self.kind = name, section, kind


def elf(names, symbols=(), contents=None, entry="module_start"):
    """An ELF32 (ARM, little-endian) with PROGBITS sections of these names (allocated; contents maps
    a name to its bytes, and a name starting with "!" is a section that is not allocated), a .symtab of
    these symbols (strings: functions in .text; Sym; None: no symbol table at all) and its .strtab, and
    a .shstrtab. The entry point (module_start, or the app's main) is one of the symbols unless None."""
    contents = contents or {}
    names = list(names)
    if ".text" not in names:
        names = [".text"] + names
    if symbols is not None:
        symbols = [s if isinstance(s, Sym) else Sym(s) for s in symbols]
        if entry:
            symbols = [Sym(entry)] + symbols
    blobs = []  # (name, type, flags, data, link, info, entsize)
    for n in names:
        flags = 0 if n.startswith("!") else SHF_ALLOC | (SHF_EXEC if n == ".text" else 0)
        blobs.append((n.lstrip("!"), 1, flags, contents.get(n, b""), 0, 0, 0))
    index = {b[0]: i + 1 for i, b in enumerate(blobs)}  # (0: the null section)
    if symbols is not None:
        sym_names = b"\0"
        entries = bytes(16)  # the null symbol
        for s in symbols:
            shndx = index[s.section] if s.section else 0
            entries += struct.pack("<IIIBBH", len(sym_names), 0, 0, 0x10 | s.kind, 0, shndx)  # global
            sym_names += s.name.encode() + b"\0"
        strtab_index = len(blobs) + 2  # after the null section and .symtab
        # sh_info: one past the last local symbol, the null one (audit N7)
        blobs.append((".symtab", 2, 0, entries, strtab_index, 1, 16))
        blobs.append((".strtab", 3, 0, sym_names, 0, 0, 0))
    shstr = b"\0"
    name_offsets = []
    for b in blobs + [(".shstrtab",)]:
        name_offsets.append(len(shstr))
        shstr += b[0].encode() + b"\0"
    body, placed, at = b"", [], 52
    for b in blobs:
        placed.append((at, len(b[3])))
        body += b[3]
        at += len(b[3])
    shstr_at = at
    body += shstr
    shoff = shstr_at + len(shstr)
    shnum = len(blobs) + 2
    head = b"\x7fELF" + bytes([1, 1, 1]) + bytes(9)
    head += struct.pack("<HHIIIIIHHHHHH", 2, 40, 1, 0, 0, shoff, 0, 52, 0, 0, 40, shnum, shnum - 1)
    table = bytes(40)
    for (n, kind, flags, data, link, info, entsize), off, (pos, size) in zip(blobs, name_offsets, placed):
        table += struct.pack("<IIIIIIIIII", off, kind, flags, 0, pos, size, link, info, 4, entsize)
    table += struct.pack("<IIIIIIIIII", name_offsets[-1], 3, 0, 0, shstr_at, len(shstr), 0, 0, 1, 0)
    return head + body + table


def run(*args):
    return subprocess.run([sys.executable, TOOL, *args], capture_output=True, text=True, timeout=30)


class Files:
    def __init__(self, *contents):
        self.contents = contents

    def __enter__(self):
        self.dir = tempfile.TemporaryDirectory()
        self.paths = []
        for i, data in enumerate(self.contents):
            p = Path(self.dir.name) / f"module{i}"
            p.write_bytes(data)
            self.paths.append(str(p))
        return self.paths

    def __exit__(self, *exc):
        self.dir.cleanup()


def check(data, *args):
    with Files(data) as (p,):
        return run(*args, p)


STUBS = [".vitalink.fstubs.SceLibKernel", ".vitalink.fstubs.SceIofilemgr", ".vitalink.vstubs.ScePafStdc", ".rodata"]
HASHES = ["br_sha256_init", "br_sha256_update", "br_sha1_out", "br_range_dec32be", "vjo_queue_id",
          "vjo_game_ocr_exchange", "net_main", "net_thread", "sync_locked", "delete_sync_objects"]
# The app's: newlib's three socket calls, defined in settings/no_net.c
NO_NET = ["sceNetRecv", "sceNetSend", "sceNetSocketClose"]


class Plugins(unittest.TestCase):
    def test_the_offline_modules_pass(self):
        r = check(elf(STUBS, HASHES, {".rodata": b"VjoNet stack: at least 4096 of 16384 B used\0"}))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("3 import libraries, no network library; 11 symbols, no network code; no network module named",
                      r.stdout)

    def test_a_network_library_fails(self):
        for lib in ("SceNet", "SceNetCtl", "SceNetPs", "SceNetPsForDriver", "SceNetAdhoc", "SceHttp", "SceSsl",
                    "ScePspnet", "ScePspnetAdhoc", "SceNpWebApi", "SceWlan"):
            for kind in ("fstubs", "vstubs"):
                with self.subTest(lib=lib, kind=kind):
                    r = check(elf(STUBS + [f".vitalink.{kind}.{lib}"], HASHES))
                    self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
                    self.assertIn("imports " + lib, r.stdout)
                    self.assertIn("Rubi has no network code", r.stdout)

    def test_network_code_fails(self):
        for sym in ("br_ssl_client_init_full", "br_sslio_write_all", "br_sslrec_in_cbc_init", "br_tls12_sha256_prf",
                    "br_x509_minimal_init", "mbedtls_ssl_setup", "wolfSSL_connect", "SSL_CTX_new", "X509_free",
                    "gnutls_init", "ptls_new", "s2n_connect", "curl_easy_perform", "sceNetSocket", "sceHttpInit",
                    "sceSslInit", "ksceNetSocket", "ksceNetRecvfrom", "ksceHttpInit", "ksceSslInit", "vjo_tls_open",
                    "vjo_http_send", "vjo_net_pool_init", "vjo_sync_run", "sqlite3_open_v2", "ZSTD_decompressStream",
                    "_ZN6rustls6client4tls1x", "_RNvCs1_7reqwest3get", "_ZN5tokio7runtime"):
            with self.subTest(sym=sym):
                r = check(elf(STUBS, HASHES + [sym]))
                self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
                self.assertIn("has network code (" + sym, r.stdout)

    def test_an_undefined_network_reference_fails_too(self):
        r = check(elf(STUBS, HASHES + [Sym("sceNetSocket", None)]))
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)

    def test_many_network_symbols_are_summarised(self):
        r = check(elf(STUBS, [f"br_ssl_engine_{i}" for i in range(11)]))
        self.assertEqual(r.returncode, 1)
        self.assertIn("and 3 more", r.stdout)

    def test_a_network_module_named_in_the_data_fails(self):
        # audit F1: a kernel plugin resolving SceNetPs's export by NID (module_get_export_func(KERNEL_PID,
        # "SceNetPs", ...)), or a module loading a network module's file, passed the import and symbol rules
        for text in (b"SceNetPs", b"SceNet", b"SceNetCtl", b"SceHttp", b"SceSsl", b"ScePspnet", b"SceNpWebApi",
                     b"SceWlan", b"vs0:sys/external/libnet.suprx", b"vs0:sys/external/libnetctl.suprx",
                     b"vs0:sys/external/libhttp.suprx", b"vs0:sys/external/libssl.suprx"):
            for section in (".rodata", ".data", ".text"):
                with self.subTest(text=text, section=section):
                    r = check(elf(STUBS + [".data"], HASHES, {section: b"\x01\x02" + text + b"\0xyz"}))
                    self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
                    self.assertIn("names network modules (", r.stdout)

    def test_names_outside_the_loaded_sections_do_not_count(self):
        # a section that is not loaded (.comment, debug info), or the stubs' own (their contents are code)
        for section, data in (("!.comment", b"SceNetPs libnet"), (".vitalink.fstubs.SceLibKernel", b"SceNetPs")):
            with self.subTest(section=section):
                r = check(elf(STUBS + [section], HASHES, {section: data}))
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_a_module_that_cannot_be_checked_is_an_error(self):
        cases = {
            "stripped": elf(STUBS, None),                                  # audit: the stripped Shell
            "no import stub (the .velf)": elf([".text", ".rodata"], HASHES),  # audit F2: "0 import libraries"
            "no module_start": elf(STUBS, HASHES, entry=None),             # audit N2: an empty .symtab passed
            "empty symbol table": elf(STUBS, [], entry=None),
        }
        for what, data in cases.items():
            with self.subTest(what):
                r = check(data)
                self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
                self.assertNotIn("Traceback", r.stderr)

    def test_a_renamed_stub_section_is_still_caught_by_its_symbol(self):
        # the audit's binary mutant: the SceNet stub section renamed away, its function's symbol kept
        r = check(elf(STUBS + [".vitalink.fstubs.SceLibc"], HASHES + [Sym("sceNetSocket", ".vitalink.fstubs.SceLibc")]))
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        # (both the section and the symbol renamed is deliberate obfuscation: a name check cannot stop it)

    def test_names_are_read_from_the_section_and_symbol_tables(self):
        self.assertEqual(c.imported_libraries(elf([".vitalink.fstubs.SceSysmem", ".text"])), ["SceSysmem"])
        self.assertEqual(c.section_names(elf([".a"], None)), ["", ".text", ".a", ".shstrtab"])
        self.assertEqual(c.symbol_names(elf([".a"], ["b", "a"])), ["a", "b", "module_start"])
        self.assertIsNone(c.symbol_names(elf([".a"], None)))
        self.assertEqual(c.network_module_names(elf([".rodata"], [], {".rodata": b"SceHttp\0libssl"})),
                         ["SceHttp", "libssl"])

    def test_every_module_is_checked_and_the_worst_counts(self):
        # the Vita build checks five modules in one call: a later clean one never resets the verdict, and
        # one that cannot be read stops nothing (audit N3)
        bad, good = elf(STUBS + [".vitalink.fstubs.SceNet"], HASHES), elf(STUBS, HASHES)
        code = elf(STUBS, HASHES + ["sqlite3_open"])
        for order in ((bad, good), (good, bad), (code, good), (good, code)):
            with Files(*order) as paths:
                self.assertEqual(run(*paths).returncode, 1)
        with Files(good, good) as paths:
            self.assertEqual(run(*paths).returncode, 0)
        with Files(b"not an elf", bad, good) as paths:
            r = run(*paths)
            self.assertEqual(r.returncode, 2)
            self.assertIn("imports SceNet", r.stdout)  # the second was still checked
            self.assertEqual(r.stdout.count("offline imports:"), 2)
            self.assertEqual(r.stderr.count("offline imports:"), 1)


class App(unittest.TestCase):
    def app(self, symbols, libs=(".vitalink.fstubs.SceLibKernel", ".vitalink.fstubs.SceAppMgr"), contents=None):
        return elf(list(libs) + [n for n in contents or {} if n not in libs], symbols, contents, entry="main")

    def test_the_app_with_no_net_c_passes(self):
        r = check(self.app(["vjo_afv_send", "vjo_apkg_write"] + NO_NET), "--app")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("2 import libraries, no network library; 6 symbols, no network code, newlib's 3 defined in "
                      "settings/no_net.c", r.stdout)
        r = check(self.app(["vjo_afv_send"]), "--app")  # (without them too)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_the_app_is_checked_as_a_plugin_is(self):
        for symbols, libs, contents, want in (
            (NO_NET + ["sceNetSocket"], (), None, "has network code (sceNetSocket"),
            (NO_NET + ["sceNetCtlInit"], (), None, "has network code (sceNetCtlInit"),
            (NO_NET + ["sqlite3_open"], (), None, "has network code (sqlite3_open"),
            (NO_NET + ["br_ssl_client_init_full"], (), None, "has network code (br_ssl_client_init_full"),
            (NO_NET, (".vitalink.fstubs.SceNet",), None, "imports SceNet"),
            (NO_NET, (".vitalink.fstubs.SceNetCtl",), None, "imports SceNetCtl"),
            (NO_NET, (".vitalink.fstubs.SceHttp",), None, "imports SceHttp"),
            (NO_NET, (".vitalink.fstubs.SceSsl",), None, "imports SceSsl"),
            (NO_NET, (), {".rodata": b"vs0:sys/external/libhttp.suprx"}, "names network modules (libhttp"),
        ):
            with self.subTest(want):
                data = self.app(symbols, (".vitalink.fstubs.SceLibKernel",) + tuple(libs), contents)
                r = check(data, "--app")
                self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
                self.assertIn(want, r.stdout)

    def test_no_net_c_s_names_only_as_the_app_s_own_functions(self):
        # audit H1: a SceNet stub that the link resolves next to no_net.c's definitions; a stub's
        # function is defined in its .vitalink section
        stub = ".vitalink.fstubs.SceLibc"
        for bad, why in ((Sym("sceNetRecv", None), "undefined"), (Sym("sceNetSend", stub), "an import's stub"),
                         (Sym("sceNetSocketClose", ".text", 1), "an object, not a function")):
            with self.subTest(why):
                data = self.app([s for s in NO_NET if s != bad.name] + [bad], (".vitalink.fstubs.SceLibKernel", stub))
                r = check(data, "--app")
                self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
                self.assertIn(f"has {bad.name} other than as a function of its own code", r.stdout)

    def test_a_plugin_may_not_define_them(self):
        r = check(elf(STUBS, HASHES + NO_NET))
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("has network code (sceNetRecv, sceNetSend, sceNetSocketClose", r.stdout)

    def test_the_app_needs_main(self):
        r = check(elf([".vitalink.fstubs.SceLibKernel"], NO_NET, entry="module_start"), "--app")
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("no main", r.stderr)

    def test_plugins_and_the_app_in_one_call(self):
        good, bad_app = elf(STUBS, HASHES), self.app(NO_NET, (".vitalink.fstubs.SceHttp",))
        with Files(good, bad_app) as (plugin, app):
            self.assertEqual(run(plugin, "--app", app).returncode, 1)
            self.assertEqual(run("--app", app, plugin).returncode, 1)
        with Files(good, self.app(NO_NET)) as (plugin, app):
            self.assertEqual(run("--app", app, plugin).returncode, 0)


class Parsing(unittest.TestCase):
    def test_no_module_is_a_usage_error(self):
        self.assertEqual(run().returncode, 2)
        self.assertEqual(run("--app").returncode, 2)

    def test_another_elf_class_or_a_cut_header_is_an_error(self):
        good = elf(STUBS, HASHES)
        elf64 = good[:4] + bytes([2]) + good[5:]  # ELFCLASS64
        big = good[:5] + bytes([2]) + good[6:]    # big-endian
        for data in (elf64, big, good[:4], good[:5], good[:6], good[:40], good[:60], good[:-1], b"SCE\0" + bytes(60)):
            for args in ((), ("--app",)):
                with self.subTest(size=len(data), args=args):
                    r = check(data, *args)
                    self.assertEqual(r.returncode, 2, r.stderr)  # 1 is "network imports or code"
                    self.assertNotIn("Traceback", r.stderr)

    @staticmethod
    def headers(data):
        shoff, = struct.unpack_from("<I", data, 0x20)
        shnum, = struct.unpack_from("<H", data, 0x30)
        return shoff, shnum

    def patched(self, data, section, field, value):
        """data with section number section's header field (0-9: name, type, flags, addr, offset, size,
        link, info, align, entsize) set to value."""
        shoff, _ = self.headers(data)
        at = shoff + 40 * section + 4 * field
        return data[:at] + struct.pack("<I", value) + data[at + 4:]

    def test_bad_symbol_and_string_tables_are_errors(self):
        # audit N8: the .symtab and its string table's bounds, each broken alone
        data = elf(STUBS, ["sceNetSocket"] + HASHES)  # (a network module: a pass would show)
        names = c.section_names(data)
        symtab, strtab = names.index(".symtab"), names.index(".strtab")
        size = len(data)
        for what, section, field, value in (
            ("symtab offset beyond the file", symtab, 4, size + 16),
            ("symtab size beyond the file", symtab, 5, size),
            ("symtab size not whole symbols", symtab, 5, 16 * 3 + 5),
            ("symtab link out of range", symtab, 6, 99),
            ("symtab link not a string table", symtab, 6, 1),
            ("strtab beyond the file", strtab, 5, size),
            ("strtab offset beyond the file", strtab, 4, size + 1),
            ("a section name's offset out of range", 1, 0, 100000),
        ):
            with self.subTest(what):
                r = check(self.patched(data, section, field, value))
                self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
                self.assertNotIn("Traceback", r.stderr)

    def test_unterminated_names_are_errors(self):
        # audit N1: an unterminated last symbol name and section name were read as they were (exit 0)
        data = elf(STUBS, HASHES + ["sceNetSocket"])
        names = c.section_names(data)
        strtab = names.index(".strtab")
        shoff, shnum = self.headers(data)
        off, size = struct.unpack_from("<II", data, shoff + 40 * strtab + 16)
        cut = self.patched(data, strtab, 5, size - 1)  # the last name loses its NUL
        r = check(cut)
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("unterminated symbol name", r.stderr)
        shstrtab = shnum - 1
        off, size = struct.unpack_from("<II", data, shoff + 40 * shstrtab + 16)
        r = check(self.patched(data, shstrtab, 5, size - 1))
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)

    def test_the_test_elves_are_what_readelf_reads(self):
        # audit N7: sh_info is one past the last local symbol (the null one), as readelf expects
        data = elf(STUBS, HASHES)
        names = c.section_names(data)
        shoff, _ = self.headers(data)
        info, = struct.unpack_from("<I", data, shoff + 40 * names.index(".symtab") + 28)
        self.assertEqual(info, 1)
        symbols = c.symbols(data)
        self.assertEqual(sorted(n for n, _, _ in symbols), sorted(["module_start"] + HASHES))
        self.assertTrue(all(names[shndx] == ".text" and kind == 2 for _, shndx, kind in symbols))


class TheBuildChecksEveryModule(unittest.TestCase):
    """Audit F5: nothing kept the list complete (main had dropped the Adrenaline bridge)."""

    def test_every_vita_create_self_module_is_checked(self):
        made = set()
        for path in [ROOT / "vita/CMakeLists.txt"] + sorted((ROOT / "compat").glob("*/CMakeLists.txt")):
            made |= set(re.findall(r"vita_create_self\(\s*\S+\s+(\w+)", path.read_text()))
        made.discard("vjo_settings_smoke")  # the Vita3K smoke app: off by default, never released
        self.assertTrue({"VitaJPOverlay_Kernel", "VitaJPOverlay_Shell", "VitaJPOverlay_OCR", "VitaJPOverlay_Settings",
                         "adrenaline_vsh"} <= made, made)
        text = (ROOT / "vita/CMakeLists.txt").read_text()
        block = text.split("# ---------------- no network anywhere in Rubi ----------------", 1)[1]
        block = block.split("\n# ----------------", 1)[0]
        listed = re.search(r"set\(VJO_OFFLINE_MODULES([^)]*)\)", block).group(1)  # (no parenthesis inside)
        modules = set(re.findall(r"\$<TARGET_FILE:(\w+)>", listed))
        modules |= set(re.findall(r"list\(APPEND VJO_OFFLINE_MODULES \$<TARGET_FILE:(\w+)>\)", block))
        apps = set(re.findall(r"--app \$<TARGET_FILE:(\w+)>", block))
        self.assertEqual(apps, {"VitaJPOverlay_Settings"})
        self.assertEqual(made - modules - apps, set(), "built but not checked for network code")
        self.assertIn("check_offline_imports.py ${VJO_OFFLINE_MODULES}", block)
        # and the release needs the check
        self.assertRegex(text, r"add_custom_target\(release[\s\S]*?DEPENDS[\s\S]*?vjo_offline_imports")


if __name__ == "__main__":
    unittest.main()
