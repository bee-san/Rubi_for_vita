#!/usr/bin/env python3
"""Check that the Rubi app's eboot.bin is the system application vita/CMakeLists.txt builds.

Since #109 the app runs beside a suspended game as a system application, and nothing else in
the build notices when a change undoes part of it: each of audit-pr109's mutants M1-M7 (no
--undefined, no ATTRIBUTE or MEMSIZE, no settings/sysapp.c, a 64 MiB heap, an application
priority, a worker off CPU3) built and packaged. This reads, without dependencies:

- the SELF's control info 6 (vita-toolchain src/self.h): attribute 0x0E and a 0xC800 KiB (50 MiB)
  budget with no physically contiguous part, as vita-make-fself -at 0x0E -m 0xC800 writes them;
- its process parameters, which vita-elf-create puts right after the module info that e_entry
  points to (src/vita-elf-create/sce-elf.c): the main thread's priority and CPU mask, by pointer
  to settings/sysapp.c's sceUserMainThreadPriority and sceUserMainThreadCpuAffinityMask, read in
  the SELF's own (decompressed) segments;
- the linked ELF: newlib's heap (_newlib_heap_size_user, else newlib's 128 MiB), which with the
  SELF's segments and libvita2d's fixed allocations (and the Cards page's picture texture) must fit
  the budget; and every call of
  sceKernelCreateThread made from settings/sysapp.c's wrapper (the link's --wrap), which puts
  each thread on CPU3, and none of sceKernelChangeThreadCpuAffinityMask;
- libvita2d's calls of sceGxmVshInitialize and sceSharedFbGetInfo going to settings/sysapp_gfx.c's
  wrappers (the link's --wrap), which make its system start wait for SceShell's hand-over with a
  pause and fall back to the app's own parameter buffer: without them the app does not open with
  no game running (owner, 2026-10-16), and the build gave no sign;
- a font's glyph atlas made through settings/vita2d_textures.c's wrapper of
  vita2d_create_empty_texture_format (libvita2d's texture_atlas_create calls it, never the function itself):
  its two stale fields cleared, so that freeing a font while the app runs (settings/main.c font_tick) frees
  nothing else (libvita2d #28).

The memory sum is a lower bound from the build and docs/vita-memory.md section 5.4's estimates,
not a measurement: the console's own numbers are the app's start line in log.txt.
"""
import argparse
import json
from pathlib import Path
import struct
import sys
import zlib

ATTRIBUTE = 0x0E              # a system application (vita/CMakeLists.txt ATTRIBUTE)
BUDGET_KIB = 0xC800           # 50 MiB (MEMSIZE)
PRIORITY = 0x10000100         # SCE_KERNEL_DEFAULT_PRIORITY (settings/sysapp.c)
CPU_MASK_SYSTEM = 0x00080000  # SCE_KERNEL_CPU_MASK_SYSTEM: CPU3
HEAP_SYMBOL = "_newlib_heap_size_user"
THREAD_IMPORT = "sceKernelCreateThread"
THREAD_WRAPPER = "__wrap_sceKernelCreateThread"
FORBIDDEN_CALLS = ("sceKernelChangeThreadCpuAffinityMask",)
# libvita2d's two calls into the system that settings/sysapp_gfx.c wraps (the link's --wrap), so
# that its system start waits for SceShell's hand-over and falls back to the app's own parameter
# buffer: with no game running the app does not open otherwise (owner, 2026-10-16).
GRAPHICS_IMPORTS = ("sceGxmVshInitialize", "sceSharedFbGetInfo")
MODULE_INFO_SIZE = 0x5C       # sce_module_info_raw (src/sce-elf-defs.h)
# What the app holds besides its heap and its segments, by source (docs/vita-memory.md section
# 5.4; estimates, not measured on a console): libvita2d a8f15ab's GXM rings, depth and stencil,
# shader patcher, index and clear buffers, parameter buffer and pool; at most 5 icon textures of
# 256 KiB (settings/icons.h); two 512 x 512 glyph atlases (a font whose atlas is full and the one that replaced it,
# until the next frame: settings/main.c font_tick, #313); the main thread's default 256 KiB stack and the
# Anki, Install and Stats workers' 64 KiB each (settings/anki_page.c, install_page.c, stats_page.c #312; the Stats tab's
# tables are in the segments: its counts, its cache's work memory and its rows, about 2 MiB; so are the Look up tab's,
# #313); the Cards page's picture (#315), one texture of at most 512 x 288 RGB (settings/card_picture.h
# VJO_CARD_PICTURE_W/_H, three bytes a pixel; its list and card views are in the segments, libjpeg's work in the heap).
FIXED = {"libvita2d": 10_424_320, "icons": 5 * 256 * 1024, "glyph_atlas": 2 * 512 * 512,
         "stacks": 256 * 1024 + 3 * 64 * 1024, "card_picture": 512 * 288 * 3}
