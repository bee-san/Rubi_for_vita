#!/usr/bin/env python3
"""The overlay's lines in a taiHEN configuration (config.txt), edited the way taiHEN reads it.

The PC installer's copy of core/taicfg.c, the Rubi app's editor (docs/one-vpk-install.md, "The
config.txt editor"): both write the same bytes (tools/tests/test_tai_config.py and
host/tests/test_taicfg.c run them on tools/tests/fixtures/taicfg/). It works on bytes with taiHEN's
own rules (taiHEN 309b380 builds DaveeFTW/taihen-parser 6d817fd): a line ends at CR or LF only, only
the six C spaces are trimmed, a raw line of 256 bytes or more and a NUL stop taiHEN. Its result is
checked with a copy of that parser (taihen_parse): every other plugin loads for exactly the titles
and in the order it did, the kernel plugin once for KERNEL, the Shell once for main, and the OCR
worker as the mode says.

--worker kernel (VJO_KERNEL_LOADS_WORKER): the kernel plugin loads the game OCR worker into every
game turned ON (#138, Agent A), so no worker line is written and the ones older installs left are
removed. --worker lines: taiHEN loads it from one *TITLEID section per game (the rules before):
the --title games and the games registered before (here or in a --keep-titles-from
configuration), never *ALL.

    patch_tai_config.py SOURCE OUTPUT --worker kernel|lines [--title TITLEID]... [--uninstall]
                        [--keep-titles-from OTHER]... [--kernel-plugin ur0:tai/OTHER.skprx]...

Exit 0: OUTPUT written (a summary on stdout). 3: lines mode without a game, or a --title the worker
cannot serve. 4: the configuration is refused (why on stderr); OUTPUT is not written.
"""
import argparse
import sys
from pathlib import Path

KERNEL = "ur0:tai/VitaJPOverlay_Kernel.skprx"
SHELL = "ur0:tai/VitaJPOverlay_Shell.suprx"
OCR = "ur0:tai/VitaJPOverlay_OCR.suprx"
TEXT = "ur0:tai/VitaJPOverlay_Text.suprx"
DISABLED = "# Mainline OCR: "
NOTE = "# VitaJPOverlay (added by its installer)"
# The line the Rubi app wrote before a section it added for an Adrenaline bubble (#124).
APP_NOTE = "# VitaJPOverlay: the OCR worker in this Adrenaline bubble's PSP game (Overlay app)"
LINE_MAX = 256           # taihen-parser lexer.h CONFIG_MAX_LINE_LENGTH
TEXT_MAX = 64 * 1024     # core/taicfg.h VJO_TAI_TEXT_MAX
SPACE = b" \t\n\v\f\r"   # the lexer's isspace (lexer.c:24-35, NO_CTYPE as taiHEN builds it)
GAMES_MAX = 512
INSTALL, UNINSTALL = "install", "uninstall"
WORKER_KERNEL, WORKER_LINES = "kernel", "lines"
NO_TITLES = ("No game is registered for the OCR worker (VitaJPOverlay_OCR.suprx), and it is never "
             "registered under *ALL. Name each game with --title TITLEID, for example "
             "--title PCSG00415 (CLANNAD); for Adrenaline's PSP games see docs/guide.md 'Install' (PSPEMUCFW, "
             "each bubble's ID).")

O_NONE, O_KERNEL, O_SHELL, O_WORKER, O_TEXT = range(5)
_NAMES = {O_KERNEL: b"vitajpoverlay_kernel.skprx", O_SHELL: b"vitajpoverlay_shell.suprx",
          O_WORKER: b"vitajpoverlay_ocr.suprx", O_TEXT: b"vitajpoverlay_text.suprx"}
L_COMMENT, L_SECTION, L_PATH = range(3)
KEEP, COMMENT, REMOVE = range(3)


class TitleError(ValueError):
    """A --title the worker cannot serve, or lines mode with no game to register it for."""


class Refused(ValueError):
    """The configuration cannot be edited safely (core/taicfg.h VJO_TAI_E_*): rc, line, why."""

    def __init__(self, rc, line, why):
        super().__init__(why)
        self.rc, self.line, self.why = rc, line, why


E_ARGS, E_TOO_LARGE, E_NUL, E_LONG_LINE, E_EMPTY, E_NO_SECTION, E_TITLE, E_SPACE, E_CHECK = range(-1, -10, -1)


