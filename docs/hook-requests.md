# Hook requests

A native text hook reads a game's own text, so the popup shows exactly what the game draws instead of OCR's
reading of the screen ([native text hooks](../README.md#native-text-hooks)). Rubi 0.9.8 has game-specific entries for
188 Vita title IDs: LunaTranslator's table and Rubi's own verified entries. Every other Vita game is read with OCR,
after Rubi tries its string copies. That long tail grows fastest when the people who play those games ask for them,
because each request brings what a verified entry needs from the player's own console: the debug build's
`hooks.log`. The owner's hook farm does the rest.

**[Open a hook request](https://github.com/bee-san/Rubi_for_vita/issues/new?template=hook-request.yml)** (a GitHub
form; it asks for the steps below).

## Ask for a hook

1. **Install the debug build.** Only the debug build writes `hooks.log`
   ([two builds](release-variants.md)). Recent releases (0.9.7, 0.9.8) are the normal build only; every test build
   has the debug one: the newest pre-release on the [releases page](https://github.com/bee-san/Rubi_for_vita/releases),
   `Rubi-<version>-debug.vpk` ([#351](https://github.com/bee-san/Rubi_for_vita/issues/351) says what the current one
   carries). Close the Rubi app if it is open, install the VPK with VitaShell, open Rubi, choose **Update** on its
   Install page and restart the Vita. Your settings, games and cards stay; the release's VPK, installed the same way,
   takes you back.
2. **Check two settings** in the Rubi app: the game is **ON** on the **Games** page, and **OCR & lookup → Text
   source** is **Automatic** (with **OCR only** nothing is written). Leave `hook_sites` out of `config.ini`, or on:
   with `hook_sites = off` no table site is tried, and the log says so.
3. **Start the game** and play until its first dialogue is on screen, then close it. The worker writes the log as
   the game starts, before the game's own code runs.
4. **Copy `ux0:data/VitaJPOverlay/hooks.log`** before you start another game: the start of every game that is ON
   rewrites it (VitaShell and the Rubi app do not: the worker is never loaded into them,
   [worker loading](worker-loading.md)). Any of these works:
   - VitaShell's USB connection: on the computer, the memory card's drive has it as `data/VitaJPOverlay/hooks.log`.
   - VitaShell's FTP connection (**Select**, once **SELECT button** is FTP in VitaShell's settings):
     `curl -o hooks.log ftp://VITA_IP:1337/ux0:/data/VitaJPOverlay/hooks.log`, or any FTP client in
     `ux0:/data/VitaJPOverlay/`.
   - With the debug build's zip extracted on a computer (`Rubi-<version>-debug.zip`, Python 3.9+, bash and curl):
     `bash tools/install_ftp.sh --collect VITA_IP` fetches it with every other log
     ([what it collects](self-test.md#collecting-everything---collect)) and writes `hook-request.md`, what
     `tools/hook_request.py` reads in it, beside them, with a one-line summary at the end of its output.
5. **[Open the form](https://github.com/bee-san/Rubi_for_vita/issues/new?template=hook-request.yml)**: the game, its
   title ID, your Rubi version, what the popup shows now, and the whole `hooks.log` pasted into its box. One request
   per title ID: look for an open one first and add your log to it.

Never attach the game or any file of it (its `eboot.bin`, a dump): the farm needs only `hooks.log`.

### What `hooks.log` says

It holds no game text (`game_ocr/hooks.c`): the title ID and text source, the game's main module (its name, module
NID, segment 0's address and size, and `APP_VER`, the game's version), each table site the worker tried with its
verdict and, for a refused one, the 16 bytes of code there, and each string function it hooked. An illustration, with
made-up numbers: a game LunaTranslator lists twice, whose sites Rubi refused, and whose string copies are hooked:

```text
PCSG99999: text source 1
main module adv NID 1A2B3C4D seg0 81000000+2F1A40 APP_VER 0100; 0 verified, 2 Luna sites
luna 8003A1C4 r0 +361C4 refused: entry
 bytes 4FF0000310B50446E1F7F2FC0028F6D0
luna 81196DDA r7 refused: range
 bytes 
import BE43BB07/7205BFDB: hooked
import BE43BB07/85B924B7: hooked
import BE43BB07/9F87712D: hooked
import BE43BB07/AF5C218D: not hooked
import CAE9ACE6/14E9DBD7: hooked
import CAE9ACE6/C458D60A: not hooked
import CAE9ACE6/2CDFCD1C: not hooked
import CAE9ACE6/AC595E68: hooked
sites hooked 0, refused 2; CALL imports hooked 5 of 8
```

| Line | What it says |
| --- | --- |
| `PCSG99999: text source 1` | the title ID; the text source (1 Automatic, 2 Every game), `, sites off` with `hook_sites = off` |
| `main module …` | the game's executable as the console loaded it: name, module NID, segment 0 (`+` its size), `APP_VER` (`0100` is 01.00; `0000` when param.sfo has none), and how many verified entries and Luna sites this build of Rubi has for the title |
| `verified +OFF thumb rN …` | one of Rubi's verified entries, at segment 0 + OFF, the text in register rN (`, Luna's text`: a Luna site at the same address cleans its text) |
| `luna ADDR rN +OFF …` | one of LunaTranslator's sites, read at segment 0 + OFF (no `+OFF`: no reading of its address is inside segment 0) |
| `… hooked, stream N` | installed: its text goes to stream N |
| `… refused: REASON` | not installed, and why (below); the next line, ` bytes …`, is the code there (empty outside segment 0) |
| `… skipped: …` | the address is hooked already (an entry and a Luna site at one address) |
| `import LIB/FUNC: hooked` | a string function of the game (memcpy, strcpy …) that the worker hooked, the string-copy source |
| `sites hooked …; CALL imports hooked …` | the totals: the log's last line; a log without it stopped before the worker finished |
| `nothing: …` | nothing was hooked: the PSP emulator's process (no native PSP hooks yet), no title ID, or no main module |
| `psp probe 1: …` | the debug build's PSP probe, in a PSP game ([PSP probe](native-hooks.md#psp-probe-debug-build)) |

The reasons: `range` (outside segment 0), `align`, `entry` (not a function's entry that saves LR: taiHEN cannot move
its first instructions safely), `window` (an instruction there is not on the placement check's allow-list),
`signature` (a verified entry's module NID or bytes differ: another build of the game), `unported` (it needs code Rubi
has not ported), `version` (an entry for another `APP_VER`), `full` (all 8 site slots taken), `tai` (taiHEN refused
it). How the worker decides: [native-hooks.md](native-hooks.md#table-sites).

The debug build of Rubi 0.9.8 and older reads `APP_VER` only for a game that has an entry or a Luna site that needs
it, so for any other game the line says `APP_VER 0000`; later debug builds read it for every game. Give the version
in the form then (Game version).

## What happens to a request

1. **Triage.** A maintainer reads the request with `tools/hook_request.py` (below). It says what the console saw and
   what is needed: nothing (Rubi's verified entry is installed: a problem with its text is a bug report), a newer
   build (this checkout has entries the player's build had not), or the farm: no entry for the title, LunaTranslator's
   sites refused, or an entry for another build of the game (`signature` or `version`: the module NID and `APP_VER`
   say which build the player has).
2. **The farm.** The owner's hook farm (bee-san/psandvita, private) runs the Japanese release of the title in the
   Vita3K emulator: an engineer agent finds or builds a hook at the first text box, and reviewers approve its text
   page by page against the frames the game drew. It farms Japanese visual novels. Its result
   for the title is `rubi-hook.json` in its data bucket: the module NID, segment-0 offset, register, encoding, the 16
   bytes at the site and `APP_VER`. With `--bucket`, `tools/hook_request.py` says whether the farm proved the
   player's build: the same module NID, and at a site the log has bytes for, the same bytes.
3. **The entry.** The farm's `scripts/rubi_export.py --titles <ID> --rubi <a Rubi checkout>` checks that taiHEN can
   move the instructions at the site (on the farm's dump of segment 0) and prints the entry's row for
   `core/hook_place.c` (its `docs/rubi-export.md`). A pull request adds it, with Rubi's clean-up filter for the
   game's text codes where it needs one ([Rubi's filters](native-hooks.md#rubis-filters)) and the farm checks'
   numbers (`tools/hook_farm.py --bucket DIR check <ID>`, [farm checks](native-hooks.md#farm-checks)).
4. **The console check.** The entry ships in the next test build ([#351](https://github.com/bee-san/Rubi_for_vita/issues/351)).
   The player installs it, plays the game and sends the new `hooks.log` to the request: `verified +OFF … hooked,
   stream 1`, and a popup that shows the game's line, close the request. Until a console has run it, an entry is
   "proven in Vita3K only".

## For maintainers

```sh
# a request's text, from its issue (or a comment's, with gh api)
gh issue view N -R bee-san/Rubi_for_vita --json body -q .body | python3 tools/hook_request.py -
# a --collect folder or zip: the log, and log.txt's readings of the game (numbers only)
python3 tools/hook_request.py build/device/collect/vjo-collect-YYYYMMDD-HHMMSSZ.zip
# against the farm's bucket: did it prove this build?
python3 tools/hook_request.py --bucket ~/farm-bucket hooks.log
# as data (the farm's spellings: "0x1EE45F02", "01.00"), for a farm queue or a PR body
python3 tools/hook_request.py --json hooks.log
```

`tools/hook_request.py` compares the log with the checkout's tables (`core/hook_place.c`'s verified entries,
`core/hook_luna_table.inc`'s Luna sites; `--rubi DIR` names another checkout), and with `--bucket` with the farm's
`vita/<ID> <title>/hook/rubi-hook.json`. It reads only the logs and never prints text: `log.txt`'s game text stays in
the file. Requests carry the `hook request` label.

## Privacy

`hooks.log` holds no text and nothing personal. The debug build's `log.txt` holds the text Rubi read, and a
`--collect` zip also holds your Anki queue (your cards: words and sentences) and your settings (API keys
redacted, [what it collects](self-test.md#collecting-everything---collect)): attach those only if you are happy to
share them publicly, or send them to the owner privately ([Discord](http://discord.skerritt.blog)).
