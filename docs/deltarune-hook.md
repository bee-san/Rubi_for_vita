# Experimental Deltarune v0.73 hook

This prototype prepares a separate on-disk profile for WolffsRoom's native
Butterscotch port, title `DELT00001`, APP_VER `00.73`. It has **no console text
verification**. It is not included in the shipped hook catalogue or title counts.
The older YoYo Chapter One app (`DLTR00000`) is a different target.

The installed release's executable SHA-256 is
`9d9c7a6d464850b5b8140ccfefc8d38c603c5f81a06ed64408e93753be4710b2`;
its main-module NID is `8D433617`.

## Capture point

The port's `RValue_makeIndependent` and `RValue_toString` duplicate GameMaker
strings. The executable's `_strdup_r` at segment-0 offset `1B11F0` receives its
source string in r1. Its first eight bytes are
`70 B5 04 46 08 46 0D 46`: PUSH and register MOV instructions. taiHEN's eight-byte
patch ends before the relocated BL at +8. The tool checks the exact executable,
module NID, signature and absence of relocations overlapping that patch.

The new `VJO_PLACE_READ_DELTARUNE` profile reader uses the existing bounded
scratch buffer. Before offering a string to its stream, it requires a dialogue
bullet (ASCII `*` or Japanese `＊`) after layout, expression and pause codes,
valid UTF-8, and Japanese text. Other copies leave the captured page intact.
The profile's filter removes those codes, the initial star and trailing message
terminators, and turns `&` into a line break. A real observer regression tests
that later paths, menus, English and malformed strings cannot erase the page.
No allocations, static buffers or threads are added. The game worker changes;
the kernel and Shell have no code changes.

This captures string copies, **not proof of the visible page**. It may see a
line before it appears or a preloaded line. It can miss unstarred dialogue.
Renderer entries were also inspected, but per-glyph drawing cannot establish a
complete dialogue page. This prototype therefore remains separate from release
support until tested against actual gameplay.

## Prepare offline

Use a private backup of the installed catalogue. The tool migrates the known
v0.10.1 definition revision to this checkout's revision, changing only revision
bytes 20..51 in existing profiles; their definition bodies and checksums stay
identical. It refuses any other source revision. **The resulting catalogue
requires the matching rebuilt game worker.** Old workers reject the new reader
and revision; a new worker rejects old-revision profiles.

```sh
python3 tools/deltarune_hook.py \
  --base /private/backup/profiles.vjhp \
  --eboot /private/backup/DELT00001/eboot.bin \
  --output /private/output/deltarune-profiles.vjhp
python3 -m unittest discover -s tools/tests -p test_deltarune_hook.py -v
```

The tool writes a new archive and a JSON receipt; it never connects to the Vita,
overwrites a file, replaces an existing Deltarune entry or includes game data.
The receipt lists each preserved definition body's hash and both revisions.
Host tests use Rubi's actual
external-profile parser, placement checks and filter interpreter, including
wrong-version, signature, corruption and unrelated-string refusals.

## Console verification still needed

Build and inspect the matching worker, back up its installed binary and the
catalogue, review both replacements, and obtain the owner's approval before
deployment as required by AGENTS.md. Test one change at a time. Use
`hook_trust = off` for the experiment: verified-profile sites are
otherwise trusted immediately. Launch the Japanese game and compare multiple
dialogue pages with the text in the debug log. Check typewriter prefixes,
speaker changes, choices, room changes and repeated lines. In particular, reject
this capture point if it keeps a preloaded or future line. Do not call it a
working dialogue hook until those checks pass. Restore both the backed-up worker
and catalogue, then restart the game, to remove the experiment.

Do not use remote Shell screenshots during these checks: the existing capture
runtime can retain a PAF JPEG allocation after capture. Physical screen checks
avoid changing that memory pressure.
