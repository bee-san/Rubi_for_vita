#!/usr/bin/env python3
"""A hook request from the debug build's hooks.log (docs/hook-requests.md).

The debug build's game worker rewrites ux0:data/VitaJPOverlay/hooks.log at each game start (game_ocr/hooks.c): the
title, the text source, the game's main module (its name, module NID, segment 0 and APP_VER), every table site it
tried with its verdict (a refused one with the 16 bytes there) and every string-copy import. It holds no game text.
This reads one and says what it shows and what a verified entry for the game needs (core/hook_place.c: the module
NID, the site's bytes, APP_VER).

    python3 tools/hook_request.py SOURCE [--json] [--out FILE] [--bucket DIR] [--rubi DIR]

SOURCE is a hooks.log copied off the Vita; a `tools/install_ftp.sh --collect` folder or its zip (log.txt's hook
readings of the game are counted too: numbers, never text); the text of a hook request issue (the form's hooks.log
block), as in `gh issue view N -R bee-san/Rubi_for_vita --json body -q .body | python3 tools/hook_request.py -`; or -
for standard input.

In a Rubi checkout (--rubi, by default the one this file is in) it also compares the log with that tree's tables
(core/hook_place.c's verified entries, core/hook_luna_table.inc's Luna sites). --bucket DIR compares it with the hook
farm's data bucket (vita/<ID> <title>/hook/rubi-hook.json, read as tools/hook_farm.py reads it): did the farm prove
the build this console runs?

--json gives the same as data, its hex as the log writes it ("0x1EE45F02") and APP_VER as "01.00" (the farm's
rubi-hook.json writes the same values, its hex mostly in lower case; compare them as numbers). --out FILE writes the
report (Markdown, or JSON with --json) to FILE and prints a one-line summary instead (what install_ftp.sh --collect
does: hook-request.md). Exit 0, or 2 when SOURCE holds no hooks.log or cannot be read.
"""
import argparse
import json
import re
import sys
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEVICE_PATH = "ux0:data/VitaJPOverlay/hooks.log"  # game_ocr/hooks.c and game_ocr/psp_probe.c write it
COLLECT_DIR = "ux0/data/VitaJPOverlay"  # its folder in a --collect folder (collect_vita.py keeps device paths)
DOC = "https://github.com/bee-san/Rubi_for_vita/blob/main/docs/hook-requests.md"
FORM = "https://github.com/bee-san/Rubi_for_vita/issues/new?template=hook-request.yml"
MODES = {0: "ocr", 1: "auto", 2: "hooks"}  # VJO_TEXT_SOURCE_* (include/vjo_api.h)
LUNA_SEG0 = 0x80004000  # core/hook_luna.h VJO_LUNA_SEG0: a Luna address's segment-0 offset is address - this
# The title ID prefixes of Japanese PS Vita releases: the farm works on Japanese releases of visual novels, and an entry
# is tried only in the title ID it was made for (core/hook_place.c vjo_place_verified)
JAPANESE = ("PCSG", "PCSH", "VLJM", "VLJS")
SITE_STREAMS = 8  # include/vjo_game_ocr.h VJO_HOOK_SITE_STREAMS: streams 1-8 are table sites, 9-20 string-copy callers
# VJO_PLACE_* (core/hook_place.h, vjo_place_reason), as a player or a maintainer reads them
REASONS = {
    "range": "outside the game's segment 0",
    "align": "misaligned for its instruction set",
    "entry": "not a function's entry that saves LR (the placement check)",
    "window": "an instruction taiHEN would move is not on the placement check's allow-list",
    "signature": "the entry's module NID or bytes differ: another build of the game",
    "unported": "it needs code Rubi has not ported",
    "version": "the entry is for another game version (APP_VER)",
    "full": "all 8 site slots were taken",
    "tai": "taiHEN refused the hook",
}
# game_ocr/hooks.c call_imports: (library NID, function NID) -> name
IMPORTS = {(0xBE43BB07, 0x7205BFDB): "memcpy", (0xBE43BB07, 0x85B924B7): "strcpy",
           (0xBE43BB07, 0x9F87712D): "strncpy", (0xBE43BB07, 0xAF5C218D): "memmove",
           (0xCAE9ACE6, 0x14E9DBD7): "sceClibMemcpy", (0xCAE9ACE6, 0xC458D60A): "sceClibStrncpy",
           (0xCAE9ACE6, 0x2CDFCD1C): "sceClibStrlcpy", (0xCAE9ACE6, 0xAC595E68): "sceClibStrnlen"}