# ---------------- taiHEN's parser (taihen-parser 6d817fd), for the check ----------------

def _tokens(text):
    """lexer.c's tokens for text (bytes; read as a C string): (kind, value) for each, then
    ('end', None) or ('error', None) for a raw line of 256 bytes or more."""
    end = text.find(b"\0")
    if end >= 0:
        text = text[:end]
    pos, n = 0, len(text)
    while True:
        if pos >= n:
            yield "end", None
            return
        line_end = pos
        while line_end < n and text[line_end] not in (13, 10):
            line_end += 1
        if line_end - pos >= LINE_MAX:
            yield "error", None
            return
        line = text[pos:line_end]
        pos = line_end + 1
        line = line.lstrip(SPACE)
        if not line or line[:1] == b"#":
            yield "comment", None
            continue
        line = line[:1] + line[1:].rstrip(SPACE)
        if line[:1] == b"*":
            yield "section", None
            rest = line[1:].lstrip(SPACE)
            if rest[:1] == b"!":
                yield "halt", None
                rest = rest[1:].lstrip(SPACE)
            yield "name", rest
        else:
            yield "path", line


def taihen_parse(text, section):
    """parser.c taihen_config_parse: the plugin paths taiHEN loads for section (bytes), in order."""
    out, halt, record = [], False, False
    for kind, value in _tokens(text):
        if kind == "halt":
            halt = True
        elif kind == "name":
            record = (value == b"ALL" and section != b"KERNEL") or value == section
        elif kind == "section":
            if record and halt:
                return out
            halt = False
        elif kind == "path":
            if record:
                out.append(value)
    return out


def taihen_validate(text):
    """parser.c taihen_config_validate's value: 0 at the first section name or path (check_utf8 is 0
    for any non-empty string), or after a lexer error; 1 for a file of comments. taiHEN keeps the file
    when it is 0 (plugin.c:129)."""
    for kind, _ in _tokens(text):
        if kind in ("name", "path", "error"):
            return 0
    return 1


# ---------------- lines, cut as taiHEN's lexer cuts them ----------------

class Line:
    __slots__ = ("start", "raw", "next", "b", "e", "nb", "ne", "kind", "halt")


def cut(t, pos):
    """The line at pos (< len(t)); CRLF is one line end here (taiHEN reads its LF as an empty line)."""
    n, i = len(t), pos
    while i < n and t[i] not in (13, 10):
        i += 1
    line = Line()
    line.start, line.raw = pos, i - pos
    if i < n:
        i += 2 if t[i] == 13 and i + 1 < n and t[i + 1] == 10 else 1
    line.next, line.halt, line.nb, line.ne = i, False, 0, 0
    b, e = pos, pos + line.raw
    while b < e and t[b] in SPACE:
        b += 1
    line.b, line.e = b, e
    if b == e or t[b] == 0x23:  # '#'
        line.kind = L_COMMENT
        return line
    while e > b and t[e - 1] in SPACE:
        e -= 1
    line.e = e
    if t[b] != 0x2A:  # '*'
        line.kind = L_PATH
        return line
    line.kind = L_SECTION
    k = b + 1
    while k < e and t[k] in SPACE:
        k += 1
    if k < e and t[k] == 0x21:  # '!'
        line.halt = True
        k += 1
        while k < e and t[k] in SPACE:
            k += 1
    line.nb, line.ne = k, e
    return line


def lines_of(t):
    pos = 0
    while pos < len(t):
        line = cut(t, pos)
        yield line
        pos = line.next


def ours(path):
    """Which of ours a plugin path (bytes) is: its file name after the last '/' or ':', in any case."""
    i = max(path.rfind(b"/"), path.rfind(b":")) + 1
    name = path[i:].lower()
    return next((k for k, v in _NAMES.items() if v == name), O_NONE)


def _name(t, line):
    return t[line.nb:line.ne]


def _content(t, line):
    return t[line.b:line.e]


def _line_ours(t, line):
    return ours(_content(t, line)) if line.kind == L_PATH else O_NONE


def _is_note(t, line, which=(NOTE, APP_NOTE)):
    return line.kind == L_COMMENT and _content(t, line).rstrip(SPACE) in [w.encode() for w in which]


def _blank(line):
    return line.kind == L_COMMENT and line.b == line.e


