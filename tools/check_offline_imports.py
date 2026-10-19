#!/usr/bin/env python3
"""Rubi has no network code (AGENTS.md rule 11): no module imports a network library, links network
code, or names a network module to load or resolve at run time.

    check_offline_imports.py [--app APP_ELF] MODULE_ELF [MODULE_ELF ...]

Every module, the kernel plugin, the Shell plugin, the game OCR worker, the Adrenaline bridge and the
Rubi app (--app), is read as its linked ELF (before vita-elf-create) and fails when:

- one of its import stub sections (.vitalink.fstubs.<library>, .vitalink.vstubs.<library>, as
  vita-elf-create packages them) names a network library: SceNet* (SceNet, SceNetCtl, SceNetPs,
  SceNetPsForDriver, ...), SceHttp*, SceSsl*, ScePspnet*, SceNpWebApi*, SceWlan*;
- its symbol table (defined or referenced, global or local) has a name of network, TLS, HTTP or sync
  code: a TLS library's (BearSSL's engine, records, PRFs and X.509, mbedTLS, wolfSSL, OpenSSL,
  GnuTLS, picotls, s2n, rustls), an HTTP client's, Sony's network API's (sceNet*, sceHttp*,
  sceSsl*, and the kernel's ksceNet*, ksceHttp*, ksceSsl*), the old network code's own (vjo_tls_,
  vjo_http_, vjo_net_, vjo_sync_), or a sync's SQLite and zstd. BearSSL's hashes (br_sha*: the
  queue's IDs, the screenshots' names) are not network code;
- one of its allocated sections holds the name of a network module or its file, as a module that
  resolves an export at run time by NID would need (module_get_export_func(..., "SceNetPs", ...),
  taiGetModuleExportFunc, sceKernelLoadStartModule("vs0:sys/external/libnet.suprx", ...)):
  SceNet, SceHttp, SceSsl, ScePspnet, SceNpWebApi, SceWlan, libnet, libhttp, libssl, libnetctl;
- it has no import stub section at all (not a linked module: the .velf or the SELF instead of the
  ELF, or a toolchain that names its stubs otherwise), no symbol table (a stripped module cannot be
  checked) or no entry point in it (module_start; the app's main): exit 2.

The Rubi app is checked as a plugin is, with one exception: VitaSDK's newlib calls three SceNet
functions for a socket descriptor, which the app can never hold, and settings/no_net.c defines them
to fail (docs/shell-slim.md). Exactly those three, sceNetRecv, sceNetSend and sceNetSocketClose, may
be in the app's symbol table, and only as functions defined in its own code, never as an import's
stub (a stub is defined in a .vitalink section) and never undefined.

Every module given is checked and reported. Exit status 0 = every module passes, 1 = one has
network imports or code, 2 = one cannot be read or checked (or a usage error).
"""
import struct
import sys

STUBS = (".vitalink.fstubs.", ".vitalink.vstubs.")
NETWORK_LIBRARIES = ("SceNet", "SceHttp", "SceSsl", "ScePspnet", "SceNpWebApi", "SceWlan")
# Symbols of network code: name prefixes, and substrings for Rust's mangled names.
NETWORK_SYMBOL_PREFIXES = (
    "br_ssl_", "br_sslio_", "br_sslrec_", "br_tls", "br_x509_",  # BearSSL's TLS engine, records, PRFs, X.509
    "mbedtls_", "wolfSSL_", "SSL_", "X509_", "gnutls_", "ptls_", "s2n_",  # other TLS libraries
    "curl_",                                                     # an HTTP client
    "sceNet", "sceHttp", "sceSsl",                               # Sony's network, HTTP and SSL API
    "ksceNet", "ksceHttp", "ksceSsl",                            # and the kernel's
    "vjo_tls_", "vjo_http_", "vjo_net_", "vjo_sync_",            # the old network code's own
    "sqlite3", "ZSTD_",                                          # a sync's collection and compression
)
NETWORK_SYMBOL_PARTS = ("rustls", "reqwest", "tokio")
# The names a module would hold to load a network module or resolve its exports at run time.
NETWORK_MODULE_NAMES = (b"SceNet", b"SceHttp", b"SceSsl", b"ScePspnet", b"SceNpWebApi", b"SceWlan",
                        b"libnet", b"libhttp", b"libssl", b"libnetctl")
# settings/no_net.c: newlib's three socket calls, defined in the app to fail (docs/shell-slim.md).
APP_DEFINED = ("sceNetRecv", "sceNetSend", "sceNetSocketClose")
SHT_SYMTAB, SHT_NOBITS, SHF_ALLOC, STT_FUNC = 2, 8, 2, 2
ENTRY = {"plugin": "module_start", "app": "main"}


def _string(table, offset, what):
    """The NUL-terminated string at offset in table; ValueError when it is not one."""
    if offset >= len(table):
        raise ValueError(f"bad {what} offset {offset}")
    end = table.find(b"\0", offset)
    if end < 0:
        raise ValueError(f"unterminated {what}")
    return table[offset:end].decode("ascii", "replace")