# libvita2d's glyph atlas texture and the wrapper it is made through (settings/vita2d_textures.c).
ATLAS_FUNCTION = "texture_atlas_create"
TEXTURE_FUNCTION = "vita2d_create_empty_texture_format"


class Invalid(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise Invalid(message)


def unpack(fmt, data, offset, what):
    require(0 <= offset and offset + struct.calcsize(fmt) <= len(data), f"{what}: truncated at 0x{offset:x}")
    return struct.unpack_from(fmt, data, offset)


def read_self(data):
    """The SELF's control info 6, its process parameters' two thread values, its segments' sizes."""
    require(data[:4] == b"SCE\0", "not a SELF (no SCE header)")
    elf_offset, phdr_offset = unpack("<QQ", data, 0x40, "SCE header")
    seginfo_offset, = unpack("<Q", data, 0x58, "SCE header")
    control_offset, control_size = unpack("<QQ", data, 0x68, "SCE header")
    control, offset = {}, control_offset
    while offset < control_offset + control_size:
        kind, size, more, _ = unpack("<4I", data, offset, "control info")
        require(16 <= size and offset + size <= control_offset + control_size,
                f"control info at 0x{offset:x}: size {size}")
        control[kind] = data[offset + 16:offset + size]
        offset += size
        if not more:
            break
    require(6 in control and len(control[6]) >= 16, "no control info 6 (the boot parameters)")
    is_used, attribute, phycont_kib, total_kib = struct.unpack_from("<4I", control[6], 0)

    require(data[elf_offset:elf_offset + 6] == b"\x7fELF\x01\x01", "no little-endian ELF32 header in the SELF")
    entry, = unpack("<I", data, elf_offset + 24, "ELF header")
    phentsize, phnum = unpack("<HH", data, elf_offset + 42, "ELF header")
    require(phentsize == 32 and 1 <= phnum <= 16, "bad program headers")
    segments = []
    for index in range(phnum):
        kind, _, vaddr, _, filesz, memsz, _, _ = unpack("<8I", data, phdr_offset + 32 * index, "program header")
        offset, length, compression, encryption = unpack("<4Q", data, seginfo_offset + 32 * index, "segment info")
        require(encryption == 2, f"segment {index} is encrypted")
        require(compression in (1, 2), f"segment {index}: compression {compression}")
        require(offset + length <= len(data), f"segment {index} runs past the SELF's end")
        blob = data[offset:offset + length]
        if compression == 2:
            try:
                blob = zlib.decompressobj().decompress(blob)  # vita-make-fself pads after the stream
            except zlib.error as error:
                raise Invalid(f"segment {index}: {error}") from None
        else:
            blob = blob[:filesz]
        require(len(blob) == filesz, f"segment {index}: {len(blob)} bytes, the program header says {filesz}")
        segments.append((kind, vaddr, memsz, blob))

    loaded = [s for s in segments if s[0] == 1]
    seg, at = entry >> 30, entry & 0x3FFFFFFF
    require(seg < len(segments) and segments[seg][0] == 1, "e_entry names no loaded segment (no module info)")
    blob = segments[seg][3]
    size, magic, version = unpack("<I4sI", blob, at + MODULE_INFO_SIZE, "process parameters")
    require(magic == b"PSP2" and (size, version) in ((0x34, 6), (0x30, 5)),
            "no process parameters after the module info")
    words = unpack("<12I", blob, at + MODULE_INFO_SIZE, "process parameters")

    def value(pointer, symbol):
        require(pointer, f"the process parameters have no {symbol}: vita-elf-create found no such symbol "
                         "(settings/sysapp.c, the link's --undefined)")
        for _, vaddr, memsz, image in loaded:
            if vaddr <= pointer and pointer + 4 <= vaddr + memsz:
                where = pointer - vaddr
                return struct.unpack_from("<I", image, where)[0] if where + 4 <= len(image) else 0
        raise Invalid(f"{symbol}: pointer 0x{pointer:08x} is outside the SELF's segments")

    return {"is_used": is_used, "attribute": attribute, "phycont_kib": phycont_kib, "total_kib": total_kib,
            "priority": value(words[5], "sceUserMainThreadPriority"),
            "cpu_mask": value(words[10], "sceUserMainThreadCpuAffinityMask"),
            "segments": sum(s[2] for s in loaded)}


def read_elf(data):
    """The linked ELF's symbols (name -> [(value, size, section)]), sections and relocations."""
    require(data[:6] == b"\x7fELF\x01\x01", "not a little-endian ELF32")
    shoff, = unpack("<I", data, 32, "ELF header")
    shentsize, shnum = unpack("<HH", data, 46, "ELF header")
    require(shentsize == 40 and shnum, "no section headers")
    sections = [unpack("<10I", data, shoff + 40 * i, "section header") for i in range(shnum)]
    tables = [i for i, s in enumerate(sections) if s[1] == 2]  # SHT_SYMTAB
    require(tables, "no symbol table (the ELF is stripped)")
    symtab = sections[tables[0]]
    strings = sections[symtab[6]]

    def name(offset):
        start = strings[4] + offset
        end = data.index(b"\0", start)
        return data[start:end].decode("ascii", "replace")

    symbols, by_index = {}, []
    for i in range(symtab[5] // 16):
        st_name, st_value, st_size, _, _, st_shndx = unpack("<IIIBBH", data, symtab[4] + 16 * i, "symbol")
        entry = (name(st_name) if st_name else "", st_value, st_size, st_shndx)
        by_index.append(entry)
        if entry[0] and st_shndx:
            symbols.setdefault(entry[0], []).append(entry[1:])
    relocations = []  # (place, symbol name)
    for s in sections:
        if s[1] == 9 and s[6] == tables[0]:  # SHT_REL against the symbol table
            for offset in range(s[4], s[4] + s[5] - 7, 8):
                place, info = unpack("<II", data, offset, "relocation")
                if info >> 8 < len(by_index):
                    relocations.append((place, by_index[info >> 8][0]))
    return {"data": data, "sections": sections, "symbols": symbols, "relocations": relocations}


def heap_size(elf):
    found = elf["symbols"].get(HEAP_SYMBOL)
    require(found, f"{HEAP_SYMBOL} is not in the ELF: newlib's heap would be its default 128 MiB "
                   "(settings/sysapp.c)")
    value, size, index = found[0]
    require(size == 4 and index < len(elf["sections"]), f"{HEAP_SYMBOL}: a {size}-byte symbol")
    section = elf["sections"][index]
    require(section[1] == 1 and section[3] <= value and value + 4 <= section[3] + section[5],
            f"{HEAP_SYMBOL} has no initial value (section {index})")
    return struct.unpack_from("<I", elf["data"], section[4] + value - section[3])[0]


def thread_calls(elf):
    """The places that call sceKernelCreateThread, all inside the wrapper."""
    places = [place for place, symbol in elf["relocations"] if symbol == THREAD_IMPORT]
    for symbol in FORBIDDEN_CALLS:
        uses = [place for place, name in elf["relocations"] if name == symbol]
        if uses:
            raise Invalid(f"{symbol} is called at 0x{uses[0]:08x}: every thread stays on CPU3 (settings/sysapp.c)")
    if not places:
        return 0
    wrapper = elf["symbols"].get(THREAD_WRAPPER)
    if not wrapper:
        raise Invalid(f"{THREAD_IMPORT} is called at 0x{places[0]:08x} with no {THREAD_WRAPPER} in the ELF "
                      "(settings/sysapp.c, the link's --wrap)")
    start = wrapper[0][0] & ~1
    end = start + wrapper[0][1]
    outside = [place for place in places if not start <= place < end]
    if outside:
        raise Invalid(f"{THREAD_IMPORT} is called at 0x{outside[0]:08x}, outside {THREAD_WRAPPER} "
                      f"(0x{start:08x}-0x{end:08x}): the link's --wrap=sceKernelCreateThread is missing")
    return sum(1 for _, symbol in elf["relocations"] if symbol == THREAD_WRAPPER)


def graphics_calls(elf):
    """libvita2d's calls of each graphics import go to its wrapper, and the import is called only
    from there (the wrapper's __real_ call): {import: the calls of its wrapper}."""
    counts = {}
    for name in GRAPHICS_IMPORTS:
        wrapper = "__wrap_" + name
        calls = sum(1 for _, symbol in elf["relocations"] if symbol == wrapper)
        if not calls:
            raise Invalid(f"nothing calls {wrapper}: the link's --wrap={name} is missing, so libvita2d's system start "
                          "does not wait for SceShell (settings/sysapp_gfx.c)")
        found = elf["symbols"].get(wrapper)
        if not found:
            raise Invalid(f"{wrapper} is called but not defined (settings/sysapp_gfx.c)")
        start = found[0][0] & ~1
        end = start + found[0][1]
        outside = [place for place, symbol in elf["relocations"] if symbol == name and not start <= place < end]
        if outside:
            raise Invalid(f"{name} is called at 0x{outside[0]:08x}, outside {wrapper} (0x{start:08x}-0x{end:08x}): "
                          f"the link's --wrap={name} is missing there")
        counts[name] = calls
    return counts


def atlas_calls(elf):
    """The glyph atlas's texture is made through the wrapper: texture_atlas_create calls
    __wrap_vita2d_create_empty_texture_format, never the function itself. The places it calls the wrapper from."""
    wrapper = "__wrap_" + TEXTURE_FUNCTION
    atlas = elf["symbols"].get(ATLAS_FUNCTION)
    require(atlas, f"{ATLAS_FUNCTION} is not in the ELF: libvita2d's fonts have no atlas to check")
    require(elf["symbols"].get(wrapper), f"{wrapper} is not defined (settings/vita2d_textures.c)")
    start = atlas[0][0] & ~1
    end = start + atlas[0][1]
    require(atlas[0][1] > 0, f"{ATLAS_FUNCTION} has no size in the symbol table")
    inside = [(place, name) for place, name in elf["relocations"] if start <= place < end]
    direct = [place for place, name in inside if name == TEXTURE_FUNCTION]
    if direct:
        raise Invalid(f"{ATLAS_FUNCTION} calls {TEXTURE_FUNCTION} at 0x{direct[0]:08x}, not {wrapper}: the link's "
                      f"--wrap={TEXTURE_FUNCTION} is missing, so a font freed while the app runs may free another "
                      "block (settings/vita2d_textures.c)")
    calls = [place for place, name in inside if name == wrapper]
    require(calls, f"{ATLAS_FUNCTION} does not call {wrapper} (settings/vita2d_textures.c, the link's --wrap)")
    return len(calls)


def check(self_path, elf_path):
    sce = read_self(Path(self_path).read_bytes())
    require(sce["is_used"] == 1, "control info 6 is not in use")
    require(sce["attribute"] == ATTRIBUTE,
            f"SELF attribute 0x{sce['attribute']:02X}, not 0x{ATTRIBUTE:02X}: not a system application "
            "(vita_create_self ATTRIBUTE 0x0E)")
    require(sce["total_kib"] == BUDGET_KIB,
            f"SELF budget 0x{sce['total_kib']:X} KiB, not 0x{BUDGET_KIB:X} (vita_create_self MEMSIZE 0xC800)")
    require(sce["phycont_kib"] == 0, f"SELF asks for 0x{sce['phycont_kib']:X} KiB of phycont, not none")
    require(sce["priority"] == PRIORITY,
            f"main thread priority 0x{sce['priority']:08X}, not 0x{PRIORITY:08X} (settings/sysapp.c)")
    require(sce["cpu_mask"] == CPU_MASK_SYSTEM,
            f"main thread CPU mask 0x{sce['cpu_mask']:08X}, not SCE_KERNEL_CPU_MASK_SYSTEM "
            f"0x{CPU_MASK_SYSTEM:08X} (settings/sysapp.c)")
    elf = read_elf(Path(elf_path).read_bytes())
    heap = heap_size(elf)
    budget = BUDGET_KIB * 1024
    used = heap + sce["segments"] + sum(FIXED.values())
    require(used <= budget, f"heap {heap} B, segments {sce['segments']} B and libvita2d's, the icons', the "
                            f"atlas' and the stacks' {sum(FIXED.values())} B are {used} B, more than the "
                            f"{budget} B budget")
    calls = thread_calls(elf)
    graphics = graphics_calls(elf)
    graphics[TEXTURE_FUNCTION + " (the glyph atlas)"] = atlas_calls(elf)
    return {"attribute": f"0x{sce['attribute']:02X}", "budget_bytes": budget, "phycont_bytes": 0,
            "main_thread": {"priority": f"0x{sce['priority']:08X}", "cpu_mask": f"0x{sce['cpu_mask']:08X}"},
            "heap_bytes": heap, "segment_bytes": sce["segments"],
            "at_least_bytes": used, "headroom_bytes": budget - used, "estimated": FIXED,
            "thread_creations_through_the_wrapper": calls, "graphics_calls_through_the_wrappers": graphics,
            "result": "pass"}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("self", type=Path, help="the Rubi app's eboot.bin")
    parser.add_argument("--elf", type=Path, required=True, help="its linked ELF (VitaJPOverlay_Settings)")
    args = parser.parse_args()
    try:
        report = check(args.self, args.elf)
    except Invalid as error:
        print(f"check_settings_self: FAIL: {error}", file=sys.stderr)
        sys.exit(1)
    print(json.dumps({"self": str(args.self), **report}, indent=2))


if __name__ == "__main__":
    main()