def is_game(name):
    """A section lines mode registers the worker in (bytes or str): a game's nine-character title ID
    (core/games.c vjo_game_is_candidate)."""
    if isinstance(name, str):
        name = name.encode("latin-1", "replace")
    return (len(name) == 9 and all(48 <= c <= 57 or 65 <= c <= 90 for c in name) and
            name not in (b"VITASHELL", b"VJOSET001") and (name == b"NPXS10028" or not name.startswith(b"NPXS")))


def game_title(value):
    """The section name for one --title value (PCSG00415, pcsg00415 or *PCSG00415), or TitleError."""
    title = value.strip().lstrip("*").strip().upper()
    if title in ("ALL", "KERNEL", "MAIN"):
        raise TitleError(f"--title {value}: the OCR worker is registered per game, never under "
                         "*ALL, *main or *KERNEL")
    if len(title) != 9 or not all(c.isascii() and c.isalnum() for c in title):
        raise TitleError(f"--title {value!r} is not a title ID: expected 9 letters and digits, "
                         "such as PCSG00415")
    if not is_game(title):
        raise TitleError(f"--title {title} is a system application, VitaShell or the settings app, "
                         "where the OCR worker never runs (for Adrenaline see docs/guide.md 'Install')")
    return title


def registered(text):
    """The games (str) whose sections hold a canonical worker line, in file order (lines mode's
    games registered before)."""
    if isinstance(text, (list, tuple)):  # the lines of a configuration, as earlier versions took them
        text = "\n".join(text).encode("utf-8", "surrogateescape")
    games, section = [], None
    for line in lines_of(text):
        if line.kind == L_SECTION:
            section = line
        elif (line.kind == L_PATH and section is not None and _content(text, line) == OCR.encode() and
              is_game(_name(text, section))):
            name = _name(text, section).decode("ascii")
            if name not in games:
                games.append(name)
    return games


class _Sim:
    """taiHEN's reading of one title as the edit walks the lines (parser.c:237-264)."""

    def __init__(self, title):
        self.title, self.kernel = title, title == b"KERNEL"
        self.record = self.halt = self.stopped = False

    def section(self, name, halt):
        if self.stopped:
            return
        if self.record and self.halt:
            self.stopped = True
            return
        self.halt = halt
        self.record = (not self.kernel and name == b"ALL") or name == self.title

    def loads(self):
        return self.record and not self.stopped


def _line_end(t):
    """The line end of added lines: CRLF when the file has one, else LF, else CR when it has only CR."""
    cr = False
    for i, c in enumerate(t):
        if c == 13 and i + 1 < len(t) and t[i + 1] == 10:
            return b"\r\n"
        if c == 13:
            cr = True
        if c == 10:
            return b"\n"
    return b"\r" if cr else b"\n"