def sections(data):
    """The (name, type, flags, offset, size, link) of each section of an ELF32 little-endian file."""
    if len(data) < 52:  # the ELF32 header
        raise ValueError(f"{len(data)} B: too short for an ELF32 header")
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise ValueError("not a 32-bit little-endian ELF")
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    if shentsize < 40 or shnum == 0 or shstrndx >= shnum or shoff + shnum * shentsize > len(data):
        raise ValueError("bad section header table")
    headers = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]
    stroff, strsize = headers[shstrndx][4:6]
    if stroff + strsize > len(data):
        raise ValueError("bad section name table")
    strtab = data[stroff:stroff + strsize]
    out = []
    for h in headers:
        name = _string(strtab, h[0], "section name") if h[0] or strtab[:1] == b"\0" else ""
        if h[1] != SHT_NOBITS and h[4] + h[5] > len(data):
            raise ValueError(f"section {name!r} beyond the file")
        out.append((name, h[1], h[2], h[4], h[5], h[6]))
    return out


def section_names(data):
    return [s[0] for s in sections(data)]


def imported_libraries(data):
    return sorted({n[len(p):] for n in section_names(data) for p in STUBS if n.startswith(p)})


def symbols(data):
    """Every symbol of the symbol table (.symtab) as (name, section index, type), or None when there is
    none. Bad offsets, sizes, links and names are a ValueError."""
    secs = sections(data)
    tables = [s for s in secs if s[1] == SHT_SYMTAB]
    if not tables:
        return None
    out = []
    for _, _, _, offset, size, link in tables:
        if link >= len(secs) or offset + size > len(data) or size % 16:
            raise ValueError("bad symbol table")
        _, kind, _, stroff, strsize, _ = secs[link]
        if kind != 3 or stroff + strsize > len(data):  # SHT_STRTAB
            raise ValueError("bad symbol string table")
        strtab = data[stroff:stroff + strsize]
        for at in range(offset, offset + size, 16):  # Elf32_Sym: 16 bytes
            name, _, _, info, _, shndx = struct.unpack_from("<IIIBBH", data, at)
            if name:
                out.append((_string(strtab, name, "symbol name"), shndx, info & 15))
    return out


def symbol_names(data):
    found = symbols(data)
    return None if found is None else sorted({n for n, _, _ in found if n})


def network_symbols(names):
    return sorted(n for n in set(names)
                  if n.startswith(NETWORK_SYMBOL_PREFIXES) or any(p in n for p in NETWORK_SYMBOL_PARTS))


def network_module_names(data):
    """The network module names and files the allocated sections' bytes hold (F1)."""
    found = set()
    for name, kind, flags, offset, size, _ in sections(data):
        if not flags & SHF_ALLOC or kind == SHT_NOBITS or name.startswith(STUBS):
            continue
        blob = data[offset:offset + size]
        found.update(m.decode() for m in NETWORK_MODULE_NAMES if m in blob)
    return sorted(found)


def check(path, data, role):
    """0, or 1 with the module's problems printed; ValueError when it cannot be checked."""
    secs = sections(data)
    libs = imported_libraries(data)
    if not libs:
        raise ValueError("no import stub section (not a linked module: the .velf or the SELF?)")
    found = symbols(data)
    if found is None:
        raise ValueError("no symbol table (a stripped module cannot be checked)")
    names = {n for n, _, _ in found}
    if ENTRY[role] not in names:
        raise ValueError(f"no {ENTRY[role]} in its symbol table (not a {role}, or an empty symbol table)")
    problems = []
    network = [lib for lib in libs if lib.startswith(NETWORK_LIBRARIES)]
    if network:
        problems.append(f"imports {', '.join(network)}")
    code = network_symbols(names)
    if role == "app":
        allowed = [n for n in code if n in APP_DEFINED]
        code = [n for n in code if n not in APP_DEFINED]
        for name in allowed:  # defined functions of its own code only
            where = [(shndx, kind) for n, shndx, kind in found if n == name]
            bad = [w for w in where if not (0 < w[0] < len(secs)) or w[1] != STT_FUNC
                   or secs[w[0]][0].startswith(".vitalink")]
            if bad:
                problems.append(f"has {name} other than as a function of its own code (an import's stub or "
                                "an undefined reference)")
    if code:
        shown = ", ".join(code[:8]) + (f" and {len(code) - 8} more" if len(code) > 8 else "")
        problems.append(f"has network code ({shown})")
    modules = network_module_names(data)
    if modules:
        problems.append(f"names network modules ({', '.join(modules)})")
    if problems:
        print(f"offline imports: {path} {'; '.join(problems)}: Rubi has no network code")
        return 1
    extra = f", newlib's {len([n for n in names if n in APP_DEFINED])} defined in settings/no_net.c" \
        if role == "app" else ""
    print(f"offline imports: {path}: {len(libs)} import libraries, no network library; "
          f"{len(names)} symbols, no network code{extra}; no network module named")
    return 0


def main(argv):
    modules = []
    i = 0
    while i < len(argv):
        if argv[i] == "--app":
            if i + 1 >= len(argv):
                print(__doc__.strip(), file=sys.stderr)
                return 2
            modules.append((argv[i + 1], "app"))
            i += 2
        else:
            modules.append((argv[i], "plugin"))
            i += 1
    if not modules:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    status = 0
    for path, role in modules:  # every one, whatever an earlier one gave
        try:
            with open(path, "rb") as f:
                data = f.read()
            status = max(status, check(path, data, role))
        except (OSError, ValueError, struct.error) as e:  # struct.error: a header cut short
            print(f"offline imports: {path}: {e}", file=sys.stderr)
            status = 2
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
