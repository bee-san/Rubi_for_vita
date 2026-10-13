#!/usr/bin/env python3
"""Patch only this overlay's taiHEN entries; preserve other plugins and CRLF.

The kernel plugin goes under *KERNEL and the Shell plugin under *main. The game
OCR worker goes under one *TITLEID section per game and never under *ALL, which
would load it into SceShell and every application as well (docs/vita-memory.md,
section 4). Its games are the --title IDs plus every game already registered in
this configuration or a --keep-titles-from one; with none, the patch is refused.
"""
import argparse
import re
import sys
from pathlib import Path

KERNEL = "ur0:tai/VitaJPOverlay_Kernel.skprx"
SHELL = "ur0:tai/VitaJPOverlay_Shell.suprx"
TEXT = "ur0:tai/VitaJPOverlay_Text.suprx"
OCR = "ur0:tai/VitaJPOverlay_OCR.suprx"
DISABLED = "# Mainline OCR: "
# The PSP emulator's ID, which the overlay uses for Adrenaline's PSP games
# (core/games.c); README 'Install' explains PSPEMUCFW, Adrenaline's own ID.
PSP_EMULATOR = "NPXS10028"
# Title IDs have the XXXXYYYYY form (taiHEN README, 'config.txt'), e.g. PCSG00415.
TITLE_ID = re.compile(r"[A-Z0-9]{9}")
NO_TITLES = ("No game is registered for the OCR worker (VitaJPOverlay_OCR.suprx), and it is never "
             "registered under *ALL. Name each game with --title TITLEID, for example "
             "--title PCSG00415 (CLANNAD); for Adrenaline's PSP games see README 'Install' (NPXS10028).")


class TitleError(ValueError):
    """A --title the worker cannot serve, or no game to register it for."""


def section_name(item):
    """The name a stripped '*' line opens, as taiHEN's lexer reads it: the halt
    mark '!' and the spaces around it are not part of the name."""
    name = item[1:].lstrip()
    if name.startswith("!"):
        name = name[1:].lstrip()
    return name


def is_game(name):
    """The worker serves this process: what core/games.c's
    vjo_game_is_candidate accepts, in the nine-character title ID form."""
    return (TITLE_ID.fullmatch(name) is not None and name not in ("VITASHELL", "VJOSET001") and
            (name == PSP_EMULATOR or not name.startswith("NPXS")))


def game_title(value):
    """The section name for one --title value (PCSG00415, pcsg00415 or
    *PCSG00415), or TitleError. Title IDs are upper case and taiHEN compares
    section names case-sensitively, so the value is upper-cased."""
    title = value.strip().lstrip("*").strip().upper()
    if title in ("ALL", "KERNEL", "MAIN"):
        raise TitleError(f"--title {value}: the OCR worker is registered per game, never under "
                         "*ALL, *main or *KERNEL")
    if not TITLE_ID.fullmatch(title):
        raise TitleError(f"--title {value!r} is not a title ID: expected 9 letters and digits, "
                         "such as PCSG00415")
    if not is_game(title):
        raise TitleError(f"--title {title} is a system application, VitaShell or the settings app, "
                         "where the OCR worker never runs (for Adrenaline see README 'Install')")
    return title


def registered(lines):
    """The games whose sections hold an active worker line, in file order."""
    titles, current = [], None
    for line in lines:
        item = line.strip()
        if item.startswith("*"):
            current = section_name(item)
        elif item == OCR and current is not None and is_game(current) and current not in titles:
            titles.append(current)
    return titles


def add(lines, name, plugin):
    """Adds plugin under section name once: after the section's first header,
    or in a new section at the end."""
    current = None
    for line in lines:
        item = line.strip()
        if item.startswith("*"):
            current = section_name(item)
        elif current == name and item == plugin:
            return
    begin = next((i for i, line in enumerate(lines)
                  if line.strip().startswith("*") and section_name(line.strip()) == name), None)
    if begin is None:
        lines.extend(["", "*" + name, plugin])
        return
    lines.insert(begin + 1, plugin)


def games_for(lines, titles=(), keep_from=()):
    """The games to register the worker for: the --title IDs, then the games
    registered in the other configurations and in this one, without repeats.
    Every configuration patched with the same arguments gets the same list."""
    games = []
    for value in titles:
        title = game_title(value)
        if title not in games:
            games.append(title)
    for other in [config.decode("utf-8").splitlines() for config in keep_from] + [lines]:
        games += [title for title in registered(other) if title not in games]
    return games


def patch(raw: bytes, extra_kernel=(), uninstall=False, titles=(), keep_from=()) -> bytes:
    source = raw.decode("utf-8")
    newline = "\r\n" if "\r\n" in source else "\n"
    lines = source.splitlines()
    if uninstall:
        lines = [line for line in lines if line.strip() not in (KERNEL, SHELL, TEXT, OCR)]
        return (newline.join(lines) + newline).encode("utf-8")
    games = games_for(lines, titles, keep_from)
    if not games:
        raise TitleError(NO_TITLES)
    add(lines, "KERNEL", KERNEL)
    for plugin in extra_kernel:
        add(lines, "KERNEL", plugin)
    add(lines, "main", SHELL)
    # Comment out old Text registrations and every worker line that is not the
    # first in one of the games' sections: *ALL, *main, system apps, duplicates.
    # Repeating an update is stable.
    current = None
    kept = set()
    for index, line in enumerate(lines):
        item = line.strip()
        if item.startswith("*"):
            current = section_name(item)
        elif item == TEXT or (item == OCR and (current not in games or current in kept)):
            lines[index] = DISABLED + line
        elif item == OCR:
            kept.add(current)
    for title in games:
        add(lines, title, OCR)
    return (newline.join(lines) + newline).encode("utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--kernel-plugin", action="append", default=[])
    parser.add_argument("--title", action="append", default=[],
                        help="a game to load the OCR worker in (repeatable)")
    parser.add_argument("--keep-titles-from", action="append", default=[], type=Path,
                        help="another configuration whose registered games are kept (repeatable)")
    parser.add_argument("--uninstall", action="store_true")
    args = parser.parse_args()
    try:
        keep = [path.read_bytes() for path in args.keep_titles_from]
        raw = args.source.read_bytes()
        result = patch(raw, args.kernel_plugin, args.uninstall, args.title, keep)
    except TitleError as error:
        print(error, file=sys.stderr)
        raise SystemExit(3)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    try:
        args.output.write_bytes(result)
    except OSError as error:
        parser.error(str(error))
    if not args.uninstall:
        games = games_for(raw.decode("utf-8").splitlines(), args.title, keep)
        print(f"OCR worker registered for {', '.join(games)}")


if __name__ == "__main__":
    main()