class _Edit:
    def __init__(self, t, op, mode, titles):
        self.t, self.op, self.mode, self.nl = t, op, mode, _line_end(t)
        self.games = []  # [name(bytes), has_header, has_line, kept, first_done]
        self.keep_marked_headers = False
        for title in titles:
            self._game(title.encode())

    def _game(self, name):
        for g in self.games:
            if g[0] == name:
                return g
        if len(self.games) >= GAMES_MAX:
            raise Refused(E_SPACE, 0, "More than 512 games have the OCR worker.")
        g = [name, False, False, False, False]
        self.games.append(g)
        return g

    def _find(self, name):
        return next((g for g in self.games if g[0] == name), None)

    # pass 1
    def analyse(self):
        t = self.t
        k, m = _Sim(b"KERNEL"), _Sim(b"main")
        self.kernel_keep = self.shell_keep = self.kernel_header = self.main_header = -1
        section, content = None, False
        for number, line in enumerate(lines_of(t), 1):
            if line.raw >= LINE_MAX:
                raise Refused(E_LONG_LINE, number, "A line is 256 bytes or longer: taiHEN stops reading the file "
                              "there, so nothing after it loads. Shorten or remove it in VitaShell first.")
            if line.kind == L_COMMENT:
                continue
            content = True
            if line.kind == L_SECTION:
                name = _name(t, line)
                k.section(name, line.halt)
                m.section(name, line.halt)
                if self.kernel_header < 0 and name == b"KERNEL":
                    self.kernel_header = line.start
                if self.main_header < 0 and name == b"main":
                    self.main_header = line.start
                if is_game(name):
                    g = self._find(name)
                    if g:
                        g[1] = True
                section = line
                continue
            o = _line_ours(t, line)
            if o == O_KERNEL and self.kernel_keep < 0 and _content(t, line) == KERNEL.encode() and k.loads():
                self.kernel_keep = line.start
            # kept only in a section named main (core/taicfg.c): *ALL matches main too, but is another section
            if (o == O_SHELL and self.shell_keep < 0 and _content(t, line) == SHELL.encode() and m.loads() and
                    section is not None and _name(t, section) == b"main"):
                self.shell_keep = line.start
            if (o == O_WORKER and self.mode == WORKER_LINES and self.op == INSTALL and section is not None and
                    _content(t, line) == OCR.encode() and is_game(_name(t, section))):
                g = self._game(_name(t, section))
                g[1] = g[2] = True
        if not t:
            raise Refused(E_EMPTY, 0, "It is empty: taiHEN may not start with it. Restore a backup "
                          "(config.txt.vjo-*.bak) or HENkaku's default first.")
        if not content:
            raise Refused(E_NO_SECTION, 0, "It has no section (*KERNEL, *main) and no plugin: taiHEN loads "
                          "nothing from it. Restore a backup (config.txt.vjo-*.bak) or HENkaku's default first.")

    def fate(self, line, section):
        """(KEEP | COMMENT | REMOVE, the game whose worker line is kept or None)."""
        t, o = self.t, _line_ours(self.t, line)
        if not o:
            return KEEP, None
        if self.op == UNINSTALL:
            return REMOVE, None
        if o == O_KERNEL:
            return (KEEP if line.start == self.kernel_keep else COMMENT), None
        if o == O_SHELL:
            return (KEEP if line.start == self.shell_keep else COMMENT), None
        if self.mode == WORKER_KERNEL:
            return REMOVE, None
        if o == O_TEXT:
            return COMMENT, None
        if section is not None and _content(t, line) == OCR.encode() and is_game(_name(t, section)):
            g = self._find(_name(t, section))
            if g and not g[3]:
                return KEEP, g
        return COMMENT, None

    def emptied(self, header):
        """(the section of header has nothing left but blank lines and notes, it held a line of our
        worker or Text plugin)."""
        held = False
        pos = header.next
        while pos < len(self.t):
            line = cut(self.t, pos)
            pos = line.next
            if line.kind == L_SECTION:
                break
            if line.kind == L_COMMENT:
                if not _blank(line) and not _is_note(self.t, line):
                    return False, held
                continue
            if _line_ours(self.t, line) in (O_WORKER, O_TEXT):
                held = True
            if self.fate(line, header)[0] != REMOVE:
                return False, held
        return True, held

    def removable(self, header, marked):
        """core/taicfg.c removable: an install removes a title's emptied section that held our worker
        (never by a note: one left by an earlier edit would make a second install remove more); an
        uninstall that too, or a header marked by a note right before it."""
        if header.kind != L_SECTION or header.halt:
            return False
        if self.op == INSTALL and self.mode == WORKER_LINES:
            return False
        empty, held = self.emptied(header)
        if not empty:
            return False
        reserved = _name(self.t, header) in (b"KERNEL", b"main", b"ALL")
        if self.op == INSTALL:
            return held and not reserved
        if reserved:
            return marked and not self.keep_marked_headers
        return held or marked

    def note_goes(self, line):
        """A note dropped wherever it is: every one in an uninstall, #124's in a kernel-mode install."""
        return _is_note(self.t, line) and (self.op == UNINSTALL or
                                           (self.mode == WORKER_KERNEL and _is_note(self.t, line, (APP_NOTE,))))

    # pass 2
    def emit(self, report):
        t, nl, out = self.t, self.nl, bytearray()

        def line_start():
            if out and out[-1] not in (10, 13):
                out.extend(nl)

        def put_line(line):
            line_start()
            out.extend(t[line.start:line.next])

        def put_new(s):
            line_start()
            out.extend(s.encode() if isinstance(s, str) else s)
            out.extend(nl)

        def note_section(section):
            name = "(no section)" if section is None else "*" + _name(t, section)[:30].decode("latin-1")
            if name not in report["removed_sections"]:
                report["removed_sections"].append(name)

        for g in self.games:
            g[3] = g[4] = False
        section, held, in_removed = None, None, False
        for line in lines_of(t):
            if line.kind == L_SECTION:
                marked = held is not None and _is_note(t, held)
                if self.removable(line, marked):
                    if held is not None:
                        if _is_note(t, held):
                            report["removed"] += 1
                        held = None
                    report["headers_removed"] += 1
                    in_removed, section = True, line
                    continue
                in_removed = False
                if held is not None:
                    if self.note_goes(held):
                        report["removed"] += 1
                    else:
                        put_line(held)
                    held = None
                put_line(line)
                section = line
                if self.op != INSTALL:
                    continue
                if line.start == self.kernel_header and self.kernel_keep < 0:
                    put_new(KERNEL)
                    report["added"] += 1
                if line.start == self.main_header and self.shell_keep < 0:
                    put_new(SHELL)
                    report["added"] += 1
                if self.mode == WORKER_LINES and is_game(_name(t, line)):
                    g = self._find(_name(t, line))
                    if g and not g[4]:
                        g[4] = True
                        if not g[2]:
                            put_new(OCR)
                            g[3] = True
                            report["added"] += 1
                continue
            if held is not None:
                if self.note_goes(held):
                    report["removed"] += 1
                else:
                    put_line(held)
                held = None
            if line.kind == L_COMMENT:
                if _blank(line) or _is_note(t, line):
                    held = line
                    continue
                put_line(line)
                continue
            f, g = self.fate(line, section)
            if f == KEEP:
                if g:
                    g[3] = True
                if not in_removed:
                    put_line(line)
                continue
            if _line_ours(t, line) in (O_WORKER, O_TEXT):
                note_section(section)
            if f == COMMENT and len(DISABLED) + line.raw < LINE_MAX:
                line_start()
                out.extend(DISABLED.encode())
                out.extend(t[line.start:line.next])
                report["commented"] += 1
                continue
            report["removed"] += 1
        if held is not None:
            if self.note_goes(held):
                report["removed"] += 1
            else:
                put_line(held)
        if self.op != INSTALL:
            return bytes(out)
        if self.kernel_header < 0:
            put_new(NOTE), put_new("*KERNEL"), put_new(KERNEL)
            report["added"] += 1
            report["sections_added"] += 1
        if self.main_header < 0:
            put_new(NOTE), put_new("*main"), put_new(SHELL)
            report["added"] += 1
            report["sections_added"] += 1
        if self.mode == WORKER_LINES:
            for g in self.games:
                if g[1]:
                    continue
                put_new(NOTE), put_new(b"*" + g[0]), put_new(OCR)
                g[3] = True
                report["added"] += 1
                report["sections_added"] += 1
        return bytes(out)

    def check_title(self, old, new, title):
        a, b = taihen_parse(old, title), taihen_parse(new, title)
        others = lambda paths: [p for p in paths if not ours(p)]
        if others(a) != others(b):
            raise Refused(E_CHECK, 0, "The edit would change what taiHEN loads for *%s besides the overlay: "
                          "nothing was changed (please report this)." % title[:40].decode("latin-1"))
        count = lambda kind: sum(1 for p in b if ours(p) == kind)
        is_kernel, is_main = title == b"KERNEL", title == b"main"
        game = (self.op == INSTALL and self.mode == WORKER_LINES and not is_kernel and not is_main and
                self._find(title) is not None)
        want_kernel, want_shell = self.op == INSTALL and is_kernel, self.op == INSTALL and is_main
        shown = title[:40].decode("latin-1")
        if count(O_KERNEL) != want_kernel:
            raise Refused(E_CHECK, 0, "taiHEN would not load the kernel plugin from it exactly once (a *! section "
                          "stops it before *KERNEL?): nothing was changed." if want_kernel else
                          "taiHEN would load the kernel plugin for *%s: nothing was changed." % shown)
        if count(O_SHELL) != want_shell:
            raise Refused(E_CHECK, 0, "taiHEN would not reach the Shell plugin's line (a *! section, *!ALL say, "
                          "stops it before *main): nothing was changed." if want_shell else
                          "taiHEN would load the Shell plugin for *%s: nothing was changed." % shown)
        if count(O_WORKER) != game or count(O_TEXT):
            raise Refused(E_CHECK, 0, ("taiHEN would not load the OCR worker once for *%s (a *! section stops "
                                       "it?): nothing was changed." if game else
                                       "taiHEN would load an old OCR plugin line for *%s: nothing was changed.") % shown)

    def check(self, old, new):
        if not new or taihen_validate(new) != 0:
            raise Refused(E_CHECK, 0, "taiHEN would not keep the result: nothing was changed.")
        if any(line.raw >= LINE_MAX for line in lines_of(new)):
            raise Refused(E_CHECK, 0, "The result would have a line taiHEN stops at.")
        self.check_title(old, new, b"KERNEL")
        self.check_title(old, new, b"main")
        seen = {b"KERNEL", b"main"}
        for text in (old, new):
            for line in lines_of(text):
                if line.kind == L_SECTION and _name(text, line) not in seen:
                    seen.add(_name(text, line))
                    self.check_title(old, new, _name(text, line))
        for i in range(1000):
            title = b"VJOCHK%03d" % i
            if title not in seen:
                self.check_title(old, new, title)
                break


