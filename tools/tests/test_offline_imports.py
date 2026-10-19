"""tools/check_offline_imports.py on minimal ELFs: a plugin with a network library's stubs or
network code's symbols fails, and so does the Rubi app with a network library its sync does not use."""
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


def elf(names, symbols=()):
    """An ELF32 (ARM, little-endian) with empty PROGBITS sections of these names, a .symtab of these
    symbols (None: no symbol table at all) and its .strtab, and a .shstrtab."""
    sym_names = b"\0"
    entries = bytes(16)  # the null symbol
    for s in symbols or ():
        entries += struct.pack("<IIIBBH", len(sym_names), 0, 0, 0x12, 0, 1)  # a global function
        sym_names += s.encode() + b"\0"
    blobs = []  # (name, type, data, link)
    for n in names:
        blobs.append((n, 1, b"", 0))
    if symbols is not None:
        strtab_index = len(blobs) + 2  # after the null section and .symtab
        blobs.append((".symtab", 2, entries, strtab_index))
        blobs.append((".strtab", 3, sym_names, 0))
    shstr = b"\0"
    name_offsets = []
    for n, _, _, _ in blobs + [(".shstrtab", 3, b"", 0)]:
        name_offsets.append(len(shstr))
        shstr += n.encode() + b"\0"
    body = b""
    placed = []
    at = 52
    for _, _, data, _ in blobs:
        placed.append((at, len(data)))
        body += data
        at += len(data)
    shstr_at = at
    body += shstr
    shoff = shstr_at + len(shstr)
    shnum = len(blobs) + 2
    head = b"\x7fELF" + bytes([1, 1, 1]) + bytes(9)
    head += struct.pack("<HHIIIIIHHHHHH", 2, 40, 1, 0, 0, shoff, 0, 52, 0, 0, 40, shnum, shnum - 1)
    table = bytes(40)
    for (n, kind, data, link), off, (pos, size) in zip(blobs, name_offsets, placed):
        entsize = 16 if kind == 2 else 0
        table += struct.pack("<IIIIIIIIII", off, kind, 0 if kind != 1 else 6, 0, pos, size, link, 0, 4, entsize)
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


KERNEL_LIKE = [".text", ".vitalink.fstubs.SceLibKernel", ".vitalink.fstubs.SceIofilemgr",
               ".vitalink.vstubs.ScePafStdc", ".rodata"]
HASHES = ["module_start", "br_sha256_init", "br_sha256_update", "br_sha1_out", "br_range_dec32be",
          "vjo_queue_id", "vjo_game_ocr_exchange"]


