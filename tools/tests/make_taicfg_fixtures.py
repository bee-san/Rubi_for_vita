#!/usr/bin/env python3
"""The config.txt editor's fixtures (tools/tests/fixtures/taicfg/*.in), with the byte-exact cases
(CR, BOM, NUL, long lines) written from here so nothing in git can change them.

    python3 tools/tests/make_taicfg_fixtures.py          # writes the inputs
    python3 tools/tests/make_taicfg_fixtures.py --check  # exit 1 when a committed input differs

Each input NAME.in has expected results beside it, made by core/taicfg.c and checked by hand:
NAME.kernel.out (install, the kernel loads the worker), NAME.lines.out (install, worker lines; the
games of NAME.titles, one per line, when that file exists), NAME.uninstall.out, or NAME.refused
(the refusal's code and line) instead of all three. host/tests/test_taicfg.c and
tools/tests/test_tai_config.py run both editors on them.

Sources: HENkaku's own configurations (henkaku 784e483 bootstrap/bootstrap.c:46-69, the recovery
header for ur0, and update_taihen_config's appended copy, :543-559); Adrenaline's kernel line
(TheOfficialFloW/Adrenaline 7d382b7 readme.md:27-32); the overlay's earlier installers
(v0.7.12's *ALL worker line, install-vita.sh --title's blank line + *TITLEID + worker, #124's note
+ section, the guide's manual install); and the cases of the design audit (bytes that are line
ends or spaces to Python's str but not to taiHEN, 255/256-byte lines, case and spacing of section
names, *! sections, plugins in the wrong section, 4 KiB lengths, non-UTF-8 comments).
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent / "fixtures" / "taicfg"
K = "ur0:tai/VitaJPOverlay_Kernel.skprx"
S = "ur0:tai/VitaJPOverlay_Shell.suprx"
W = "ur0:tai/VitaJPOverlay_OCR.suprx"
T = "ur0:tai/VitaJPOverlay_Text.suprx"
APP_NOTE = "# VitaJPOverlay: the OCR worker in this Adrenaline bubble's PSP game (Overlay app)"

# henkaku 784e483 bootstrap/bootstrap.c:46-69
RECOVERY = "# This file is used as an alternative if ux0:tai/config.txt is not found.\n"
UPDATED = "# henkaku.suprx and henkaku.skprx has moved to ur0:tai, you may remove\n# the old entries above.\n"
HEADER = ("# For users plugins, you must refresh taiHEN from HENkaku Settings for\n# changes to take place.\n"
          "# For kernel plugins, you must reboot for changes to take place.\n")
CONFIG = ("*KERNEL\n# henkaku.skprx is hard-coded to load and is not listed here\n*main\n"
          "# main is a special titleid for SceShell\nur0:tai/henkaku.suprx\n*NPXS10015\n"
          "# this is for modifying the version string\nur0:tai/henkaku.suprx\n*NPXS10016\n"
          "# this is for modifying the version string in settings widget\nur0:tai/henkaku.suprx\n")
STOCK_UX0 = HEADER + CONFIG
STOCK_UR0 = RECOVERY + HEADER + CONFIG


def crlf(s):
    return s.replace("\n", "\r\n")


def fill(base, size, nl="\n"):
    """base, then comment lines up to exactly size bytes (each line ends with nl)."""
    out = base
    while len(out.encode()) < size:
        room = size - len(out.encode()) - len(nl)
        out += "#" + "x" * max(0, min(room - 1, 200)) + nl if room >= 1 else nl
    assert len(out.encode()) == size, (len(out.encode()), size)
    return out


def fixtures():
    f = {}
    f["henkaku-ux0"] = STOCK_UX0
    f["henkaku-ur0"] = STOCK_UR0
    # an old HENkaku's ux0 file with the stock text appended again by update_taihen_config
    old = CONFIG.replace("ur0:tai/henkaku.suprx", "ux0:tai/henkaku.suprx").replace(
        "# henkaku.skprx is hard-coded to load and is not listed here\n", "ux0:tai/henkaku.skprx\n")
    f["henkaku-updated"] = HEADER + old + "\n" + UPDATED + CONFIG
    f["crlf"] = crlf(STOCK_UR0)
    f["cr-only"] = STOCK_UX0.replace("\n", "\r")
    # a CRLF file with a lone LF, a lone CR, a CR CR LF and no final line end
    f["mixed-endings"] = ("*KERNEL\r\nur0:tai/a.skprx\n*main\rur0:tai/henkaku.suprx\r\r\n"
                          "*NPXS10015\r\nur0:tai/henkaku.suprx")
    f["no-final-line-end"] = "*KERNEL\nur0:tai/a.skprx\n*main"
    f["adrenaline"] = STOCK_UX0.replace(
        "*KERNEL\n", "*KERNEL\nux0:app/PSPEMUCFW/sce_module/adrenaline_kernel.skprx\n")
    # v0.7.12's install-vita.sh: the worker under *ALL (CRLF, as tools/tests/test_tai_config.py has it)
    f["installed-0712"] = crlf("*KERNEL\n" + K + "\nur0:tai/Other.skprx\n*main\n" + S + "\nur0:tai/henkaku.suprx\n"
                               "*ALL\nur0:tai/Every.suprx\n" + W + "\n")
    # a test.3 install (install-vita.sh --title PCSG00415 --title PSPEMUCFW over v0.7.12's)
    f["installed-test3"] = ("*KERNEL\n" + K + "\nux0:app/PSPEMUCFW/sce_module/adrenaline_kernel.skprx\n*main\n" + S +
                            "\nur0:tai/henkaku.suprx\n*ALL\nur0:tai/Every.suprx\n# Mainline OCR: " + W +
                            "\n\n*PCSG00415\n" + W + "\n\n*PSPEMUCFW\n" + W + "\n")
    # #124: the Rubi app appended a bubble's section (in place, after a .vjo-app-bak)
    f["app-bubble"] = (STOCK_UX0.replace("*KERNEL\n", "*KERNEL\n" + K + "\n").replace("*main\n", "*main\n" + S + "\n")
                       + APP_NOTE + "\n*ULJS00378\n" + W + "\n" + APP_NOTE + "\n*PSPEMU005\n" + W + "\nux0:tai/other.suprx\n")
    f["text-plugin"] = ("*KERNEL\n" + K + "\n*main\n" + S + "\n*ALL\n" + T + "\n*PCSG00415\n" + T + "\n" + W + "\n")
    f["bom-comment"] = "\ufeff" + STOCK_UX0
    f["bom-kernel"] = "\ufeff*KERNEL\nur0:tai/Other.skprx\n*main\nur0:tai/henkaku.suprx\n"
    # bytes Python's str.splitlines()/strip() take for line ends or spaces, taiHEN does not
    f["python-spaces"] = ("# note\x0c*ALL\n*KERNEL\nur0:tai/a.skprx\x0b\n*main\nur0:tai/henkaku.suprx\n"
                          "# next\x85*ALL\n# para\u2028*ALL\n# sep\x1c*ALL\n"
                          "*PCSG00415\n\u3000" + W + "\n*PCSE00120\n" + W + "\u00a0\n")
    # a 255-byte raw line (spaces included) before and after the insertion points
    long_path = "ur0:tai/" + "p" * (255 - len("ur0:tai/") - len(".suprx") - 2) + ".suprx  "
    assert len(long_path) == 255
    f["long-255"] = "*KERNEL\n" + long_path + "\n*main\nur0:tai/henkaku.suprx\n" + long_path + "\n"
    f["case-sections"] = ("*kernel\nur0:tai/a.skprx\n*Main\nur0:tai/b.suprx\n*all\nur0:tai/c.suprx\n"
                          "* KERNEL\nur0:tai/d.skprx\n*  !  NPXS10015\t\nur0:tai/e.suprx\n*\tmain \nur0:tai/henkaku.suprx\n")
    f["halts"] = ("*!KERNEL\nur0:tai/a.skprx\n*KERNEL\nur0:tai/b.skprx\n*!main\nur0:tai/henkaku.suprx\n*main\n"
                  "ur0:tai/c.suprx\n*!ALL\nur0:tai/d.suprx\n*NPXS10015\nur0:tai/henkaku.suprx\n")
    f["wrong-sections"] = ("*KERNEL\n" + S + "\n" + W + "\n*main\n" + K + "\n" + W + "\nur0:tai/henkaku.suprx\n*NPXS10015\n"
                           + W + "\nur0:tai/henkaku.suprx\n*ALL\n" + K + "\n")
    f["duplicates"] = ("*KERNEL\nux0:tai/VitaJPOverlay_Kernel.skprx\n" + K + "\nUR0:TAI/vitajpoverlay_kernel.SKPRX\n" + K +
                       "\n*main\nur0:/tai/VitaJPOverlay_Shell.suprx\n" + S + "\n  " + S + "  \n*KERNEL\n" + K + "\n")
    f["user-comment"] = STOCK_UX0 + "*PCSG00415\n# mine: CLANNAD\n" + W + "\n"
    f["manual-install"] = (STOCK_UX0.replace("*KERNEL\n", "*KERNEL\n" + K + "\n").replace("*main\n", "*main\n" + S + "\n")
                           + "*PCSG00415\n" + W + "\n")
    f["halt-title"] = "*KERNEL\n" + K + "\n*main\n" + S + "\n*!PCSG00415\n" + W + "\n*ALL\nur0:tai/all.suprx\n"
    f["mainline-comments"] = ("*KERNEL\n# Mainline OCR: " + K + "\n# keep ur0:tai/VitaJPOverlay_Kernel.skprx in mind\n"
                              "*main\n# Mainline OCR: " + T + "\nur0:tai/henkaku.suprx\n")
    f["only-ours"] = "*KERNEL\n" + K + "\n*main\n" + S + "\n"
    # our Shell line under *ALL (it loads into SceShell too): commented out, and the one under *main kept;
    # with none under *main, one goes right after the first *main header
    f["shell-under-all"] = ("*KERNEL\n" + K + "\n*ALL\n" + S + "\nur0:tai/every.suprx\n*main\n" + S +
                            "\nur0:tai/henkaku.suprx\n")
    f["shell-only-under-all"] = "*KERNEL\n" + K + "\n*main\nur0:tai/henkaku.suprx\n*ALL\n" + S + "\n"
    f["no-kernel-main"] = "# just a comment\n*NPXS10015\nur0:tai/henkaku.suprx\n"
    f["japanese"] = STOCK_UX0 + "# 日本語のコメント：CLANNAD\n"
    # 4 KiB: an input of exactly 4,096 B; one whose install result is exactly 4,096 B before the pad;
    # one an uninstall brings to exactly 4,096 B; and the CRLF pad
    f["input-4096"] = fill(STOCK_UX0, 4096)
    grow = len(K) + 1 + len(S) + 1
    f["install-to-4096"] = fill(STOCK_UX0, 4096 - grow)
    f["uninstall-to-4096"] = fill(STOCK_UX0, 4096).replace("*KERNEL\n", "*KERNEL\n" + K + "\n", 1).replace(
        "*main\n", "*main\n" + S + "\n", 1)
    f["install-to-4096-crlf"] = fill(crlf(STOCK_UX0), 4096 - grow - 2, "\r\n")
    # refusals
    f["refuse-empty"] = ""
    f["refuse-comments-only"] = "# nothing\n\n   \n# at all\n"
    f["refuse-nul"] = "*KERNEL\nur0:tai/a.skprx\n*main\x00\nur0:tai/henkaku.suprx\n"
    f["refuse-long-256"] = "*KERNEL\n" + "#" * 256 + "\n*main\nur0:tai/henkaku.suprx\n"
    f["refuse-all-hides-main"] = "*KERNEL\nur0:tai/a.skprx\n*!ALL\nur0:tai/all.suprx\n*NPXS10015\nur0:tai/henkaku.suprx\n"
    return f


def raw_fixtures():
    """Inputs that are not UTF-8: a Latin-1 and a Shift-JIS comment."""
    return {"latin1": STOCK_UX0.encode() + "# caf\xe9 \xe0 la cr\xe8me\n".encode("latin-1"),
            "shift-jis": STOCK_UX0.encode() + "# 日本語\n".encode("shift_jis")}


# Lines mode's games, for the fixtures that name some (install-vita.sh --title).
TITLES = {"henkaku-ux0": ["PCSG00415", "pspemucfw"], "installed-0712": ["PCSG00415"],
          "app-bubble": ["PCSG00415"], "halt-title": ["PCSG00415", "PCSE00120"],
          "user-comment": ["PCSG00415"], "no-kernel-main": ["PCSG00415"]}


def main():
    check = "--check" in sys.argv[1:]
    HERE.mkdir(parents=True, exist_ok=True)
    wanted = {name + ".in": text.encode("utf-8") for name, text in fixtures().items()}
    wanted.update({name + ".in": data for name, data in raw_fixtures().items()})
    wanted.update({name + ".titles": ("\n".join(titles) + "\n").encode() for name, titles in TITLES.items()})
    bad = []
    for name, data in sorted(wanted.items()):
        path = HERE / name
        if check:
            if not path.is_file() or path.read_bytes() != data:
                bad.append(name)
        else:
            path.write_bytes(data)
    if check and bad:
        print("differs from make_taicfg_fixtures.py: " + ", ".join(bad))
        return 1
    print(("checked " if check else "wrote ") + str(len(wanted)) + " fixture inputs in " + str(HERE))
    return 0


if __name__ == "__main__":
    sys.exit(main())
