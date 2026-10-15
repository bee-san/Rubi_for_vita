#!/usr/bin/env python3
"""Upload .vjdict files through VitaShell FTP and optionally enable local lookup.

Uses versioned filenames: a failed transfer cannot replace an active dictionary.
Run while games are closed. Existing config is backed up before --enable.
"""
import argparse
import ftplib
import hashlib
import io
from pathlib import Path
import re
import secrets
import sys
import time

from convert_dictionary import HEADER, MAGIC, MAGIC_V2, RECORD, PAGE_SIZE

DATA_DIR = "/ux0:/data/VitaJPOverlay"
DICT_DIR = DATA_DIR + "/dictionaries"
# The API keys of jpdb.io and jiten.moe, the removed online dictionaries (api_key: jpdb's
# older name): --enable drops every line that starts with one, as the overlay ignores them
# (core/config.c removed_secret; no setting's name starts with one of them).
REMOVED_SECRETS = ("jpdb_api_key", "jiten_api_key", "api_key")


def inspect(path):
    with path.open("rb") as f:
        raw = f.read(HEADER.size)
        if len(raw) != HEADER.size:
            raise ValueError(f"{path}: incomplete dictionary header")
        magic, version, header, count, stride, index, data, size, r1, r2 = HEADER.unpack(raw)
        common_valid = (header == HEADER.size and stride == RECORD.size and count > 0
                        and data <= size and size == path.stat().st_size and size <= 0x7FFFFFFFFFFFFFFF)
        v1_valid = (version == 1 and magic == MAGIC and index == HEADER.size
                    and data == header + count * stride and r1 == 0 and r2 == 0)
        v2_valid = (version == 2 and magic == MAGIC_V2 and index == PAGE_SIZE
                    and 0 < r1 <= 0xFFFFFFFF and not (r1 & (r1 - 1))
                    and r1 <= r2 <= 0xFFFFFFFF and data == PAGE_SIZE * (1 + r2))
        if not common_valid or not (v1_valid or v2_valid):
            raise ValueError(f"{path}: incompatible or incomplete .vjdict file")
        sha = hashlib.sha256(raw)
        for block in iter(lambda: f.read(1024 * 1024), b""):
            sha.update(block)
    stem = re.sub(r"[^A-Za-z0-9_-]", "_", path.stem)[:28] or "dictionary"
    return f"{stem}-{sha.hexdigest()[:12]}.vjdict"


def config_with_local(text, names):
    if not names or len(names) > 8:
        raise ValueError("select between 1 and 8 dictionaries")
    values = {"dictionary": "local", "local_dictionary_dir": "ux0:data/VitaJPOverlay/dictionaries",
              "local_dictionaries": ",".join(names)}
    lines, used = [], set()
    for line in text.splitlines():
        if line.lstrip().lower().startswith(REMOVED_SECRETS):
            continue  # the removed online dictionaries' API keys (core/config.c removed_secret)
        match = re.match(r"\s*([a-zA-Z_]+)\s*=", line)
        key = match.group(1).lower() if match else None
        if key in values:
            # Collapse duplicates so the last setting cannot undo the change.
            if key in used:
                continue
            line = f"{key} = {values[key]}"
            used.add(key)
        lines.append(line)
    lines.extend(f"{key} = {value}" for key, value in values.items() if key not in used)
    return "\n".join(lines) + "\n"


def without_secrets(raw):
    """The config file's bytes without the removed online dictionaries' API key lines (a BOM and every
    other byte kept): the backup --enable leaves on the Vita holds no key either."""
    bom = b"\xef\xbb\xbf" if raw.startswith(b"\xef\xbb\xbf") else b""
    kept = [line for line in raw[len(bom):].split(b"\n")
            if not line.decode("utf-8", "replace").lstrip().lower().startswith(REMOVED_SECRETS)]
    return bom + b"\n".join(kept)


def mkdirs(ftp):
    for directory in ("/ux0:/data", DATA_DIR, DICT_DIR):
        try:
            ftp.mkd(directory)
        except ftplib.error_perm:
            # Confirm it is an existing directory, not an ignored permission error.
            ftp.cwd(directory)


def transfer(ftp, local, remote):
    partial = remote + ".part"
    with local.open("rb") as stream:
        ftp.storbinary("STOR " + partial, stream, blocksize=65536)
    # A disconnect leaves only .part, never a configured active file.
    ftp.rename(partial, remote)


def enable(ftp, names):
    old = io.BytesIO()
    # Refuse to overwrite an unreadable config (including a missing config).
    # The overlay's installer creates it (install-vita.sh, tools/install_ftp.sh), and so does any
    # save in the Overlay app; the Shell plugin never writes it.
    ftp.retrbinary("RETR " + DATA_DIR + "/config.ini", old.write)
    raw = old.getvalue()
    text = raw.decode("utf-8-sig")
    updated = config_with_local(text, names).encode()
    backup = DATA_DIR + "/config.ini.before-local-" + str(time.time_ns())
    ftp.storbinary("STOR " + backup, io.BytesIO(without_secrets(raw)))
    ftp.storbinary("STOR " + DATA_DIR + "/config.ini.local.tmp", io.BytesIO(updated))
    # VitaShell rename can reject an existing destination. Keep the old file
    # under a second name, then restore it if the replacement fails.
    previous = backup + ".active"
    ftp.rename(DATA_DIR + "/config.ini", previous)
    try:
        ftp.rename(DATA_DIR + "/config.ini.local.tmp", DATA_DIR + "/config.ini")
    except ftplib.all_errors:
        ftp.rename(previous, DATA_DIR + "/config.ini")
        raise
    # The replaced file itself: the backup above has its settings (without a removed API key).
    try:
        ftp.delete(previous)
    except ftplib.error_reply:
        pass  # VitaShell confirms DELE with a code ftplib does not expect (as install_ftp.sh notes)
    except ftplib.all_errors:
        print("Could not delete " + previous + ": delete it by hand (it can hold an old API key).")
    print("Enabled local dictionaries. Previous settings: " + backup)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("vita", help="Vita IP address (VitaShell FTP)")
    parser.add_argument("files", nargs="+", type=Path, help="converted .vjdict files, in priority order")
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument("--enable", action="store_true", help="select these files in the existing config.ini")
    args = parser.parse_args()
    try:
        if len(args.files) > 8:
            raise ValueError("install at most 8 files at a time; combine more ZIPs with the converter")
        inspected = [inspect(path) for path in args.files]
        suffix = "-" + secrets.token_hex(4) + ".vjdict"
        names = [name[:-7] + suffix for name in inspected]
        if len(names) != len(set(names)):
            raise ValueError("the same dictionary was selected more than once")
        with ftplib.FTP() as ftp:
            ftp.connect(args.vita, args.port, timeout=30)
            ftp.login()
            mkdirs(ftp)
            for path, name in zip(args.files, names):
                transfer(ftp, path, DICT_DIR + "/" + name)
                print("Installed " + name)
            if args.enable:
                enable(ftp, names)
            else:
                print("Set dictionary = local and local_dictionaries = " + ",".join(names))
    except (OSError, ValueError, *ftplib.all_errors) as exc:
        parser.exit(1, f"Install failed: {exc}\nIf config.ini is missing, run the overlay's installer (install-vita.sh) or "
                        "change any setting in the Overlay app to create it, then retry.\n")
    print("Close VitaShell and reopen the overlay to load the dictionaries.")


if __name__ == "__main__":
    main()