class Plugins(unittest.TestCase):
    def test_the_offline_modules_pass(self):
        with Files(elf(KERNEL_LIKE, HASHES)) as (p,):
            r = run(p)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("3 import libraries, no network library; 7 symbols, no network code", r.stdout)

    def test_a_network_library_fails(self):
        for lib in ("SceNet", "SceNetCtl", "SceNetPs", "SceHttp", "SceSsl"):
            with self.subTest(lib=lib), Files(elf([".vitalink.fstubs.SceLibKernel", ".vitalink.fstubs." + lib])) as (p,):
                r = run(p)
                self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
                self.assertIn("imports " + lib, r.stdout)
                self.assertIn("the plugins are offline", r.stdout)
        with Files(elf([".vitalink.vstubs.SceNet"])) as (p,):  # a variable import counts too
            self.assertEqual(run(p).returncode, 1)

    def test_network_code_fails(self):
        for sym in ("br_ssl_client_init_full", "br_sslio_write_all", "br_x509_minimal_init", "mbedtls_ssl_setup",
                    "wolfSSL_connect", "SSL_CTX_new", "curl_easy_perform", "sceNetSocket", "sceHttpInit",
                    "sceSslInit", "vjo_tls_open", "vjo_http_send", "vjo_net_pool_init", "vjo_sync_run",
                    "sqlite3_open_v2", "ZSTD_decompressStream", "_ZN6rustls6client4tls1x", "_RNvCs1_7reqwest3get",
                    "_ZN5tokio7runtime"):
            with self.subTest(sym=sym), Files(elf(KERNEL_LIKE, HASHES + [sym])) as (p,):
                r = run(p)
                self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
                self.assertIn("has network code (" + sym, r.stdout)

    def test_many_network_symbols_are_summarised(self):
        syms = [f"br_ssl_engine_{i}" for i in range(11)]
        with Files(elf(KERNEL_LIKE, syms)) as (p,):
            r = run(p)
        self.assertEqual(r.returncode, 1)
        self.assertIn("and 3 more", r.stdout)

    def test_a_stripped_plugin_cannot_be_checked(self):
        with Files(elf(KERNEL_LIKE, None)) as (p,):
            r = run(p)
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("no symbol table", r.stderr)

    def test_names_are_read_from_the_section_and_symbol_tables(self):
        self.assertEqual(c.imported_libraries(elf([".vitalink.fstubs.SceSysmem", ".text"])), ["SceSysmem"])
        self.assertEqual(c.section_names(elf([".a"], None)), ["", ".a", ".shstrtab"])
        self.assertEqual(c.symbol_names(elf([".a"], ["b", "a"])), ["a", "b"])
        self.assertIsNone(c.symbol_names(elf([".a"], None)))

    def test_a_clean_module_after_a_bad_one_still_fails(self):
        # the Vita build checks the three plugins in one call: a later clean one must not reset the verdict
        bad, good = elf([".vitalink.fstubs.SceNet"]), elf([".vitalink.fstubs.SceLibKernel"])
        code = elf([".vitalink.fstubs.SceLibKernel"], ["sqlite3_open"])
        for order in ((bad, good), (good, bad), (code, good), (good, code)):
            with Files(*order) as paths:
                self.assertEqual(run(*paths).returncode, 1)
        with Files(good, good) as paths:
            self.assertEqual(run(*paths).returncode, 0)


class App(unittest.TestCase):
    def test_the_app_without_network_passes(self):
        with Files(elf([".vitalink.fstubs.SceLibKernel", ".vitalink.fstubs.SceAppMgr"],
                       ["main", "sqlite3_open", "sceNetRecv"])) as (p,):
            r = run("--app", p)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)  # its symbols are not checked
        self.assertIn("2 import libraries, no network library", r.stdout)

    def test_the_app_may_import_only_its_syncs_libraries(self):
        for lib in ("SceNet", "SceNetCtl", "SceNetPs", "SceHttp", "SceSsl"):
            allowed = lib in c.APP_NETWORK
            with self.subTest(lib=lib), Files(elf([".vitalink.fstubs." + lib])) as (p,):
                r = run("--app", p)
                self.assertEqual(r.returncode, 0 if allowed else 1, r.stdout + r.stderr)
        self.assertFalse(any(lib.startswith(("SceHttp", "SceSsl")) for lib in c.APP_NETWORK))

    def test_plugins_and_the_app_in_one_call(self):
        good, bad_app = elf([".vitalink.fstubs.SceLibKernel"]), elf([".vitalink.fstubs.SceHttp"])
        with Files(good, bad_app) as (plugin, app):
            self.assertEqual(run(plugin, "--app", app).returncode, 1)
            self.assertEqual(run("--app", plugin, plugin).returncode, 0)


class Usage(unittest.TestCase):
    def test_no_module_is_a_usage_error(self):
        self.assertEqual(run().returncode, 2)
        self.assertEqual(run("--app").returncode, 2)

    def test_another_elf_class_or_a_cut_header_is_an_error(self):
        good = elf([".vitalink.fstubs.SceLibKernel"])
        elf64 = good[:4] + bytes([2]) + good[5:]  # ELFCLASS64
        big = good[:5] + bytes([2]) + good[6:]    # big-endian
        for data in (elf64, big, good[:4], good[:5], good[:6], good[:40], good[:60]):
            for args in ((), ("--app",)):
                with self.subTest(size=len(data), args=args), Files(data) as (p,):
                    r = run(*args, p)
                    self.assertEqual(r.returncode, 2, r.stderr)  # 1 is "a network library or code"
                    self.assertNotIn("Traceback", r.stderr)

    def test_a_file_that_is_no_elf_is_an_error(self):
        with Files(b"SCE\0" + bytes(60)) as (p,):
            self.assertEqual(run(p).returncode, 2)


if __name__ == "__main__":
    unittest.main()