def edit(raw, op=INSTALL, mode=WORKER_KERNEL, titles=()):
    """core/taicfg.c vjo_tai_edit: (the result, a report dict), or Refused / TitleError."""
    if op not in (INSTALL, UNINSTALL) or mode not in (WORKER_KERNEL, WORKER_LINES):
        raise Refused(E_ARGS, 0, "The editor was called wrongly.")
    if len(raw) > TEXT_MAX:
        raise Refused(E_TOO_LARGE, 0, "It is over 64 KiB, larger than a taiHEN configuration should be: "
                      "restore a backup (config.txt.vjo-*.bak) or HENkaku's default first.")
    nul = raw.find(b"\0")
    if nul >= 0:
        line = 1 + raw[:nul].replace(b"\r\n", b"\n").replace(b"\r", b"\n").count(b"\n")
        raise Refused(E_NUL, line, "It has a NUL byte: taiHEN stops reading there (HENkaku rewrites such a file). "
                      "Restore a backup (config.txt.vjo-*.bak) or HENkaku's default first.")
    names = []
    if op == INSTALL and mode == WORKER_LINES:
        for value in titles:
            title = game_title(value)
            if title not in names:
                names.append(title)
    x = _Edit(raw, op, mode, names)
    x.analyse()
    report = {"changed": False, "added": 0, "sections_added": 0, "commented": 0, "removed": 0, "headers_removed": 0,
              "padded": False, "bom_hides_section": False, "removed_sections": []}
    first = cut(raw, 0) if raw else None
    if first and raw.startswith(b"\xef\xbb\xbf"):
        rest = raw[3:first.raw].lstrip(SPACE)
        report["bom_hides_section"] = rest[:1] == b"*"
    for attempt in (0, 1):
        kept = {k: (list(v) if isinstance(v, list) else v) for k, v in report.items()}
        x.keep_marked_headers = bool(attempt)
        out = x.emit(report)
        if len(out) > TEXT_MAX:
            raise Refused(E_SPACE, 0, "The result would be over 64 KiB.")
        if out and len(out) % 4096 == 0 and out != raw:  # an unchanged file is not written at all
            out += x.nl
            report["padded"] = True
        if len(out) > TEXT_MAX:
            raise Refused(E_SPACE, 0, "The result would be over 64 KiB.")
        if op == UNINSTALL and not attempt and taihen_validate(out) != 0:
            report = kept
            continue
        break
    report["changed"] = out != raw
    x.check(raw, out)
    return out, report