# The lines vjo_hooks_start writes (game_ocr/hooks.c DLOG), in their order. A site's line has no line break of its
# own: its outcome follows on the same line, except after taiHEN's refusal (" tai CODE"), whose " refused: tai" is
# the next line; a refused site's bytes are the line after its refusal (" bytes " and nothing past segment 0).
HEADER = re.compile(r"^(?P<title>[^:\s]{0,9}): text source (?P<mode>\d+)(?P<sites_off>, sites off)?$")
NOTHING = re.compile(r"^nothing: (?P<why>.+)$")
PSP_TEXT = re.compile(r"^the PSP emulator's process: no hooks; PSP text (?P<psp_text>.+)$")  # PR #318's form of it
SITES_OFF = "sites off (hook_sites = off): no table site"
MODULE = re.compile(r"^main module (?P<name>.*) NID (?P<nid>[0-9A-F]{8}) seg0 (?P<base>[0-9A-F]{1,8})\+"
                    r"(?P<size>[0-9A-F]{1,8}) APP_VER (?P<app_ver>[0-9A-F]{4}); (?P<verified>\d+) verified, "
                    r"(?P<luna>\d+) Luna sites$")
VERIFIED = re.compile(r"^verified \+(?P<off>[0-9A-F]{1,8}) (?P<isa>thumb|arm) r(?P<reg>\d+)"
                      r"(?P<luna_text>, Luna's text)?(?P<tail>.*)$")
LUNA = re.compile(r"^luna (?P<addr>[0-9A-F]{8}) r(?P<reg>\d+)(?: \+(?P<off>[0-9A-F]{1,8}))?(?P<tail>.*)$")
TAIL = re.compile(r"^(?: hooked, stream (?P<stream>\d+)| (?P<skipped>skipped): the address is hooked already"
                  r"| refused: (?P<reason>[a-z?]+)| tai (?P<tai>[0-9A-F]{8}))$")
REFUSED = re.compile(r"^ refused: (?P<reason>[a-z?]+)$")
BYTES = re.compile(r"^ bytes ?(?P<hex>(?:[0-9A-F]{2})*) *$")
IMPORT = re.compile(r"^import (?P<lib>[0-9A-F]{8})/(?P<nid>[0-9A-F]{8}): (?P<state>hooked|not hooked)$")
TOTALS = re.compile(r"^sites hooked (?P<hooked>\d+), refused (?P<refused>\d+); "
                    r"CALL imports hooked (?P<calls>\d+) of (?P<imports>\d+)$")
TOTALS_OFF = re.compile(r"^sites off; CALL imports hooked (?P<calls>\d+) of (?P<imports>\d+)$")
PROBE = re.compile(r"^psp probe \d+: ")  # game_ocr/psp_probe.c appends these in the PSP emulator's process
# log.txt (shell/worker.c): a game's start, the worker's hooks once a session, and the debug build's source of a job
GAME_START = re.compile(r"^game (?P<title>\S+) started: ")
HOOKS_IN = re.compile(r"^hooks in (?P<title>\S+): text source (?P<mode>\S+?)(?:, sites off)?, (?P<sites>\d+) table "
                      r"sites and (?P<calls>\d+) string copies hooked, (?P<refused>\d+) sites refused$")
JOB_SOURCE = re.compile(r"^game vocr source: (?P<kind>hook|ocr) (?P<stream>\d+), flags 0x(?P<flags>[0-9A-F]+)")
FENCE = re.compile(r"^ {0,3}(?P<fence>`{3,}|~{3,})")


def app_ver_text(code):
    """0x0100 (game_ocr/hooks.c app_version: "01.00" as BCD) -> "01.00"; 0 -> None (not read)."""
    return f"{code >> 8:02X}.{code & 0xFF:02X}" if code else None


QUOTE = re.compile(r"^ {0,3}> ?")


def extract(text):
    """The hooks.log in text: a fenced block that holds its first line (an issue's form puts it in one), else the
    text from that line on (in Markdown, up to the next heading); in a reply that quotes it (every line `> `), the
    quoted text. (log, strict), log None when there is none; strict when the text is the log itself, so a line the
    grammar does not know is reported."""
    lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    found = unquoted(lines)
    if found[0] is not None or not any(QUOTE.match(x) for x in lines):
        return found
    while any(QUOTE.match(x) for x in lines):  # a quote in a quote too
        lines = [QUOTE.sub("", x, count=1) for x in lines]
        found = unquoted(lines)
        if found[0] is not None:
            return found[0], False
    return None, False


def unquoted(lines):
    """extract()'s reading of lines as they are: (log, strict) or (None, False)."""
    block, fence = None, None
    for line in lines:
        m = FENCE.match(line)
        if fence is None:
            if m:
                fence, block = m.group("fence"), []
            continue
        if m and m.group("fence")[0] == fence[0] and len(m.group("fence")) >= len(fence) and \
                not line.strip()[len(m.group("fence")):].strip():
            if any(HEADER.match(x) for x in block):
                return "\n".join(block), True
            fence, block = None, None
            continue
        block.append(line)
    for i, line in enumerate(lines):
        if HEADER.match(line):
            markdown = any(x.startswith("### ") for x in lines)
            end = next((j for j in range(i + 1, len(lines)) if lines[j].startswith("#")), len(lines)) if markdown \
                else len(lines)
            return "\n".join(lines[i:end]), not markdown and not any(x.strip() for x in lines[:i])
    return None, False


