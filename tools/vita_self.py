"""Vita SELF files as vita-make-fself writes them (fself: unencrypted, each segment stored or zlib-compressed), read
for the tools that check the Vita build's modules: the SCE header, the control infos, the ELF header, the program
headers and each segment's bytes (vita-toolchain src/self.h, src/vita-make-fself/vita-make-fself.c). No dependencies.

    sce = vita_self.read(Path("VitaJPOverlay_Shell.suprx").read_bytes())
    sce.segments[0].data      # the first segment's p_filesz bytes, decompressed
    sce.control[6]            # control info 6, the boot parameters (after its 16-byte header)

A file that is not such a SELF, or whose structures run past its end, raises Invalid with what is wrong.
"""
import struct
import zlib
from typing import NamedTuple

MAGIC = b"SCE\0"
PT_LOAD = 1
PT_SCE_RELA = 0x60000000  # the SCE relocations' segment
PAGE = 4096


class Invalid(ValueError):
    """Not a SELF this reader reads, or a structure of one outside the file."""


class Segment(NamedTuple):
    kind: int    # p_type: PT_LOAD or PT_SCE_RELA
    vaddr: int
    filesz: int
    memsz: int   # more than filesz: its .bss
    flags: int   # p_flags: 1 X, 2 W, 4 R
    data: bytes  # its p_filesz bytes, decompressed (b"" when read with headers_only)


class Self(NamedTuple):
    elf_type: int    # e_type: 0xFE04, a relocatable module (ET_SCE_RELEXEC)
    machine: int     # e_machine: 40, ARM
    entry: int       # e_entry: the module info's segment index << 30 | its offset there
    segments: tuple  # a Segment per program header, in their order
    control: dict    # control info type -> its bytes after the 16-byte header ({} with headers_only)


def unpack(fmt, data, offset, what):
    """struct.unpack_from, or Invalid when the structure is not inside data."""
    if not 0 <= offset <= len(data) - struct.calcsize(fmt):
        raise Invalid(f"{what}: truncated at 0x{offset:x}")
    return struct.unpack_from(fmt, data, offset)


def page_round(size):
    return (size + PAGE - 1) // PAGE * PAGE


def read(data, headers_only=False):
    """The SELF in data: its SCE header, control infos, ELF header, program headers and each segment's bytes, each
    segment unencrypted, inside the file, and either stored in exactly p_filesz bytes or a whole zlib stream of
    p_filesz bytes (vita-make-fself pads after the stream). headers_only: the SCE, ELF and program headers alone (no
    control info, no segment's bytes)."""
    if data[:4] != MAGIC:
        raise Invalid("not a SELF (no SCE header)")
    elf_at, phdrs_at = unpack("<QQ", data, 0x40, "SCE header")
    seginfo_at, = unpack("<Q", data, 0x58, "SCE header")
    control = {}
    if not headers_only:
        control_at, control_size = unpack("<QQ", data, 0x68, "SCE header")
        offset = control_at
        while offset < control_at + control_size:
            kind, size, more, _ = unpack("<4I", data, offset, "control info")
            if not (16 <= size and offset + size <= control_at + control_size):
                raise Invalid(f"control info at 0x{offset:x}: size {size}")
            control[kind] = data[offset + 16:offset + size]
            offset += size
            if not more:
                break
    if data[elf_at:elf_at + 6] != b"\x7fELF\x01\x01":
        raise Invalid("no little-endian ELF32 header in the SELF")
    elf_type, machine = unpack("<HH", data, elf_at + 16, "ELF header")
    entry, = unpack("<I", data, elf_at + 24, "ELF header")
    phentsize, phnum = unpack("<HH", data, elf_at + 42, "ELF header")
    if phentsize != 32 or not 1 <= phnum <= 16:
        raise Invalid("bad program headers")
    segments = []
    for index in range(phnum):
        kind, _, vaddr, _, filesz, memsz, flags, _ = unpack("<8I", data, phdrs_at + 32 * index, "program header")
        blob = b""
        if not headers_only:
            offset, length, compression, encryption = unpack("<4Q", data, seginfo_at + 32 * index, "segment info")
            if encryption != 2:
                raise Invalid(f"segment {index} is encrypted")
            if compression not in (1, 2):
                raise Invalid(f"segment {index}: compression {compression}")
            if offset + length > len(data):
                raise Invalid(f"segment {index} runs past the SELF's end")
            blob = data[offset:offset + length]
            if compression == 2:
                stream = zlib.decompressobj()
                try:
                    blob = stream.decompress(blob)
                except zlib.error as error:
                    raise Invalid(f"segment {index}: {error}") from None
                if not stream.eof:  # cut short, its adler32 trailer too (vita-make-fself pads after a whole stream)
                    raise Invalid(f"segment {index}: its zlib stream is incomplete")
            elif length != filesz:
                raise Invalid(f"segment {index}: {length} bytes stored, the program header says {filesz}")
            if len(blob) != filesz:
                raise Invalid(f"segment {index}: {len(blob)} bytes, the program header says {filesz}")
        segments.append(Segment(kind, vaddr, filesz, memsz, flags, blob))
    return Self(elf_type, machine, entry, tuple(segments), control)