def add_kernel_plugins(raw, plugins):
    """Another kernel plugin (install_ftp.sh --add-kernel-plugin) under the first *KERNEL header, once:
    the result, which loads the same for every other title, keeps KERNEL's other plugins in their order,
    and gets the editor's 4 KiB pad."""
    t = raw
    added = []
    for plugin in plugins:
        path = plugin.encode()
        if len(path) >= LINE_MAX:
            raise Refused(E_LONG_LINE, 0, "The other kernel plugin's path is 256 bytes or longer: taiHEN would stop "
                          "reading the file there.")
        k = _Sim(b"KERNEL")
        present, header = False, None
        for line in lines_of(t):
            if line.kind == L_SECTION:
                k.section(_name(t, line), line.halt)
                if header is None and _name(t, line) == b"KERNEL":
                    header = line
            elif line.kind == L_PATH and k.loads() and _content(t, line) == path:
                present = True
        if present or header is None:
            continue
        nl, at = _line_end(t), header.next
        lead = nl if header.next == header.start + header.raw else b""
        t = t[:at] + lead + path + nl + t[at:]
        added.append(path)
    if t != raw and len(t) % 4096 == 0:  # taiHEN's buffer needs room for the C string's end (the editor's pad)
        t += _line_end(t)
    before, after = list(taihen_parse(raw, b"KERNEL")), list(taihen_parse(t, b"KERNEL"))
    if ([p for p in after if p not in added] != before or sorted(p for p in after if p in added) != sorted(added)
            or taihen_parse(raw, b"main") != taihen_parse(t, b"main")):
        raise Refused(E_CHECK, 0, "Adding the other kernel plugins would change what taiHEN loads.")
    return t


