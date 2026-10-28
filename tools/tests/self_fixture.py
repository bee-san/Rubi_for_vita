"""Synthetic Vita SELF files for the tools' tests, laid out as vita-make-fself writes them (vita-toolchain src/self.h,
src/vita-make-fself/vita-make-fself.c; the Vita build's own modules have this layout): the SCE header, the app info,
the ELF header at 0xA0, the program headers at 0xE0, a segment info for each, the version info, control infos 4, 5, 6
and 7, then the segments from 0x1000, each stored or zlib-compressed (a compressed one padded to 4 bytes after its
stream, as vita-make-fself pads it).

    fself(b"code", b"data")                                  # two PT_LOAD segments of these bytes, stored
    fself(Segment(code, vaddr=0x81000000), Segment(memsz=0x2000, flags=6), entry=0x400, compress=True)
"""
import struct
import zlib
from typing import NamedTuple, Optional

PT_LOAD = 1
PT_SCE_RELA = 0x60000000  # the SCE relocations' segment
ET_SCE_RELEXEC = 0xFE04
EM_ARM = 40
ELF_AT, PHDRS_AT, SEGMENTS_AT = 0xA0, 0xE0, 0x1000


class Segment(NamedTuple):
    """One program header and its bytes: data is p_filesz's bytes, memsz p_memsz (None: len(data); more than that is
    the segment's .bss)."""
    data: bytes = b""
    memsz: Optional[int] = None
    kind: int = PT_LOAD
    vaddr: int = 0
    flags: int = 5  # PF_R | PF_X


def fself(*segments, entry=0, compress=False, boot=(1, 0, 0, 0)):
    """The SELF of segments (each a Segment, or bytes: a PT_LOAD of them). entry: e_entry (the module info's segment
    index << 30 | its offset); compress: zlib each segment (vita-make-fself -c, as the Vita build does); boot: control
    info 6's (is_used, attribute, phycont KiB, total KiB), the boot parameters, or None for a SELF without it."""
    segments = [s if isinstance(s, Segment) else Segment(bytes(s)) for s in segments]
    phnum = len(segments)
    seginfo_at = PHDRS_AT + 32 * phnum
    version_at = seginfo_at + 32 * phnum
    control_at = version_at + 16
    control = struct.pack("<4I", 4, 0x50, 1, 0) + b"\0" * 0x40
    control += struct.pack("<4I", 5, 0x110, 1, 0) + b"\0" * 0x100
    if boot is not None:
        control += struct.pack("<4I", 6, 0x110, 1, 0) + struct.pack("<4I", *boot) + b"\0" * 0xF0
    control += struct.pack("<4I", 7, 0x50, 0, 0) + b"\0" * 0x40
    out = bytearray(SEGMENTS_AT)
    struct.pack_into("<4sIHHI", out, 0, b"SCE\0", 3, 0xC0, 1, 0x600)
    struct.pack_into("<13Q", out, 0x10, SEGMENTS_AT, 0, 0, 0, 4, 0x80, ELF_AT, PHDRS_AT, 0, seginfo_at, version_at,
                     control_at, len(control))
    out[ELF_AT:ELF_AT + 8] = b"\x7fELF\x01\x01\x01\0"
    struct.pack_into("<HHII", out, ELF_AT + 16, ET_SCE_RELEXEC, EM_ARM, 1, entry)
    struct.pack_into("<HH", out, ELF_AT + 42, 32, phnum)
    out[control_at:control_at + len(control)] = control
    for index, s in enumerate(segments):
        stored = s.data
        if compress:
            stored = zlib.compress(s.data, 9)
            stored += b"\0" * (-len(stored) % 4)  # vita-make-fself's padding, after the zlib stream
        struct.pack_into("<8I", out, PHDRS_AT + 32 * index, s.kind, 0, s.vaddr, 0, len(s.data),
                         len(s.data) if s.memsz is None else s.memsz, s.flags, 16)
        struct.pack_into("<4Q", out, seginfo_at + 32 * index, len(out), len(stored), 2 if compress else 1, 2)
        out += stored
    return bytes(out)