def parse(text, strict=True):
    """hooks.log's lines -> a dict (the log's own numbers; hex codes as ints), None when text has no first line.
    strict: a line before the first or one the grammar does not know is listed in "unknown"."""
    lines = [x.rstrip("\r") for x in text.split("\n")]
    start = next((i for i, x in enumerate(lines) if HEADER.match(x)), None)
    if start is None:
        return None
    m = HEADER.match(lines[start])
    log = {"title": m.group("title") or None, "text_source": int(m.group("mode")),
           "sites_off": bool(m.group("sites_off")), "nothing": None, "psp_text": None, "module": None, "sites": [],
           "imports": [], "totals": None, "psp_probe": [], "warnings": [],
           "unknown": [x for x in lines[:start] if x.strip()] if strict else []}
    site = None  # the last site: a later " refused: tai" or " bytes" line is its
    for line in lines[start + 1:]:
        if not line.strip():
            continue
        if HEADER.match(line):
            log["warnings"].append("the text holds a second hooks.log: only the first is read")
            break
        if PROBE.match(line):
            log["psp_probe"].append(line)
            continue
        m = VERIFIED.match(line) or LUNA.match(line)
        if m:
            g = m.groupdict()
            site = {"kind": "verified" if line.startswith("verified") else "luna",
                    "addr": int(g["addr"], 16) if g.get("addr") else None,
                    "off": int(g["off"], 16) if g.get("off") else None, "reg": int(g["reg"]),
                    "isa": g.get("isa"), "luna_text": bool(g.get("luna_text")), "outcome": None, "reason": None,
                    "stream": None, "tai": None, "bytes": None}
            log["sites"].append(site)
            tail = TAIL.match(g["tail"]) if g["tail"] else None
            if g["tail"] and not tail and strict:
                log["unknown"].append(line)
            elif tail:
                t = tail.groupdict()
                if t["stream"]:
                    site.update(outcome="hooked", stream=int(t["stream"]))
                elif t["skipped"]:
                    site["outcome"] = "skipped"
                elif t["reason"]:
                    site.update(outcome="refused", reason=t["reason"])
                else:
                    site["tai"] = int(t["tai"], 16)
            continue
        m = REFUSED.match(line)
        if m and site is not None and site["outcome"] is None:
            site.update(outcome="refused", reason=m.group("reason"))
            continue
        m = BYTES.match(line)
        if m and site is not None and site["outcome"] == "refused" and site["bytes"] is None:
            site["bytes"] = m.group("hex")
            continue
        m = MODULE.match(line)
        if m:
            g = m.groupdict()
            log["module"] = {"name": g["name"], "nid": int(g["nid"], 16), "seg0": int(g["base"], 16),
                             "size": int(g["size"], 16), "app_ver": int(g["app_ver"], 16),
                             "verified": int(g["verified"]), "luna": int(g["luna"])}
            continue
        m = IMPORT.match(line)
        if m:
            lib, nid = int(m.group("lib"), 16), int(m.group("nid"), 16)
            log["imports"].append({"lib": lib, "nid": nid, "name": IMPORTS.get((lib, nid)),
                                   "hooked": m.group("state") == "hooked"})
            continue
        m = TOTALS.match(line) or TOTALS_OFF.match(line)
        if m:
            log["totals"] = {k: int(v) for k, v in m.groupdict().items()}
            continue
        m = NOTHING.match(line)
        if m:
            log["nothing"] = m.group("why")
            continue
        m = PSP_TEXT.match(line)
        if m:
            log["nothing"], log["psp_text"] = "the PSP emulator's process", m.group("psp_text")
            continue
        if line == SITES_OFF:
            log["sites_off"] = True
            continue
        if strict:
            log["unknown"].append(line)
    check(log)
    return log


