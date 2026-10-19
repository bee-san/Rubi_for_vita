#!/usr/bin/env python3
"""The plugins are offline: no network library, no network code. The Rubi app's network is
only what its Anki sync allows (AGENTS.md rule 11, #220).

    check_offline_imports.py [--app APP_ELF] PLUGIN_ELF [PLUGIN_ELF ...]

A plugin (the kernel plugin, the Shell plugin, the game OCR worker) fails when:

- one of its import stub sections (.vitalink.fstubs.<library> and .vitalink.vstubs.<library>, as
  vita-elf-create packages them) names a network library: SceNet* (SceNet, SceNetCtl, SceNetPs,
  ...), SceHttp* or SceSsl*;
- its symbol table (defined or referenced, global or local) has a name of network, TLS, HTTP or
  sync code: a TLS library's (BearSSL's engine and X.509, mbedTLS, wolfSSL, OpenSSL, rustls),
  an HTTP client's, Sony's network API's, the app's own transport and sync code (vjo_tls_,
  vjo_http_, vjo_net_, vjo_sync_), or the sync's SQLite and zstd. BearSSL's hashes (br_sha*,
  the queue's IDs and the screenshots' names) are not network code;
- it has no symbol table: a stripped module cannot be checked.

The app (--app) is checked by its imports: no network library but APP_NETWORK's, which its sync
uses; never SceHttp or SceSsl (the sync's TLS is the app's own, with pinned roots).

Exit status 0 = every module passes, 1 = one does not, 2 = an unreadable ELF or a usage error.
"""
import struct
import sys

STUBS = (".vitalink.fstubs.", ".vitalink.vstubs.")
NETWORK = ("SceNet", "SceHttp", "SceSsl")
# The network libraries the Rubi app may import (AGENTS.md rule 11): none until its Anki sync
# lands; then exactly SceNet and SceNetCtl.
APP_NETWORK = ()
# Symbols of network code: name prefixes, and substrings for Rust's mangled names.
NETWORK_SYMBOL_PREFIXES = (
    "br_ssl_", "br_sslio_", "br_x509_",          # BearSSL's TLS engine and certificate checks
    "mbedtls_", "wolfSSL_", "SSL_", "X509_",     # other TLS libraries
    "curl_",                                     # an HTTP client
    "sceNet", "sceHttp", "sceSsl",               # Sony's network, HTTP and SSL API
    "vjo_tls_", "vjo_http_", "vjo_net_", "vjo_sync_",  # the app's transport and sync
    "sqlite3", "ZSTD_",                          # the sync's collection and compression
)
NETWORK_SYMBOL_PARTS = ("rustls", "reqwest", "tokio")
SHT_SYMTAB = 2


def sections(data):
    """The (name, type, offset, size, link) of each section of an ELF32 little-endian file."""
    if len(data) < 52:  # the ELF32 header
        raise ValueError(f"{len(data)} B: too short for an ELF32 header")
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise ValueError("not a 32-bit little-endian ELF")
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    if shentsize < 40 or shstrndx >= shnum or shoff + shnum * shentsize > len(data):
        raise ValueError("bad section header table")
    headers = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]
    stroff, strsize = headers[shstrndx][4:6]
    if stroff + strsize > len(data):
        raise ValueError("bad section name table")
    strtab = data[stroff:stroff + strsize]
    out = []
    for h in headers:
        end = strtab.find(b"\0", h[0])
        name = strtab[h[0]:end if end >= 0 else len(strtab)].decode("ascii", "replace")
        out.append((name, h[1], h[4], h[5], h[6]))
    return out


def section_names(data):
    return [s[0] for s in sections(data)]


def imported_libraries(data):
    return sorted({n[len(p):] for n in section_names(data) for p in STUBS if n.startswith(p)})


def symbol_names(data):
    """Every name in the symbol table (.symtab), or None when there is none."""
    secs = sections(data)
    symtab = [s for s in secs if s[1] == SHT_SYMTAB]
    if not symtab:
        return None
    names = set()
    for _, _, offset, size, link in symtab:
        if link >= len(secs) or offset + size > len(data):
            raise ValueError("bad symbol table")
        _, _, stroff, strsize, _ = secs[link]
        if stroff + strsize > len(data):
            raise ValueError("bad symbol string table")
        strtab = data[stroff:stroff + strsize]
        for at in range(offset, offset + size - size % 16, 16):  # Elf32_Sym: 16 bytes
            name, = struct.unpack_from("<I", data, at)
            if name >= len(strtab):
                raise ValueError("bad symbol name")
            end = strtab.find(b"\0", name)
            if end > name:
                names.add(strtab[name:end].decode("ascii", "replace"))
    return sorted(names)


def network_symbols(names):
    return [n for n in names if n.startswith(NETWORK_SYMBOL_PREFIXES) or any(p in n for p in NETWORK_SYMBOL_PARTS)]


def check_plugin(path, data):
    libs = imported_libraries(data)
    network = [lib for lib in libs if lib.startswith(NETWORK)]
    names = symbol_names(data)
    if names is None:
        raise ValueError("no symbol table (a stripped module cannot be checked)")
    code = network_symbols(names)
    problems = []
    if network:
        problems.append(f"imports {', '.join(network)}")
    if code:
        shown = ", ".join(code[:8]) + (f" and {len(code) - 8} more" if len(code) > 8 else "")
        problems.append(f"has network code ({shown})")
    if problems:
        print(f"offline imports: {path} {'; '.join(problems)}: the plugins are offline")
        return 1
    print(f"offline imports: {path}: {len(libs)} import libraries, no network library; "
          f"{len(names)} symbols, no network code")
    return 0


def check_app(path, data):
    libs = imported_libraries(data)
    network = [lib for lib in libs if lib.startswith(NETWORK)]
    refused = [lib for lib in network if lib not in APP_NETWORK]
    if refused:
        allowed = ", ".join(APP_NETWORK) if APP_NETWORK else "none"
        print(f"offline imports: {path} imports {', '.join(refused)}: the Rubi app may import "
              f"only its Anki sync's network libraries ({allowed})")
        return 1
    print(f"offline imports: {path}: {len(libs)} import libraries, "
          + (f"network: {', '.join(network)} (the Anki sync's)" if network else "no network library"))
    return 0


def main(argv):
    app, plugins = [], []
    i = 0
    while i < len(argv):
        if argv[i] == "--app":
            if i + 1 >= len(argv):
                print(__doc__.strip(), file=sys.stderr)
                return 2
            app.append(argv[i + 1])
            i += 2
        else:
            plugins.append(argv[i])
            i += 1
    if not plugins and not app:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    status = 0
    for path, check in [(p, check_plugin) for p in plugins] + [(p, check_app) for p in app]:
        try:
            data = open(path, "rb").read()
            status |= check(path, data)
        except (OSError, ValueError, struct.error) as e:  # struct.error: a header cut short
            print(f"offline imports: {path}: {e}", file=sys.stderr)
            return 2
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
