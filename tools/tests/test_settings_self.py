"""tools/check_settings_self.py on synthetic SELF and ELF files laid out as vita-make-fself and the
linker write them (vita-toolchain src/self.h, src/vita-make-fself/vita-make-fself.c,
src/vita-elf-create/sce-elf.c), one change at a time: audit-pr109's mutants M1-M7 and the rest of
what the checker reads. The Vita build runs the checker on the real eboot.bin
(vita/CMakeLists.txt vjo_settings_self); VJO_SETTINGS_SELF and VJO_SETTINGS_ELF name a built pair
for test_built_eboot_passes."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools/tests"))
import check_settings_self as check  # noqa: E402
from self_fixture import PT_SCE_RELA, Segment, fself  # noqa: E402

CODE, DATA = 0x81000000, 0x81090000
PRIORITY_AT, MASK_AT = 0x200, 0x204  # the two thread values in the code segment's .rodata
INFO_AT = 0x400                      # the module info, then the process parameters
WRAP, WRAP_SIZE = 0x81000100, 0x20   # __wrap_sceKernelCreateThread (Thumb)
STUB = 0x81000300                    # the sceKernelCreateThread import stub
HEAP = 8 * 1024 * 1024
# settings/sysapp_gfx.c's wrappers of libvita2d's two calls into the system, their import stubs,
# and libvita2d's calls of them (vita2d.c's system start)
GXM_WRAP, GXM_STUB, GXM_CALL = 0x81000140, 0x81000320, CODE + 0x2A0
FB_WRAP, FB_STUB, FB_CALL = 0x81000180, 0x81000330, CODE + 0x2B0
GFX_WRAP_SIZE = 0x40
# libvita2d's texture_atlas_create, settings/vita2d_textures.c's wrapper of vita2d_create_empty_texture_format, and
# the function itself (#313: a font's atlas freed while the app runs frees nothing else)
ATLAS, TEX_WRAP, TEX_FN = 0x810001C0, 0x81000200, 0x81000340


def code_segment(priority=0x10000100, mask=0x80000, pointers=True, magic=b"PSP2"):
    blob = bytearray(INFO_AT + 0x5C + 0x34)
    struct.pack_into("<II", blob, PRIORITY_AT, priority, mask)
    blob[INFO_AT + 4:INFO_AT + 26] = b"VitaJPOverlay_Settings"
    blob[INFO_AT + 31] = 6
    params = [0x34, struct.unpack("<I", magic)[0], 6, 0x3570011, 0, CODE + PRIORITY_AT if pointers else 0,
              0, 0, 0, 0, CODE + MASK_AT if pointers else 0, 0, 0]
    struct.pack_into("<13I", blob, INFO_AT + 0x5C, *params)
    return bytes(blob)


def make_self(code=None, attribute=0x0E, phycont=0, total=0xC800, compress=True, control6=True,
              data_memsz=0x186C30, is_used=1):
    """The app's fself (self_fixture.fself: vita-make-fself's layout, the segments from 0x1000): the code segment
    with the module info at INFO_AT, the data segment with its .bss and the relocations; control info 6 with these
    boot parameters, or none."""
    return fself(Segment(code if code is not None else code_segment(), vaddr=CODE),
                 Segment(b"\x5a" * 0x1074, memsz=data_memsz, vaddr=DATA),
                 Segment(b"\0" * 0x30, memsz=0, kind=PT_SCE_RELA),
                 entry=INFO_AT, compress=compress, boot=(is_used, attribute, phycont, total) if control6 else None)


def gfx_symbols():
    return {"__wrap_sceGxmVshInitialize": (GXM_WRAP | 1, GFX_WRAP_SIZE, ".text"),
            "sceGxmVshInitialize": (GXM_STUB, 16, ".text"),
            "__wrap_sceSharedFbGetInfo": (FB_WRAP | 1, GFX_WRAP_SIZE, ".text"),
            "sceSharedFbGetInfo": (FB_STUB, 16, ".text"),
            **atlas_symbols()}


def atlas_symbols():
    return {"texture_atlas_create": (ATLAS | 1, 0x40, ".text"),
            "__wrap_vita2d_create_empty_texture_format": (TEX_WRAP | 1, 0x20, ".text"),
            "vita2d_create_empty_texture_format": (TEX_FN | 1, 0x40, ".text")}


def atlas_relocations(wrapped=True):
    """The atlas calls the wrapper (the link's --wrap), which calls the function (its __real_)."""
    return [(ATLAS + 0x10, "__wrap_vita2d_create_empty_texture_format" if wrapped else "vita2d_create_empty_texture_format",
             10), (TEX_WRAP + 0x08, "vita2d_create_empty_texture_format", 10)]


def gfx_relocations():
    """Each wrapper calls the import (its __real_, twice for the GPU library's second try); libvita2d
    calls the wrappers."""
    return [(GXM_WRAP + 0x08, "sceGxmVshInitialize", 30), (GXM_WRAP + 0x20, "sceGxmVshInitialize", 30),
            (FB_WRAP + 0x08, "sceSharedFbGetInfo", 30),
            (GXM_CALL, "__wrap_sceGxmVshInitialize", 10), (FB_CALL, "__wrap_sceSharedFbGetInfo", 10),
            *atlas_relocations()]


def make_elf(symbols=None, relocations=None, heap=HEAP, heap_section=".data", gfx=True):
    """A linked ELF32 with .text, .data, .bss, a symbol table and .rel.text (the link's -q).
    symbols: name -> (value, size, section name); relocations: [(place, symbol name, type)];
    gfx: add settings/sysapp_gfx.c's wrappers and libvita2d's calls of them."""
    if symbols is None:
        symbols = {"__wrap_sceKernelCreateThread": (WRAP | 1, WRAP_SIZE, ".text"),
                   "sceKernelCreateThread": (STUB, 16, ".text")}
        if heap is not None:
            symbols[check.HEAP_SYMBOL] = (DATA + 0x24, 4, heap_section)
    if relocations is None:
        relocations = [(WRAP + 0x18, "sceKernelCreateThread", 30), (CODE + 0x2E0, "__wrap_sceKernelCreateThread", 10)]
    if gfx:
        symbols = {**gfx_symbols(), **symbols}
        relocations = gfx_relocations() + list(relocations)
    text = b"\0" * 0x400
    data = bytearray(0x40)
    if heap is not None:
        struct.pack_into("<I", data, 0x24, heap)
    names = [".text", ".data", ".bss", ".symtab", ".strtab", ".rel.text", ".shstrtab"]
    index = {name: i + 1 for i, name in enumerate(names)}
    strtab, offsets = b"\0", {}
    for name in symbols:
        offsets[name] = len(strtab)
        strtab += name.encode() + b"\0"
    order = list(symbols)
    symtab = b"\0" * 16
    for name in order:
        value, size, section = symbols[name]
        symtab += struct.pack("<IIIBBH", offsets[name], value, size, 0x12, 0, index[section])
    rel = b"".join(struct.pack("<II", place, (order.index(name) + 1) << 8 | kind)
                   for place, name, kind in relocations)
    shstrtab = b"\0" + b"".join(n.encode() + b"\0" for n in names)
    bodies = {".text": text, ".data": bytes(data), ".bss": b"", ".symtab": symtab, ".strtab": strtab,
              ".rel.text": rel, ".shstrtab": shstrtab}
    out = bytearray(0x34)
    placed = {}
    for name in names:
        placed[name] = len(out)
        out += bodies[name]
        out += b"\0" * (-len(out) % 4)
    shoff = len(out)
    out += b"\0" * 40
    name_at = 1
    for name in names:
        kind = {".symtab": 2, ".strtab": 3, ".rel.text": 9, ".shstrtab": 3, ".bss": 8}.get(name, 1)
        addr = {".text": CODE, ".data": DATA, ".bss": DATA + 0x1000}.get(name, 0)
        size = 0x40 if name == ".bss" else len(bodies[name])
        link = index[".strtab"] if name == ".symtab" else index[".symtab"] if name == ".rel.text" else 0
        info = index[".text"] if name == ".rel.text" else 0
        out += struct.pack("<10I", name_at, kind, 0, addr, placed[name], size, link, info, 4,
                           16 if name == ".symtab" else 8 if name == ".rel.text" else 0)
        name_at += len(name) + 1
    out[0:8] = b"\x7fELF\x01\x01\x01\0"
    struct.pack_into("<HHIIIIIHHHHHH", out, 16, 2, 40, 1, CODE, 0, shoff, 0x05000000, 0x34, 32, 0, 40,
                     len(names) + 1, index[".shstrtab"])
    return bytes(out)


class SettingsSelf(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def run_check(self, self_bytes=None, elf_bytes=None):
        directory = Path(self.tmp.name)
        (directory / "eboot.bin").write_bytes(self_bytes if self_bytes is not None else make_self())
        (directory / "app.elf").write_bytes(elf_bytes if elf_bytes is not None else make_elf())
        return check.check(directory / "eboot.bin", directory / "app.elf")

    def fails(self, pattern, **parts):
        with self.assertRaisesRegex(check.Invalid, pattern):
            self.run_check(**parts)

    def test_a_system_application_passes(self):
        report = self.run_check()
        self.assertEqual(report["result"], "pass")
        self.assertEqual(report["attribute"], "0x0E")
        self.assertEqual(report["budget_bytes"], 50 * 1024 * 1024)
        self.assertEqual(report["main_thread"], {"priority": "0x10000100", "cpu_mask": "0x00080000"})
        self.assertEqual(report["heap_bytes"], HEAP)
        self.assertEqual(report["segment_bytes"], len(code_segment()) + 0x186C30)
        self.assertEqual(report["thread_creations_through_the_wrapper"], 1)
        self.assertEqual(report["at_least_bytes"], HEAP + report["segment_bytes"] + sum(check.FIXED.values()))
        # vita-make-fself without -c stores the segments as they are
        self.assertEqual(self.run_check(make_self(compress=False))["result"], "pass")

    def test_m2_m7_the_attribute_and_the_budget(self):
        self.fails("attribute 0x00, not 0x0E", self_bytes=make_self(attribute=0, total=0))      # M2
        self.fails("attribute 0x00, not 0x0E", self_bytes=make_self(attribute=0))               # M7
        self.fails("budget 0xA000 KiB, not 0xC800", self_bytes=make_self(total=0xA000))
        self.fails("budget 0x0 KiB", self_bytes=make_self(total=0))
        self.fails("phycont", self_bytes=make_self(phycont=0x400))
        self.fails("no control info 6", self_bytes=make_self(control6=False))
        self.fails("control info 6 is not in use", self_bytes=make_self(is_used=0))

    def test_m1_m3_m6_the_main_thread(self):
        # M1 (no --undefined) and M3 (no settings/sysapp.c): vita-elf-create writes null pointers
        self.fails("no sceUserMainThreadPriority", self_bytes=make_self(code_segment(pointers=False)))
        self.fails("priority 0x000000A0, not 0x10000100", self_bytes=make_self(code_segment(priority=0xA0)))  # M6
        self.fails("CPU mask 0x00000000", self_bytes=make_self(code_segment(mask=0)))
        self.fails("CPU mask 0x00070000", self_bytes=make_self(code_segment(mask=0x70000)))
        self.fails("no process parameters", self_bytes=make_self(code_segment(magic=b"PSP3")))

    def test_m3_m4_the_heap(self):
        self.fails("_newlib_heap_size_user is not in the ELF", elf_bytes=make_elf(heap=None))  # M3
        self.fails("more than the 52428800 B budget", elf_bytes=make_elf(heap=64 * 1024 * 1024))  # M4
        self.fails("no initial value", elf_bytes=make_elf(heap_section=".bss"))
        # the segments count too: a huge .bss with the 8 MiB heap does not fit either
        self.fails("more than the 52428800 B budget", self_bytes=make_self(data_memsz=40 * 1024 * 1024))
        self.assertEqual(self.run_check(elf_bytes=make_elf(heap=16 * 1024 * 1024))["heap_bytes"], 16 * 1024 * 1024)

    def test_m5_every_thread_through_the_wrapper(self):
        # the link without --wrap: the worker's call goes to the import, and the wrapper is gc'd
        self.fails("with no __wrap_sceKernelCreateThread",
                   elf_bytes=make_elf(symbols={"sceKernelCreateThread": (STUB, 16, ".text"),
                                               check.HEAP_SYMBOL: (DATA + 0x24, 4, ".data")},
                                      relocations=[(CODE + 0x2E0, "sceKernelCreateThread", 10)]))
        # a second call of the import, outside the wrapper
        self.fails("called at 0x810002e0, outside __wrap_sceKernelCreateThread",
                   elf_bytes=make_elf(relocations=[(WRAP + 0x18, "sceKernelCreateThread", 30),
                                                   (CODE + 0x2E0, "sceKernelCreateThread", 10)]))
        # its address taken elsewhere (a pointer to it, R_ARM_ABS32) counts the same
        self.fails("outside", elf_bytes=make_elf(relocations=[(WRAP + 0x18, "sceKernelCreateThread", 30),
                                                              (DATA + 0x10, "sceKernelCreateThread", 2)]))
        self.fails("sceKernelChangeThreadCpuAffinityMask is called",
                   elf_bytes=make_elf(symbols={"__wrap_sceKernelCreateThread": (WRAP | 1, WRAP_SIZE, ".text"),
                                               "sceKernelCreateThread": (STUB, 16, ".text"),
                                               "sceKernelChangeThreadCpuAffinityMask": (STUB + 16, 16, ".text"),
                                               check.HEAP_SYMBOL: (DATA + 0x24, 4, ".data")},
                                      relocations=[(WRAP + 0x18, "sceKernelCreateThread", 30),
                                                   (CODE + 0x80, "sceKernelChangeThreadCpuAffinityMask", 10)]))
        # an app that creates no thread needs no wrapper (--gc-sections drops it)
        report = self.run_check(elf_bytes=make_elf(symbols={check.HEAP_SYMBOL: (DATA + 0x24, 4, ".data")},
                                                   relocations=[]))
        self.assertEqual(report["thread_creations_through_the_wrapper"], 0)

    def test_libvita2d_starts_through_the_graphics_wrappers(self):
        """settings/sysapp_gfx.c: libvita2d's sceGxmVshInitialize and sceSharedFbGetInfo go through the
        wrappers that make its system start wait for SceShell and fall back (no game running)."""
        report = self.run_check()
        self.assertEqual(report["graphics_calls_through_the_wrappers"],
                         {"sceGxmVshInitialize": 1, "sceSharedFbGetInfo": 1,
                          "vita2d_create_empty_texture_format (the glyph atlas)": 1})
        for name, place in (("sceGxmVshInitialize", GXM_CALL), ("sceSharedFbGetInfo", FB_CALL)):
            with self.subTest(name):
                # the link without --wrap=name: libvita2d calls the import, and the wrapper is gc'd
                symbols = {k: v for k, v in gfx_symbols().items() if k != "__wrap_" + name}
                relocations = [r for r in gfx_relocations() if r[1] not in (name, "__wrap_" + name)]
                relocations.append((place, name, 10))
                elf = make_elf(gfx=False, symbols={
                    **symbols, "__wrap_sceKernelCreateThread": (WRAP | 1, WRAP_SIZE, ".text"),
                    "sceKernelCreateThread": (STUB, 16, ".text"), check.HEAP_SYMBOL: (DATA + 0x24, 4, ".data")},
                    relocations=relocations + [(WRAP + 0x18, "sceKernelCreateThread", 30)])
                self.fails(f"nothing calls __wrap_{name}", elf_bytes=elf)
                # the wrapper there, and a second call of the import outside it
                elf = make_elf(relocations=[(WRAP + 0x18, "sceKernelCreateThread", 30), (place + 0x40, name, 10)])
                self.fails(f"{name} is called at 0x{place + 0x40:08x}, outside __wrap_{name}", elf_bytes=elf)
        # main's eboot (#109 to this change): no graphics wrapper at all
        self.fails("nothing calls __wrap_sceGxmVshInitialize",
                   elf_bytes=make_elf(gfx=False, relocations=[(WRAP + 0x18, "sceKernelCreateThread", 30),
                                                              (GXM_CALL, "sceGxmVshInitialize", 10),
                                                              (FB_CALL, "sceSharedFbGetInfo", 10)],
                                      symbols={"__wrap_sceKernelCreateThread": (WRAP | 1, WRAP_SIZE, ".text"),
                                               "sceKernelCreateThread": (STUB, 16, ".text"),
                                               "sceGxmVshInitialize": (GXM_STUB, 16, ".text"),
                                               "sceSharedFbGetInfo": (FB_STUB, 16, ".text"),
                                               check.HEAP_SYMBOL: (DATA + 0x24, 4, ".data")}))

    def test_the_glyph_atlas_is_made_through_the_texture_wrapper(self):
        """settings/vita2d_textures.c (#313): libvita2d's texture_atlas_create calls the wrapper of
        vita2d_create_empty_texture_format, which clears a plain texture's stale render target and depth buffer,
        so that a font freed while the app runs frees nothing else."""
        def elf(symbols, relocations):
            return make_elf(gfx=False, symbols={
                **{k: v for k, v in gfx_symbols().items() if k not in atlas_symbols()}, **symbols,
                "__wrap_sceKernelCreateThread": (WRAP | 1, WRAP_SIZE, ".text"), "sceKernelCreateThread": (STUB, 16, ".text"),
                check.HEAP_SYMBOL: (DATA + 0x24, 4, ".data")},
                relocations=[r for r in gfx_relocations() if r not in atlas_relocations()] + relocations +
                [(WRAP + 0x18, "sceKernelCreateThread", 30)])
        # the link without --wrap=vita2d_create_empty_texture_format: the atlas calls the function, the wrapper is gc'd
        no_wrap = {k: v for k, v in atlas_symbols().items() if not k.startswith("__wrap_")}
        self.fails("__wrap_vita2d_create_empty_texture_format is not defined",
                   elf_bytes=elf(no_wrap, [(ATLAS + 0x10, "vita2d_create_empty_texture_format", 10)]))
        # the wrapper there (settings/vita2d_textures.c linked), but the atlas not calling it
        self.fails("texture_atlas_create calls vita2d_create_empty_texture_format at 0x810001d0, not "
                   "__wrap_vita2d_create_empty_texture_format", elf_bytes=elf(atlas_symbols(), atlas_relocations(False)))
        self.fails("texture_atlas_create does not call __wrap_vita2d_create_empty_texture_format",
                   elf_bytes=elf(atlas_symbols(), atlas_relocations()[1:]))
        # no font in the app at all
        self.fails("texture_atlas_create is not in the ELF",
                   elf_bytes=elf({k: v for k, v in atlas_symbols().items() if k != "texture_atlas_create"},
                                 atlas_relocations()[1:]))

    def test_not_a_self_or_not_an_elf(self):
        self.fails("not a SELF", self_bytes=b"\x7fELF" + b"\0" * 200)
        self.fails("not a little-endian ELF32", elf_bytes=b"SCE\0" + b"\0" * 200)
        broken = bytearray(make_self())
        broken[0x1000:0x1004] = b"\xff\xff\xff\xff"  # the first segment's zlib header
        self.fails("segment 0", self_bytes=bytes(broken))

    def test_the_command_line_fails_with_the_reason(self):
        directory = Path(self.tmp.name)
        (directory / "eboot.bin").write_bytes(make_self(attribute=0))
        (directory / "app.elf").write_bytes(make_elf())
        result = subprocess.run([sys.executable, str(ROOT / "tools/check_settings_self.py"), "--elf",
                                 str(directory / "app.elf"), str(directory / "eboot.bin")],
                                capture_output=True, text=True, timeout=60, check=False)
        self.assertEqual(result.returncode, 1)
        self.assertIn("check_settings_self: FAIL: SELF attribute 0x00", result.stderr)

    @unittest.skipUnless(os.environ.get("VJO_SETTINGS_SELF") and os.environ.get("VJO_SETTINGS_ELF"),
                         "set VJO_SETTINGS_SELF and VJO_SETTINGS_ELF to a built eboot.bin and its ELF")
    def test_built_eboot_passes(self):
        report = check.check(os.environ["VJO_SETTINGS_SELF"], os.environ["VJO_SETTINGS_ELF"])
        self.assertEqual(report["result"], "pass")


if __name__ == "__main__":
    unittest.main()