def check(log):
    """The log's own counts against its lines: a log that stops early (did the game stop while the worker installed
    its hooks?) or that does not add up."""
    w = log["warnings"]
    for s in log["sites"]:
        if s["outcome"] is None:
            w.append(f"{site_name(s)} has no verdict: the log ends there")
    t = log["totals"]
    if t is None:
        if log["nothing"] is None:
            w.append("no last line (`sites hooked ...` or `sites off; ...`): the log stops before the worker finished "
                     "installing; did the game stop as it started?")
        return
    hooked = sum(s["outcome"] == "hooked" for s in log["sites"])
    refused = sum(s["outcome"] == "refused" for s in log["sites"])
    calls = sum(i["hooked"] for i in log["imports"])
    if "hooked" in t and (hooked, refused) != (t["hooked"], t["refused"]):
        w.append(f"its last line counts {t['hooked']} sites hooked and {t['refused']} refused, its site lines "
                 f"{hooked} and {refused}")
    # a taiHEN without the import hook writes no import line and still counts CALL_IMPORTS (game_ocr/hooks.c
    # install_calls): only import lines that are there are counted against the total
    if calls != t["calls"] or (log["imports"] and len(log["imports"]) != t["imports"]):
        w.append(f"its last line counts {t['calls']} of {t['imports']} imports hooked, its import lines {calls} of "
                 f"{len(log['imports'])}")
    m = log["module"]
    if m and not log["sites_off"] and len(log["sites"]) != m["verified"] + m["luna"]:
        w.append(f"the main module's line names {m['verified']} verified entries and {m['luna']} Luna sites, the log "
                 f"has {len(log['sites'])} site lines")


def site_name(s):
    if s["kind"] == "verified":
        return f"verified entry +0x{s['off']:X} ({s['isa']}, r{s['reg']})"
    where = f" (+0x{s['off']:X})" if s["off"] is not None else ""
    return f"Luna site 0x{s['addr']:08X}{where} (r{s['reg']})"


def site_offset(s):
    """The segment-0 offset the worker tried (a Luna site's as its line gives it, else Luna's space), Thumb bit
    cleared; None when it has none."""
    if s["off"] is not None:
        return s["off"] & ~1
    return (s["addr"] & ~1) - LUNA_SEG0 if s["addr"] is not None and s["addr"] >= LUNA_SEG0 else None


def site_verdict(s):
    if s["outcome"] == "hooked":
        return f"hooked, stream {s['stream']}"
    if s["outcome"] == "skipped":
        return "skipped: its address is hooked already"
    if s["outcome"] == "refused":
        tai = f" (0x{s['tai']:08X})" if s["tai"] is not None else ""
        return f"refused: {s['reason']}{tai}, {REASONS.get(s['reason'], '?')}"
    return "no verdict (the log ends)"


# ---- a Rubi tree's tables and the farm's bucket ----

VERIFIED_ROW = re.compile(
    r'\{"(?P<title>[A-Z]{4}\d{5})",\s*0x(?P<nid>[0-9A-Fa-f]+)u,\s*0x(?P<off>[0-9A-Fa-f]+)u,\s*(?P<thumb>[01]),\s*'
    r'(?P<reg>\d+),\s*VJO_HOOK_ENC_(?P<enc>\w+),\s*(?P<n>\d+),\s*(?P<pad>[^,{}]+),\s*\{(?P<sig>[^{}]*)\},\s*'
    r'(?P<filter>[^,{}]+),\s*(?P<read>[^,{}]+),\s*(?P<app_ver>[^,{}\s]+)\s*\}')
LUNA_ROW = re.compile(r"\{\.addr = 0x(?P<addr>[0-9A-F]+)u,.*?\.reg = (?P<reg>\d+),.*?\.ported = (?P<ported>\d),"
                      r".*?\.version = 0x(?P<version>[0-9A-F]+)\}, /\* (?P<title>[A-Z]{4}\d{5}):\d+ \*/")


def c_int(text):
    text = text.strip().rstrip("uU")
    return int(text, 0) if re.fullmatch(r"0x[0-9A-Fa-f]+|\d+", text) else None


def verified_rows(source):
    """core/hook_place.c's verified[] rows: [{title, nid, off, thumb, reg, encoding, sig, app_ver}]."""
    body = re.sub(r"/\*.*?\*/", "", source, flags=re.S)
    m = re.search(r"VjoPlaceVerified verified\[\][^=]*=\s*\{(?P<rows>.*?)\n\};", body, re.S)
    if not m:
        return []
    rows = []
    for r in VERIFIED_ROW.finditer(m.group("rows")):
        sig = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", r.group("sig")))
        rows.append({"title": r.group("title"), "nid": int(r.group("nid"), 16), "off": int(r.group("off"), 16),
                     "thumb": r.group("thumb") == "1", "reg": int(r.group("reg")), "encoding": r.group("enc"),
                     "sig": sig[:int(r.group("n"))].hex().upper(), "app_ver": c_int(r.group("app_ver")) or 0})
    return rows


def luna_rows(source):
    """core/hook_luna_table.inc's sites: [{title, addr, reg, ported, version}]."""
    return [{"title": r.group("title"), "addr": int(r.group("addr"), 16), "reg": int(r.group("reg")),
             "ported": r.group("ported") == "1", "version": int(r.group("version"), 16)}
            for r in LUNA_ROW.finditer(source)]