def patch(raw, extra_kernel=(), uninstall=False, titles=(), keep_from=(), mode=WORKER_KERNEL):
    """The PC installer's edit: (bytes). lines mode registers the worker for titles and the games
    registered in this configuration and the keep_from ones (TitleError when there is none)."""
    if uninstall:
        return edit(raw, UNINSTALL, mode)[0]
    names = list(titles)
    if mode == WORKER_LINES:
        for other in keep_from:
            names += [g for g in registered(other) if g not in names]
        if not names and not registered(raw):
            for value in titles:
                game_title(value)
            raise TitleError(NO_TITLES)
    out, _ = edit(raw, INSTALL, mode, names)
    return add_kernel_plugins(out, extra_kernel) if extra_kernel else out


def summary(report, op, mode, raw, out, games=()):
    """One line for the installer's output; games: lines mode's, in the editor's order (the --title
    games, the other configurations' and this one's), so that every configuration names them alike."""
    parts = []
    if op == UNINSTALL:
        parts.append("removed the overlay's lines" if report["removed"] else "no line of the overlay")
    else:
        if report["added"]:
            parts.append("added %d line%s" % (report["added"], "" if report["added"] == 1 else "s"))
        if mode == WORKER_KERNEL and report["removed_sections"]:
            parts.append("removed the OCR worker's lines from " + ", ".join(report["removed_sections"]) +
                         " (the kernel plugin loads it now)")
        if mode == WORKER_LINES:
            loaded = [g for g in games if g in registered(out)]
            loaded += [g for g in registered(out) if g not in loaded]
            parts.append("OCR worker registered for " + (", ".join(loaded) if loaded else "no game"))
        if report["commented"]:
            parts.append("commented out %d old line%s" % (report["commented"], "" if report["commented"] == 1 else "s"))
    if report["bom_hides_section"]:
        parts.append("note: a byte-order mark hides the first line's section from taiHEN")
    if not report["changed"]:
        parts.append("unchanged")
    return "; ".join(parts) or "unchanged"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--worker", choices=(WORKER_KERNEL, WORKER_LINES), required=True,
                        help="kernel: the kernel plugin loads the OCR worker (no worker lines); lines: one "
                             "*TITLEID section per game")
    parser.add_argument("--kernel-plugin", action="append", default=[])
    parser.add_argument("--title", action="append", default=[],
                        help="lines mode: a game to load the OCR worker in (repeatable); ignored in kernel mode")
    parser.add_argument("--keep-titles-from", action="append", default=[], type=Path,
                        help="lines mode: another configuration whose registered games are kept (repeatable)")
    parser.add_argument("--uninstall", action="store_true")
    args = parser.parse_args()
    try:
        raw = args.source.read_bytes()
        keep = [path.read_bytes() for path in args.keep_titles_from]
    except OSError as error:
        parser.error(str(error))
    op = UNINSTALL if args.uninstall else INSTALL
    names = []
    try:
        if op == INSTALL and args.worker == WORKER_LINES:
            names = [game_title(value) for value in args.title]
            for other in keep:
                names += [g for g in registered(other) if g not in names]
            if not names and not registered(raw):
                for value in args.title:
                    game_title(value)
                raise TitleError(NO_TITLES)
            out, report = edit(raw, op, args.worker, names)
        else:
            out, report = edit(raw, op, args.worker)
        if op == INSTALL and args.kernel_plugin:
            out = add_kernel_plugins(out, args.kernel_plugin)
            report["changed"] = out != raw
    except TitleError as error:
        print(error, file=sys.stderr)
        raise SystemExit(3)
    except Refused as error:
        where = f" (line {error.line})" if error.line else ""
        print(f"{args.source.name}{where}: {error.why}", file=sys.stderr)
        raise SystemExit(4)
    try:
        args.output.write_bytes(out)
    except OSError as error:
        parser.error(str(error))
    print(summary(report, op, args.worker, raw, out, names + [g for g in registered(raw) if g not in names]))


if __name__ == "__main__":
    main()
