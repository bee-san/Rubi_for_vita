"""tools/vita_self.py, the SELF reader the Vita build's checks share (check_memory_ledger, check_settings_self,
check_vita_syscall_abi): it reads what vita-make-fself writes, and refuses every malformed file each of the readers
it replaced refused (audit-rubi-cleanup-8: a cut zlib stream and a stored segment longer than its p_filesz)."""
import struct
import sys
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools/tests"))
import vita_self  # noqa: E402
from self_fixture import PHDRS_AT, SEGMENTS_AT, PT_SCE_RELA, Segment, fself  # noqa: E402

CODE, DATA = b"\x01code\x00" * 300, b"\x02data" * 50
SEGINFO_AT = PHDRS_AT + 32 * 3  # the three segments' infos follow their program headers


def good(compress=True):
    return fself(Segment(CODE, vaddr=0x81000000), Segment(DATA, memsz=0x2000, flags=6, vaddr=0x81010000),
                 Segment(b"\0" * 24, memsz=0, kind=PT_SCE_RELA), entry=0x40, compress=compress,
                 boot=(1, 0x0E, 0, 0xC800))


def patched(data, offset, fmt, *values):
    b = bytearray(data)
    struct.pack_into(fmt, b, offset, *values)
    return bytes(b)


class Reads(unittest.TestCase):
    def test_a_stored_and_a_compressed_self(self):
        for compress in (False, True):
            with self.subTest(compress=compress):
                sce = vita_self.read(good(compress))
                self.assertEqual((sce.elf_type, sce.machine, sce.entry), (0xFE04, 40, 0x40))
                self.assertEqual([(s.kind, s.vaddr, s.filesz, s.memsz, s.flags) for s in sce.segments],
                                 [(1, 0x81000000, len(CODE), len(CODE), 5), (1, 0x81010000, len(DATA), 0x2000, 6),
                                  (PT_SCE_RELA, 0, 24, 0, 5)])
                self.assertEqual([s.data for s in sce.segments], [CODE, DATA, b"\0" * 24])
                self.assertEqual(struct.unpack_from("<4I", sce.control[6]), (1, 0x0E, 0, 0xC800))

    def test_headers_only_reads_no_segment_and_no_control_info(self):
        sce = vita_self.read(good(), headers_only=True)
        self.assertEqual([s.data for s in sce.segments], [b"", b"", b""])
        self.assertEqual(sce.control, {})
        self.assertEqual([vita_self.page_round(s.memsz) for s in sce.segments], [4096, 8192, 0])


class Refuses(unittest.TestCase):
    def refused(self, data, message, headers_only=False):
        with self.assertRaises(vita_self.Invalid) as caught:
            vita_self.read(data, headers_only)
        self.assertIn(message, str(caught.exception))

    def test_every_malformed_kind(self):
        data = good()
        elf = 0xA0
        offset, length, _, _ = struct.unpack_from("<4Q", data, SEGINFO_AT)
        stream = zlib.decompressobj()
        stream.decompress(data[offset:offset + length])
        whole = length - len(stream.unused_data)  # the zlib stream without vita-make-fself's padding
        cut = patched(data, SEGINFO_AT + 8, "<Q", whole - 4)  # the stream without its adler32 trailer
        stored = good(compress=False)
        s_offset, s_length, _, _ = struct.unpack_from("<4Q", stored, SEGINFO_AT)
        control_at, = struct.unpack_from("<Q", data, 0x68)  # the first control info (its type, size, more, pad)
        cases = (
            (b"\x7fELF" + data[4:], "not a SELF (no SCE header)"),
            (data[:0x50], "SCE header: truncated at 0x58"),
            (patched(data, 0x68, "<Q", len(data) - 8), "control info: truncated"),
            (patched(data, 0x68 + 0x08, "<Q", 8), "control info at 0x"),
            (patched(data, elf, "<4s", b"\x7fELG"), "no little-endian ELF32 header in the SELF"),
            # the ELF class and byte order too, not only \x7fELF (audit-rubi-cleanup-8-b F2)
            (patched(data, elf + 4, "<B", 2), "no little-endian ELF32 header in the SELF"),  # ELF64
            (patched(data, elf + 5, "<B", 2), "no little-endian ELF32 header in the SELF"),  # big-endian
            # a control info shorter than its own 16-byte header, its end inside the block
            (patched(data, control_at + 4, "<I", 8), f"control info at 0x{control_at:x}: size 8"),
            (patched(data, elf + 44, "<H", 0), "bad program headers"),
            (patched(data, elf + 42, "<H", 40), "bad program headers"),
            (patched(data, 0x48, "<Q", len(data) - 8), "program header: truncated"),
            (patched(data, SEGINFO_AT + 24, "<Q", 1), "segment 0 is encrypted"),
            (patched(data, SEGINFO_AT + 24, "<Q", 0), "segment 0 is encrypted"),  # only 2 is fself's plain
            (patched(data, SEGINFO_AT + 24, "<Q", 3), "segment 0 is encrypted"),
            (patched(data, SEGINFO_AT + 16, "<Q", 3), "segment 0: compression 3"),
            (patched(data, SEGINFO_AT, "<Q", len(data)), "segment 0 runs past the SELF's end"),
            (patched(data, offset, "<I", 0xFFFFFFFF), "segment 0: Error -3"),
            (cut, "segment 0: its zlib stream is incomplete"),
            (patched(data, PHDRS_AT + 16, "<I", len(CODE) + 4), "segment 0: 1800 bytes, the program header says 1804"),
            (patched(stored, SEGINFO_AT + 8, "<Q", s_length + 16), "segment 0: 1816 bytes stored, the program header"),
            (patched(stored, SEGINFO_AT + 8, "<Q", s_length - 16), "segment 0: 1784 bytes stored, the program header"),
        )
        self.assertEqual(s_offset, SEGMENTS_AT)
        for case, (blob, message) in enumerate(cases):
            with self.subTest(case=case, message=message):
                self.refused(blob, message)

    def test_headers_only_still_refuses_bad_headers(self):
        self.refused(b"not a SELF at all", "not a SELF (no SCE header)", headers_only=True)
        self.refused(patched(good(), 0xA0 + 44, "<H", 17), "bad program headers", headers_only=True)


if __name__ == "__main__":
    unittest.main()
