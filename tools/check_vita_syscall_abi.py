#!/usr/bin/env python3
"""Audit the overlay's custom syscall linkage in packaged Vita SELF files.

This checks file metadata and relocations, not firmware's runtime binding or
successful boot. It accepts the unencrypted, compressed or uncompressed SELF
files produced by VitaSDK and its default (unoptimized) relocation format.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

import vita_self  # tools/vita_self.py: the SELF's headers and segments


LIBRARY_NID = 0xAE5E1E5E
REQUIRED = {"vjoGetVersion": 0x738D8AB1, "vjoRegisterShell": 0x1EF2A82A}
HOOKS = {
    "vjoTextSubmit": 0x06E8E79D,
    "vjoTextControl": 0x327A1EF3,
    "vjoTextReference": 0x2B6DDFAB,
    "vjoTextRead": 0x834C749E,
}
GAME_FUNCTIONS = ("vjoOcrRegister", "vjoOcrTake", "vjoOcrCancelled", "vjoOcrCaptureRows",
                  "vjoOcrComplete", "vjoOcrSubmit", "vjoOcrRead", "vjoOcrCancel",
                  "vjoOcrModelOpen", "vjoOcrModelRead", "vjoOcrModelClose", "vjoOcrHookProfile",
                  # API 23: the Anki screenshot's JPEG, written through the kernel's worker
                  "vjoOcrShotWrite", "vjoOcrShotSave", "vjoOcrHookCache",
                  # API 41: the hook watch's new line (the worker's thread, between jobs)
                  "vjoOcrHookLine")
# Removed syscalls, with the API that removed them: no module may still export or import one.
# API 19 the game worker's RGBA reader; API 24 the kernel's full-frame capture and its reader.
REMOVED = {"vjoOcrReadRaw": 19, "vjoRequestCapture": 24, "vjoReadRaw": 24}
GAME = {name: int.from_bytes(hashlib.sha256(name.encode()).digest()[:4], "big")
        for name in GAME_FUNCTIONS}
# API 31, the text source (docs/native-hooks.md): the Shell sets it from config.ini's text_source, the
# game worker reads it as its game starts. Set is the registered Shell's only (the worker never imports
# it); the kernel exports both in every build.
TEXT_SOURCE = {name: int.from_bytes(hashlib.sha256(name.encode()).digest()[:4], "big")
               for name in ("vjoSetTextSource", "vjoGetTextSource")}
WEAK_STUB = (0xE3E00000, 0xE12FFF1E, 0xE1A00000, 0)
# Sysmem's ForKernel library was renumbered in 3.63. A direct strong import
# makes an otherwise valid plugin fail before module_start on the other ABI.
# The overlay uses the stable ForDriver memory-info API instead.
FIRMWARE_SPECIFIC_SYSMEM = {0x63A519E5: "3.60", 0x02451F0F: "3.63+"}


class InvalidABI(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise InvalidABI(message)


def unpack(fmt, data, offset):
    require(0 <= offset <= len(data) - struct.calcsize(fmt),
            f"Truncated structure at file/segment offset 0x{offset:x}")
    return struct.unpack_from(fmt, data, offset)


class SelfFile:
    def __init__(self, path):
        self.path = str(path)
        data = Path(path).read_bytes()
        self.sha256 = hashlib.sha256(data).hexdigest()
        try:
            sce = vita_self.read(data)
        except vita_self.Invalid as error:
            raise InvalidABI(str(error)) from None
        require(sce.elf_type == 0xFE04 and sce.machine == 40, "Expected a relocatable Vita ARM module")
        self.segments = []
        self.relocations = {}
        for s in sce.segments:
            self.segments.append((s.vaddr, s.memsz, s.flags, s.data))
            if s.kind == vita_self.PT_SCE_RELA:
                self.read_relocations(s.data)
            else:
                require(s.kind == vita_self.PT_LOAD and s.filesz <= s.memsz, "Unexpected SELF segment")
        self.info_segment = sce.entry >> 30
        require(self.info_segment < len(self.segments), "Invalid module-info segment")
        self.info_offset = sce.entry & 0x3FFFFFFF
        self.code = self.segments[self.info_segment][3]
        info = self.read(self.info_segment, self.info_offset, 92)
        require(info[31] == 6, "Expected a PRX module")
        self.name = info[4:31].split(b"\0", 1)[0].decode("ascii")
        self.export_top, self.export_end, self.import_top, self.import_end = unpack(
            "<4I", info, 36)

    def read_relocations(self, blob):
        require(len(blob) % 12 == 0, "Unsupported optimized relocation table")
        for offset in range(0, len(blob), 12):
            word, addend, place = unpack("<3I", blob, offset)
            require(word & 15 == 0 and word >> 20 == 0,
                    "Unsupported optimized or paired relocation")
            key = ((word >> 16) & 15, place)
            relocation = ((word >> 4) & 15, addend, (word >> 8) & 255)
            self.relocations.setdefault(key, []).append(relocation)

    def read(self, segment, offset, size):
        require(0 <= segment < len(self.segments), "Invalid segment index")
        blob = self.segments[segment][3]
        require(0 <= offset <= len(blob) and size <= len(blob) - offset,
                f"Pointer extends outside segment {segment}: 0x{offset:x}+{size}")
        return blob[offset:offset + size]

    def locate(self, pointer, size):
        for index, (address, _, _, blob) in enumerate(self.segments):
            if address <= pointer and pointer + size <= address + len(blob):
                return index, pointer - address
        raise InvalidABI(f"Unmapped file pointer 0x{pointer:x}+{size}")

    def pointer(self, segment, offset, size):
        pointer, = unpack("<I", self.read(segment, offset, 4), 0)
        target_segment, target_offset = self.locate(pointer, size)
        expected = [(target_segment, target_offset, 2)]  # R_ARM_ABS32
        require(self.relocations.get((segment, offset)) == expected,
                f"Missing/wrong ABS32 relocation for pointer at segment {segment}+0x{offset:x}")
        return target_segment, target_offset

    def library(self, exports):
        top, end = ((self.export_top, self.export_end) if exports else
                    (self.import_top, self.import_end))
        require(0 <= top <= end <= len(self.code), "Invalid module table bounds")
        found = []
        offset = top
        while offset < end:
            size, version, attributes, functions = unpack("<4H", self.code, offset)
            require(size in ((32,) if exports else (36, 52)) and offset + size <= end,
                    "Unsupported module table entry size")
            if exports:
                nid_at, name_at, nids_at, entries_at = 16, 20, 24, 28
            elif size == 52:
                nid_at, name_at, nids_at, entries_at = 16, 20, 28, 32
            else:
                nid_at, name_at, nids_at, entries_at = 12, 16, 20, 24
            nid, = unpack("<I", self.code, offset + nid_at)
            if nid == LIBRARY_NID:
                require(functions > 0, "Empty custom syscall library")
                self.pointer(self.info_segment, offset + name_at, 1)
                ns, no = self.pointer(self.info_segment, offset + nids_at, functions * 4)
                es, eo = self.pointer(self.info_segment, offset + entries_at, functions * 4)
                symbols = {}
                for index in range(functions):
                    function_nid, = unpack("<I", self.read(ns, no + index * 4, 4), 0)
                    require(function_nid not in symbols, "Duplicate custom syscall NID")
                    target = self.pointer(es, eo + index * 4, 1 if exports else 16)
                    symbols[function_nid] = target
                found.append((version, attributes, symbols))
            offset += size
        require(len(found) == 1, "Expected exactly one custom syscall library")
        return found[0]

    def check_firmware_imports(self):
        offset = self.import_top
        require(0 <= offset <= self.import_end <= len(self.code), "Invalid import table bounds")
        while offset < self.import_end:
            size, _, attributes = unpack("<3H", self.code, offset)
            require(size in (36, 52) and offset + size <= self.import_end,
                    "Unsupported import table entry size")
            nid, = unpack("<I", self.code, offset + (16 if size == 52 else 12))
            require(nid not in FIRMWARE_SPECIFIC_SYSMEM or attributes & 8,
                    f"Strong SceSysmemForKernel import 0x{nid:08x} is firmware-specific; "
                    "use the stable ForDriver API for 3.60/3.65 compatibility")
            offset += size


def audit(kernel, shell, require_hooks=False, worker=None):
    require(kernel.name == "VitaJPOverlay_Kernel", "Wrong kernel module name")
    require(shell.name == "VitaJPOverlay_Shell", "Wrong shell module name")
    kernel.check_firmware_imports()
    version, attributes, exports = kernel.library(True)
    import_version, import_attributes, imports = shell.library(False)
    require(attributes == 0x4001, "Kernel library must export syscalls (0x4001)")
    require(import_attributes == 8, "Shell custom syscalls must use weak imports")
    require(version == import_version, "Custom syscall library versions differ")
    required = dict(REQUIRED, **(HOOKS if require_hooks else {}))
    for name, nid in required.items():
        require(nid in exports, f"Kernel does not export {name}")
        if name != "vjoTextSubmit":
            require(nid in imports, f"Shell does not import {name}")
    for name, nid in TEXT_SOURCE.items():
        require(nid in exports, f"Kernel does not export {name}")
    require(TEXT_SOURCE["vjoSetTextSource"] in imports, "Shell does not import vjoSetTextSource")
    # removed syscalls: the kernel exports none and the Shell imports none, in every audit (the worker's below)
    removed = {int.from_bytes(hashlib.sha256(n.encode()).digest()[:4], "big"): (n, api) for n, api in REMOVED.items()}
    for nid, (name, api) in removed.items():
        require(nid not in exports and nid not in imports, f"{name} was removed in API {api}")
    for nid, (segment, offset) in exports.items():
        thumb = offset & 1
        address = offset & ~1
        require(kernel.segments[segment][2] & 1, "Syscall target is not executable")
        require(address % (2 if thumb else 4) == 0, "Misaligned syscall target")
        kernel.read(segment, address, 2 if thumb else 4)
    for nid, (segment, offset) in imports.items():
        require(nid in exports, f"Shell imports missing syscall 0x{nid:08x}")
        require(shell.segments[segment][2] & 1 and offset % 4 == 0,
                "Import stub must be aligned ARM code in an executable segment")
        words = unpack("<4I", shell.read(segment, offset, 16), 0)
        require(words == WEAK_STUB,
                f"Unexpected unbound weak stub for syscall 0x{nid:08x}")
    if worker:
        require(worker.name == "VitaJPOverlay_OCR", "Wrong game worker module name")
        worker_version, worker_attributes, worker_imports = worker.library(False)
        require(worker_version == version and worker_attributes == 8, "Worker must use matching weak custom imports")
        for name, nid in GAME.items():
            require(nid in exports, f"Kernel does not export {name}")
            owner = imports if name in ("vjoOcrSubmit", "vjoOcrRead", "vjoOcrCancel") else worker_imports
            require(nid in owner, f"Missing role-specific import {name}")
        for nid, (name, api) in removed.items():
            require(nid not in worker_imports, f"{name} was removed in API {api}")
        shell_only = ("vjoOcrSubmit", "vjoOcrRead", "vjoOcrCancel", "vjoRegisterShell", "vjoSetTextSource")
        for name in shell_only:
            nid = int.from_bytes(hashlib.sha256(name.encode()).digest()[:4], "big")
            require(nid not in worker_imports, f"Worker imports Shell-only syscall {name}")
        # API 31: the worker reads the text source as its game starts (which hooks it installs)
        require(TEXT_SOURCE["vjoGetTextSource"] in worker_imports, "Worker does not import vjoGetTextSource")
        # API 23: the JPEG's bytes go from the game worker to the kernel, never through SceShell; API 41: a hooked
        # line is the worker's to report
        for name in ("vjoOcrShotWrite", "vjoOcrShotSave", "vjoOcrHookCache", "vjoOcrHookLine"):
            require(GAME[name] not in imports, f"Shell imports the game worker's {name}")
        for nid, (segment, offset) in worker_imports.items():
            require(nid in exports, f"Worker imports missing syscall 0x{nid:08x}")
            require(worker.segments[segment][2] & 1 and offset % 4 == 0, "Bad worker import location")
            require(unpack("<4I", worker.read(segment, offset, 16), 0) == WEAK_STUB, "Bad worker weak stub")
    return {
        "result": "pass",
        "scope": "packaged metadata and relocations; runtime binding and boot unverified",
        "library_nid": f"0x{LIBRARY_NID:08x}",
        "library_version": version,
        "kernel_exports": len(exports),
        "shell_imports": len(imports),
        "hooks_required": require_hooks,
        "worker_imports": len(worker_imports) if worker else 0,
        "worker_sha256": worker.sha256 if worker else None,
        "kernel_sha256": kernel.sha256,
        "shell_sha256": shell.sha256,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--shell", required=True, type=Path)
    parser.add_argument("--worker", type=Path)
    parser.add_argument("--require-hooks", action="store_true")
    args = parser.parse_args()
    try:
        report = audit(SelfFile(args.kernel), SelfFile(args.shell), args.require_hooks,
                       SelfFile(args.worker) if args.worker else None)
    except (InvalidABI, OSError, UnicodeError) as error:
        parser.exit(1, f"Syscall ABI verification failed: {error}\n")
    json.dump(report, sys.stdout, indent=2)
    print()


if __name__ == "__main__":
    main()
