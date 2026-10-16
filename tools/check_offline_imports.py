#!/usr/bin/env python3
"""The overlay is offline: no module imports a network library.

    check_offline_imports.py ELF [ELF ...]

Reads each linked ELF's import stub sections (.vitalink.fstubs.<library> and
.vitalink.vstubs.<library>, as vita-elf-create packages them) and fails when one
names a network library: SceNet* (SceNet, SceNetCtl, SceNetPs, ...), SceHttp*
or SceSsl*. Exit status 0 = none does, 1 = one does, 2 = an unreadable ELF.
"""
import struct
import sys

STUBS = (".vitalink.fstubs.", ".vitalink.vstubs.")
NETWORK = ("SceNet", "SceHttp", "SceSsl")


def section_names(data):
    """The section names of an ELF32 little-endian file."""
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise ValueError("not a 32-bit little-endian ELF")
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    if shentsize < 40 or shstrndx >= shnum or shoff + shnum * shentsize > len(data):
        raise ValueError("bad section header table")
    def header(i):
        return struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize)
    stroff, strsize = header(shstrndx)[4:6]
    if stroff + strsize > len(data):
        raise ValueError("bad section name table")
    strtab = data[stroff:stroff + strsize]
    names = []
    for i in range(shnum):
        name = header(i)[0]
        end = strtab.find(b"\0", name)
        names.append(strtab[name:end if end >= 0 else len(strtab)].decode("ascii", "replace"))
    return names


def imported_libraries(data):
    return sorted({n[len(p):] for n in section_names(data) for p in STUBS if n.startswith(p)})


def main(argv):
    if not argv:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    status = 0
    for path in argv:
        try:
            libs = imported_libraries(open(path, "rb").read())
        except (OSError, ValueError, struct.error) as e:  # struct.error: a header cut short
            print(f"offline imports: {path}: {e}", file=sys.stderr)
            return 2
        network = [lib for lib in libs if lib.startswith(NETWORK)]
        if network:
            print(f"offline imports: {path} imports {', '.join(network)}: the overlay is offline")
            status = 1
        else:
            print(f"offline imports: {path}: {len(libs)} import libraries, no network library")
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