def rubi_tables(title, root=ROOT):
    """A Rubi tree's entries for title, or None where there is no tree (the release zip's tools/)."""
    place, luna = Path(root) / "core/hook_place.c", Path(root) / "core/hook_luna_table.inc"
    if not title or not place.is_file() or not luna.is_file():
        return None
    return {"verified": [r for r in verified_rows(place.read_text(encoding="utf-8")) if r["title"] == title],
            "luna": [r for r in luna_rows(luna.read_text(encoding="utf-8")) if r["title"] == title]}


class Unreadable(Exception):
    """A source, or a farm file, that cannot be read: its message says which and why."""


def farm_hook(bucket, title):
    """The farm bucket's hook for title, in tools/hook_farm.py's layout: (folder name, rubi-hook.json or None), or
    False when the bucket has no folder for it. A rubi-hook.json that is not a JSON object raises Unreadable."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import hook_farm  # the repository's (the release zip has none: its tools/ never read a bucket)
    for tid, folder in hook_farm.titles(str(bucket)):
        if tid == title:
            path = Path(folder) / "hook" / "rubi-hook.json"
            try:
                hook = hook_farm.load_json(str(path))
            except (OSError, ValueError) as e:
                raise Unreadable(f"{path}: not a rubi-hook.json ({e})") from None
            if hook is not None and not isinstance(hook, dict):
                raise Unreadable(f"{path}: not a rubi-hook.json (a JSON {type(hook).__name__}, not an object)")
            return Path(folder).name, hook
    return False


def hex_int(value):
    try:
        return int(str(value), 16) if value not in (None, "") else None
    except ValueError:
        return None


# ---- log.txt of a collect ----

SHELL_START = re.compile(r"^Rubi shell started \(kernel API \d+(?P<normal>, the normal build: a short log)?\)$")


def log_readings(text, title):
    """log.txt's last session of title: its `hooks in` line and the debug build's job sources (numbers only); and
    whether log.txt's last game start is another title's or its last boot the normal build's (then hooks.log is from
    an earlier session: the normal build, and OCR only, write none and remove none)."""
    lines = text.replace("\r", "").split("\n")
    games = [m.group("title") for x in lines if (m := GAME_START.match(x))]
    boots = [bool(m.group("normal")) for x in lines if (m := SHELL_START.match(x))]
    starts = [i for i, x in enumerate(lines) if (m := GAME_START.match(x)) and m.group("title") == title]
    out = {"sessions": len(starts), "hooks_in": None, "ocr": 0, "site": 0, "call": 0, "streams": {},
           "last_game": games[-1] if games else None, "last_boot_normal": boots[-1] if boots else None}
    if not starts:
        return out
    end = next((j for j in range(starts[-1] + 1, len(lines)) if GAME_START.match(lines[j])), len(lines))
    for x in lines[starts[-1]:end]:
        if (m := HOOKS_IN.match(x)) and m.group("title") == title:
            out["hooks_in"] = x
        elif m := JOB_SOURCE.match(x):
            if m.group("kind") == "ocr":
                out["ocr"] += 1
            else:
                stream = int(m.group("stream"))
                out["site" if stream <= SITE_STREAMS else "call"] += 1
                out["streams"][stream] = out["streams"].get(stream, 0) + 1
    return out


# ---- the source ----

def read_source(source):
    """(the text that holds hooks.log or None, log.txt's text or None, where it was read)."""
    if source == "-":
        return sys.stdin.read(), None, "standard input"
    p = Path(source)
    if p.is_dir():
        for d in (p / COLLECT_DIR, p):
            if (d / "hooks.log").is_file():
                log = d / "log.txt"
                return ((d / "hooks.log").read_bytes().decode("utf-8", "replace"),
                        log.read_bytes().decode("utf-8", "replace") if log.is_file() else None, str(d / "hooks.log"))
        return None, None, str(p)
    if zipfile.is_zipfile(p):
        try:
            with zipfile.ZipFile(p) as z:
                names = set(z.namelist())
                want = f"{COLLECT_DIR}/hooks.log"
                hooks = sorted(n for n in names if n == want or n.endswith("/" + want))
                if not hooks:
                    return None, None, str(p)
                log = hooks[0][:-len("hooks.log")] + "log.txt"
                return (z.read(hooks[0]).decode("utf-8", "replace"),
                        z.read(log).decode("utf-8", "replace") if log in names else None, f"{p}:{hooks[0]}")
        except (zipfile.BadZipFile, zlib.error, RuntimeError, EOFError, NotImplementedError) as e:
            raise Unreadable(f"{p}: a damaged zip ({e})") from None
    return p.read_bytes().decode("utf-8", "replace"), None, str(p)


# ---- the report ----

def assess(log, tables=None, farm=None, readings=None):
    """(verdict, next steps): what the log says about the game, and what turns it into a verified entry."""
    m, sites = log["module"], log["sites"]
    if log["nothing"]:
        if log["nothing"].startswith("the PSP emulator"):
            return ("a PSP game: Rubi has no native hooks for PSP games yet",
                    ["A request cannot become an entry for a PSP game yet: Rubi has no hook path for PSP games "
                     "(the PSP probe's lines in the log, if any, help that later work)."])
        return (f"no game to hook ({log['nothing']})",
                ["Send the hooks.log of a PS Vita game: a PSP game or a homebrew app has no main module the worker "
                 "hooks."])
    steps = []
    if log["sites_off"]:
        steps.append("hook_sites = off in config.ini: no table site was tried. Delete the line (or set it on), start "
                     "the game again and send the new hooks.log.")
    if m and not m["app_ver"]:
        steps.append("The log has no APP_VER (0000: param.sfo had none, or a debug build made before #367, "
                     "v0.9.8-test.2 and older, which read it only for a game with a Luna site or an entry that names "
                     "its version): give the game's version in the request.")
    if readings and readings["sessions"] == 0 and readings["last_game"]:
        steps.append(f"log.txt has no start of {log['title']}, and its last game start is {readings['last_game']}'s: "
                     "this hooks.log may be from an earlier session (the normal build and OCR only write none).")
    elif readings and readings["last_game"] and readings["last_game"] != log["title"]:
        steps.append(f"log.txt's last game start is {readings['last_game']}'s, not {log['title']}'s: this hooks.log "
                     "is from before that start (a game that is OFF, the normal build and OCR only write none).")
    elif readings and readings["last_boot_normal"]:
        steps.append("log.txt's last boot is the normal build's, which writes no hooks.log: this one is from an "
                     "earlier debug session.")
    hooked = [s for s in sites if s["outcome"] == "hooked"]
    refused_entries = [s for s in sites if s["kind"] == "verified" and s["outcome"] == "refused"]
    if log["sites_off"]:
        verdict = "table sites were off (hook_sites = off): the log cannot say what Rubi has for this game"
    elif any(s["kind"] == "verified" for s in hooked):
        verdict = "Rubi's verified entry is installed in this game"
        steps.append("If the popup does not show the game's text, that is a bug of this entry (an issue saying what "
                     "the popup shows), not a hook request.")
    elif hooked:
        verdict = "a LunaTranslator site is installed (Luna's sites are not verified on a console)"
        steps.append("Say whether the popup shows the dialogue. The farm can prove a site and make it a verified entry "
                     "with Rubi's own text clean-up.")
    elif refused_entries and all(s["reason"] in ("tai", "full") for s in refused_entries):
        reasons = ", ".join(sorted({s["reason"] for s in refused_entries}))
        verdict = (f"Rubi's verified entry matches this build but was not installed ({reasons}: taiHEN refused it, "
                   "or all site slots were taken)")
        steps.append("That is a bug of Rubi, not a hook request: send this log in an issue.")
    elif refused_entries:
        reasons = ", ".join(sorted({s["reason"] for s in refused_entries}))
        verdict = f"Rubi's verified entry is refused here ({reasons}): this console runs another build of the game"
        steps.append("The farm needs this build (the module NID and APP_VER above) to make an entry for it.")
    elif sites:
        reasons = ", ".join(sorted({s["reason"] or "?" for s in sites}))
        verdict = f"LunaTranslator's {len(sites)} site(s) for this game are refused ({reasons})"
        if any(s["bytes"] for s in sites):
            steps.append("The farm can prove a site in Vita3K and make it a verified entry; the bytes this console has "
                         "at each refused site are above.")
        else:
            steps.append("No site is inside this build's segment 0 as Rubi reads Luna's addresses: the farm needs to "
                         "find the site in this build.")
    else:
        verdict = "Rubi has no entry for this game"
        steps.append("The farm needs to find a hook for this game: its title ID, APP_VER and module NID are above. It "
                     "works on Japanese releases of visual novels, a bounded number of attempts each, and a hook must "
                     "pass Rubi's checks, so not every request gets an entry.")
    if log["title"] and not log["title"].startswith(JAPANESE) and not log["sites_off"]:
        steps.append(f"{log['title']} is not a Japanese release (PCSG, PCSH, VLJM, VLJS): the farm works on Japanese "
                     "releases, and an entry is tried only in the title ID it was made for, so an entry from the "
                     "Japanese release would not reach this one.")
    calls = sum(i["hooked"] for i in log["imports"])
    if readings and readings["site"] + readings["call"]:
        verdict += f"; a hook answered {readings['site'] + readings['call']} of the readings log.txt names"
    elif calls:
        verdict += f"; {calls} string-copy imports hooked"
    if tables is not None and m and not log["sites_off"]:
        have, had = (len(tables["verified"]), len(tables["luna"])), (m["verified"], m["luna"])
        if have != had:
            counts = (f"this checkout has {have[0]} verified entries and {have[1]} Luna sites for {log['title']}, the "
                      f"console's build had {had[0]} and {had[1]}")
            if have[0] >= had[0] and have[1] >= had[1]:
                steps.append(f"Try the newest test build first: {counts}.")
            else:
                steps.append(f"The console's build had entries this checkout has not ({counts}): a test build with a "
                             f"pull request's entries, or another branch?")
        for r in tables["verified"]:
            if r["nid"] != m["nid"]:
                steps.append(f"This checkout's entry +0x{r['off']:X} is for module NID 0x{r['nid']:08X}, this "
                             f"console's is 0x{m['nid']:08X}: another build.")
            elif r["app_ver"] and m["app_ver"] and r["app_ver"] != m["app_ver"]:
                steps.append(f"This checkout's entry +0x{r['off']:X} is for APP_VER {app_ver_text(r['app_ver'])}, "
                             f"this console runs {app_ver_text(m['app_ver'])}.")
    if farm is not None and m:
        steps += farm_steps(farm, log)
    return verdict, steps


def farm_steps(farm, log):
    """What the farm's bucket says about this console's build."""
    m = log["module"]
    if farm is False:
        return ["The farm's bucket has no folder for this title: the farm has not finished it."]
    folder, hook = farm
    if not hook:
        return [f"The farm's bucket has `{folder}` but no hook/rubi-hook.json."]
    nid, pairs = hex_int(hook.get("module_nid")), (hook.get("evidence") or {}).get("approved_pairs")
    out = [f"The farm's hook ({hook.get('status')}, {pairs} approved pages): segment 0 + "
           f"{hook.get('segment0_offset')}, r{hook.get('register')}, {hook.get('encoding')}, module NID "
           f"{hook.get('module_nid')}, APP_VER {hook.get('app_ver')}."]
    if nid is None:
        out.append("The farm recorded no module NID: it must dump the game's main module first.")
    elif nid == m["nid"]:
        out.append("The farm proved this console's build (the same module NID): an entry made from it installs here.")
    else:
        out.append(f"The farm proved another build (module NID 0x{nid:08X}, this console's 0x{m['nid']:08X}): an entry "
                   f"made from it would be refused here as signature.")
    off, sig = hex_int(hook.get("segment0_offset")), (hook.get("signature") or "").upper()
    for s in log["sites"]:
        here = site_offset(s)
        if s["bytes"] and sig and off is not None and here == off & ~1:
            n = min(len(s["bytes"]), len(sig))
            same = s["bytes"][:n] == sig[:n]
            out.append(f"At the farm's site this console has {s['bytes']}: "
                       + ("the farm's bytes." if same else f"not the farm's {sig}."))
    return out


def report(log, source, verdict, steps, readings=None):
    """The Markdown report."""
    m = log["module"]
    rows = [("Title ID", log["title"] or "(none)"),
            ("Text source", f"{MODES.get(log['text_source'], '?')} ({log['text_source']})"
                            + (", table sites off" if log["sites_off"] else ""))]
    if m:
        rows += [("Main module", f"{m['name']}, module NID 0x{m['nid']:08X}"),
                 ("Segment 0", f"0x{m['seg0']:08X}, 0x{m['size']:X} bytes"),
                 ("APP_VER", app_ver_text(m["app_ver"]) or "not read (0000)"),
                 ("Entries in the console's build", f"{m['verified']} verified, {m['luna']} Luna sites")]
    elif log["nothing"]:
        rows.append(("Hooks", log["nothing"] + (f"; PSP text {log['psp_text']}" if log["psp_text"] else "")))
    if log["imports"]:
        names = [i["name"] or f"{i['lib']:08X}/{i['nid']:08X}" for i in log["imports"] if i["hooked"]]
        rows.append(("String copies", f"{len(names)} of {len(log['imports'])} imports hooked"
                                      + (f" ({', '.join(names)})" if names else "")))
    if readings and readings["sessions"]:
        rows.append(("log.txt, the game's last session", (f"`{readings['hooks_in']}`" if readings["hooks_in"]
                                                          else "no `hooks in` line")
                     + f"; job sources: {readings['ocr']} OCR, {readings['site']} table site, "
                       f"{readings['call']} string copy"))
    out = [f"# Hook request: {log['title'] or '(no title)'}", "", f"**Verdict:** {verdict}.", "",
           "| | This console (hooks.log) |", "| --- | --- |"]
    out += [f"| {k} | {v} |" for k, v in rows]
    if log["sites"]:
        out += ["", "| Site | Verdict | Bytes there (a refused site's) |", "| --- | --- | --- |"]
        out += [f"| {site_name(s)} | {site_verdict(s)} | {s['bytes'] or ''} |" for s in log["sites"]]
    if steps:
        out += ["", "Next:", ""] + [f"- {s}" for s in steps]
    notes = log["warnings"] + [f"a line this tool does not know: `{u}`" for u in log["unknown"]]
    if notes:
        out += ["", "Check:", ""] + [f"- {n}" for n in notes]
    if log["psp_probe"]:
        out += ["", f"The PSP probe's {len(log['psp_probe'])} lines follow in the log "
                    "(docs/native-hooks.md#psp-probe-debug-build)."]
    out += ["", f"Read from {source} by tools/hook_request.py. How a request becomes a verified entry: {DOC}"]
    return "\n".join(out) + "\n"


def as_json(log, verdict, steps, source, tables, farm, readings):
    """The report as data: the log's numbers in the farm's spellings (hex strings, APP_VER as "01.00")."""
    m = dict(log["module"]) if log["module"] else None
    if m:
        m.update(nid=f"0x{m['nid']:08X}", seg0=f"0x{m['seg0']:08X}", size=f"0x{m['size']:X}",
                 app_ver=app_ver_text(m["app_ver"]))
    sites = []
    for s in log["sites"]:
        d = dict(s, offset=site_offset(s))
        for k in ("addr", "off", "tai", "offset"):
            if d[k] is not None:
                d[k] = f"0x{d[k]:X}"
        sites.append(d)
    return {"tool": "tools/hook_request.py", "source": source, "title": log["title"],
            "text_source": MODES.get(log["text_source"], log["text_source"]), "sites_off": log["sites_off"],
            "nothing": log["nothing"], "module": m, "sites": sites,
            "imports": [dict(i, lib=f"0x{i['lib']:08X}", nid=f"0x{i['nid']:08X}") for i in log["imports"]],
            "totals": log["totals"], "psp_probe_lines": len(log["psp_probe"]), "verdict": verdict, "next": steps,
            "warnings": log["warnings"], "unknown": log["unknown"], "log_txt": readings,
            "rubi_tables": None if tables is None else {
                "verified": [dict(r, nid=f"0x{r['nid']:08X}", off=f"0x{r['off']:X}", app_ver=app_ver_text(r["app_ver"]))
                             for r in tables["verified"]],
                "luna": [dict(r, addr=f"0x{r['addr']:08X}", version=app_ver_text(r["version"]))
                         for r in tables["luna"]]},
            "farm": None if farm is None else False if farm is False else {"folder": farm[0], "hook": farm[1]}}


def summary_line(log, verdict):
    m = log["module"]
    what = [log["title"] or "no title"]
    if m:
        what += [f"APP_VER {app_ver_text(m['app_ver']) or '0000'}", f"main module {m['name']} (NID 0x{m['nid']:08X})"]
    return f"hooks.log: {', '.join(what)}: {verdict}."


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("source", help="hooks.log, a --collect folder or zip, an issue's text, or - (stdin)")
    ap.add_argument("--json", action="store_true", help="the report as JSON")
    ap.add_argument("--out", help="write the report here and print one line")
    ap.add_argument("--bucket", help="the hook farm's data bucket (vita/<ID> <title>/hook/rubi-hook.json)")
    ap.add_argument("--rubi", default=str(ROOT), help="the Rubi checkout whose tables to compare (default: this one)")
    args = ap.parse_args(argv)
    try:
        text, log_txt, source = read_source(args.source)
    except (OSError, Unreadable) as e:
        print(f"hook_request: {e}", file=sys.stderr)
        return 2
    body, strict = extract(text) if text is not None else (None, False)
    log = parse(body, strict) if body is not None else None
    if log is None:
        print(f"hook_request: no hooks.log in {source}: its first line is like 'PCSG00415: text source 1', and only "
              f"the debug build writes one, with Text source on Automatic ({DOC})", file=sys.stderr)
        return 2
    tables = rubi_tables(log["title"], args.rubi) if log["module"] else None
    farm = None
    if args.bucket and log["title"]:
        if not (Path(args.bucket) / "vita").is_dir():
            print(f"hook_request: {args.bucket} is not a farm bucket (it has no vita/)", file=sys.stderr)
            return 2
        try:
            farm = farm_hook(args.bucket, log["title"])
        except ImportError:
            print("hook_request: --bucket needs a Rubi checkout (tools/hook_farm.py)", file=sys.stderr)
            return 2
        except Unreadable as e:
            print(f"hook_request: {e}", file=sys.stderr)
            return 2
    readings = log_readings(log_txt, log["title"]) if log_txt and log["title"] else None
    verdict, steps = assess(log, tables, farm, readings)
    if args.json:
        out = json.dumps(as_json(log, verdict, steps, source, tables, farm, readings), ensure_ascii=False, indent=1)
        out += "\n"
    else:
        out = report(log, source, verdict, steps, readings)
    if args.out:
        try:
            Path(args.out).write_text(out, encoding="utf-8")
        except OSError as e:
            print(f"hook_request: cannot write {args.out}: {e}", file=sys.stderr)
            return 2
        print(f"{summary_line(log, verdict)} Report: {args.out}. To ask for a hook for this game: {DOC}")
    else:
        sys.stdout.write(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
