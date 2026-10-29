# The owner scenario (#528)

The owner's PCH-1001 (firmware 3.65, Rubi 0.10.1, API 57) logged Rubi's memory failures in `log.txt`
([#528](https://github.com/bee-san/Rubi_for_vita/issues/528)): the overlay that did not open (`overlay not opened:
no Paf memory for its text (20 KiB)`), the lookups whose result arena the Paf reserve guard refused, and the Paf
heap's free size at each of them and at every game start and exit. The owner scenario replays those conditions on
the host through Rubi's own code, counts what fails, and holds the counts with a ratchet, so a change that makes
them worse fails the build. It is the acceptance test of the memory work for #528.

It is host evidence only. It says what Rubi's code does under the conditions the console logged; it does not say
how much memory SceShell or a game has (AGENTS.md rule 5). Every console number below says where it was logged.

## What it replays

**The Shell's Paf heap** (ctest `shell_worker_paf`, test `owner scenario`, in `host/tests/owner_scenario.h`). The
events are in `tools/owner_scenario.txt`, made from the owner's `log.txt` by `tools/owner_scenario.py --extract`:
numbers only, no game text and no title IDs. Each boot of a build since API 31 that logged a refusal or a saved
card is one scenario, named by its API and first line (`api57-line11385`): 25 of them, 766 events, from the
collection's `current-device/rubi-log.txt` (gist `5e67276`). An event is:

| Event | From the log | What the replay runs |
| --- | --- | --- |
| `boot`, `start`, `exit` | the ledger's `ledger boot/start/exit paf: current=… free=…` lines | at `boot` the Shell's start step: the overlay's text block, taken and kept from then on (`shell/overlay_mem.h` `vjo_overlay_mem_boot`, since #534; 0.10.1 took nothing there); at an exit `on_game_exit`'s step, and at a start or an exit, with no game in front, the control thread's idle rule (`shell/worker.c` `RESULT_MEM_GAME_EXIT`, `RESULT_MEM_IDLE`) |
| `press` | `overlay text arena Paf reserve guard: free=…` (0.10.1), `overlay page Paf reserve guard: free=…` (since #537) | the overlay opens (`shell/overlay_mem.h`, as `overlay.cpp`'s `open_page`), the cursor's word is looked up (`shell/worker.c` `mem_alloc`; when the log has the press's own job, its start and run steps first: `RESULT_MEM_JOB_START`, `RESULT_MEM_JOB_RUN`), and the overlay closes |
| `lookup` | a `result arenas` refusal (the guard's line, or the normal build's `Paf allocation refused`) that is not a press's | a background read's lookup: its job's start and run steps, then the arena after OCR |
| `anki` | `anki: save … rc=0` (with the word in the debug build), or an `Anki arena` refusal | a press that saved a card: the Anki arena (`shell/anki.c`'s request, `VJO_BUDGET_PAF_ANKI`) while the overlay is open |

A press's own job's lookup is the `result arenas` refusal at the same free size up to three lines before it, else the
first one at the same free size after it, before the next press, ledger line or (debug build) `job start`. Line
history's refusals are counted by `--extract` but not replayed; the visits that took its 16 KiB block are not logged
in the normal build (the API 33 debug boot at line 4403 reached the ledger's 241,664 B worst case with one: the
replay's peak there is 184,320 B since the Anki arena's bound, #548; 225,280 B with the 128 KiB arena).
Every request goes through the Shell's own guard (`shell/paf_guard.h`, which `shell/memory.cpp` asks before every
Paf allocation), against a model of the heap: 5,242,880 B (`VJO_PAF_HEAP`), of which others held what the log
showed was neither free nor Rubi's (the heap less `free=` less Rubi's `current=`; at a refusal, Rubi's figure from the
boot's last ledger line: 8,192 B in 0.10.1, 28,672 B since #534 kept the text block) until the next event. A build that
holds less than 0.10.1 did leaves more free beside the same others.
The overlay page's own Paf, which `PageOpen` takes for its widgets outside Rubi's ledger, is 103,296 B while it is
open: the most of the 81 opens in the owner's logs where Rubi allocated again before the page closed (81,152 to
103,296 B, median 94,928 B; 94,800 B over the 67 whose next block was a result arena; the largest followed a line
history visit, whose view may add widgets) [measured, log]. With less free, `PageOpen` fails (`overlay page failed to open`) and the press counts as not opened; since #528's
follow-up the page check before it (`shell/paf_guard.h` `vjo_paf_page_fits`: 103,296 B and SceShell's 64 KiB margin
free, read through the scenario's own `vjo_shell_heap_free_size`) refuses first, and `page_failed` counts both.
The result arena's lifetime is the worker's own: its `RESULT_MEM_*` steps (`shell/worker.c`) run in the replay where
the worker runs them, so a change of when the arena is held changes the table.

Each scenario prints a line, `owner-scenario shell NAME: …`:

- `presses`, `not_opened`, `page_failed`: toggles, those whose overlay did not open (the user sees nothing), and of
  these the ones whose page itself had no room;
- `lookups`, `refused`, `told`, `silent`: result arena requests, the refused ones, the refused ones in an open
  overlay, which shows why (`OOM_SHELL_RESULTS`), and the rest (`refused - told`: no open overlay showed them);
- `anki_ok`, `anki_refused`: saved cards;
- `peak`, `held`: the most Paf Rubi held, and the most it held between jobs (at game starts and exits).

**The game's USER memory** (ctest `alloc_sim`, test `owner scenario`, with the release models). The owner's job as
the Shell asks for it with his `config.ini` (Large, `ocr_lm = 1000k`, `ocr_cascade_ms = 2000`, `ocr_threads = 3`,
the spotter and the other defaults) runs through the real worker and kernel (`game_ocr/main.c`, `kernel/`) on a
textbox and on the full screen, at the free USER sizes his games logged and at smaller ones. The game's free memory
is reported as the console reports it: in whole MiB, rounded down (#324: all 78 samples of あおかな were 1-4 MiB; every
`free USER=` the owner's debug builds logged is a whole MiB). Each level runs the job twice: cold (nothing of Rubi's
resident) and warm (`LEVEL-REGION-warm`: beside the weights, LM and spotter the cold job left, as every later job of
a session runs). `tier-SIZE-88MiB` is the owner's config with `vocr_tier` set to each size, at CLANNAD's measured
88,080,384 B, on the textbox, cold. Each case prints a line, `owner-scenario game NAME: …`: `rc`, `failed`, the
`tier`, `lm` tier and `cascade` the memory check chose, `spot` (the spotter found the lines), `helpers`, `lines`,
`peak` (the USER bytes of Rubi's blocks, a warm job's resident ones included) and `told` (it read text, or its
refusal carries the memory check's figures, which the Shell shows and logs). The request is
`host/tests/owner_request.h`'s, which `test_shell_worker.c`'s `owner scenario request` holds against the Shell's own. `spot` and `helpers` come from the job's whole stats, which the host build of the worker keeps (`host_last_stats`): a cold job's 20-entry step log is full before its spotter and helper steps (Mmem-game-7).

## The model's limits

- The normal build logs refusals, the ledger's lines and saved cards, not a press that opened the overlay: those
  are not in the events. The replay covers where 0.10.1 failed, the game starts and exits and the saved cards,
  not every press.
- Others' use is replayed as the log shows it; it does not react to Rubi's. The ~2.2 MB steps between one game's
  exit and the next start (#528: consistent with the device agent's JPEG buffer, 2,187,264 B in its source [source]) are such use.
- The game's free USER was logged by the debug builds only. The native ports the owner ran under 0.10.1 (normal
  build) logged none: their levels are estimates. A helper thread's stack (16 KiB and its TLS) is not in `peak`:
  threads take it from the game's budget outside the worker's memory blocks. The frame is the simulator's
  three-line text box.

## Rubi 0.10.1's numbers

The replay of 0.10.1's Shell code (the overlay's text arena taken at each open and given back at each close, as
before #534) gives the log's own counts: 24 overlays not opened and 28 result arenas refused in the API 57 boots, 10
and 23 in API 31's. `tools/owner_scenario.json` keeps the column (`before.shell`, from a build of this harness with
0.10.1's `shell/overlay_mem.h` steps).

| Shell, by boot | Presses (cards) | Not opened | Refused | Told | Silent | Peak B |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| API 31 (line 1735) | 10 (0) | 10 | 23 | 0 | 23 | 8,192 |
| API 57 (line 11156) | 11 (6) | 5 | 15 | 0 | 15 | 225,280 |
| API 57 (line 11385) | 10 (0) | 10 | 12 | 0 | 12 | 8,192 |
| API 57 (line 11446) | 1 (1) | 0 | 1 | 0 | 1 | 225,280 |
| API 57 (line 11563) | 9 (0) | 9 | 0 | 0 | 0 | 8,192 |
| all 25 scenarios | 274 (240) | 34 | 51 | 0 | 51 | 225,280 |

In the log, Rubi's ledger shows the plugin's 8,192 B alone at 828 of its 836 game starts and exits; the other 8, none
in a boot with a pressure step, held the text arena or a result arena for a moment (28,672 or 73,728 B). The replay
holds 8,192 B at every start and exit of 0.10.1's code. Since #534 keeps the text block from the Shell's start, Rubi
holds 28,672 B there, and `held` and, in the three scenarios that took nothing else, `peak` are 28,672 B: the
ratchet's two deliberate raises.

The game part as recorded with whole-MiB reports and a warm job (#528, mem-game, `--reset-limits`; main `cb1449d2`,
whose game memory code gives the same 51 rows as 0.10.1's at main `1d698559`), textbox (the full screen takes 4,096 B
more at every level that reads):

| Game, free USER | Cold: tier, LM, spotter, helpers | Peak B | Warm: tier, LM, spotter, helpers | Peak B |
| --- | --- | ---: | --- | ---: |
| 88,080,384 (CLANNAD), 29,360,128 (Adrenaline) and 10,485,760 (PCSG00402) [measured]; 12,664,832 (PCSG00402: the check's figure, the free figure plus Rubi's blocks it counts as released, [measured] in the normal build's "tier large did not fit" lines); 8 MiB [estimate] | Large, 1000k, yes, 2 | 4,648,960 | the same | 4,648,960 |
| 6 MiB [estimate] | Large, 1000k, yes, 0 | 3,915,776 | Large, 1000k, yes, 1 | 4,317,184 |
| 5 MiB [estimate] | Large, off, yes, 0 | 2,871,296 | Large, 500k, yes, 0 | 3,436,544 |
| 4 MiB [estimate] | Normal, 500k, no, 0 | 2,125,824 | Normal, 1000k, yes, 0 | 2,605,056 |
| 3 MiB [estimate] | Small, off, no, 0 | 1,015,808 | the same | 1,015,808 |
| 2.5 MiB [estimate] | Small, off, no, 0 | 1,015,808 | refused, with the check's figures | 823,296 |
| 2 MiB [estimate] | refused, with the check's figures | 61,440 | refused | 61,440 |
| 1.5 MiB [estimate] | refused | 0 | refused | 0 |

Each size at CLANNAD's 88,080,384 B (textbox, cold): Small with the 1000k LM and Normal's model for the cascade,
4,128,768 B; Normal with the 1000k LM and Large's for the cascade, 5,980,160 B; Large with the 1000k LM, the spotter
and two helpers (Large has no cascade), 4,648,960 B.

Since the spotter's part runs over the job's extras (#528, Mmem-regress-2), Small's job there peaks at 4,014,080 B and
Normal's at 5,922,816 B (114,688 and 57,344 B less, the same text: host/tests/test_alloc_sim.c
`spot_overlap_reads_the_same_text`), and at 3 MiB the job keeps the spotter beside Normal and the 500k LM (61,440 B
more at the textbox, the host's spotter block, and 65,536 B at the full screen, where its part also takes a workspace
page beside the LM's extras; it finds the frame's three lines, where GRAD's full-screen job found a fourth box,
928 × 26, across the text box). Small and Normal run the spotter and both helpers at 88 MiB too: the table's `spot`
and `helpers` read the job's step log, whose 20 entries (`VJO_GAME_OCR_STEPS`) a cold job with the LM and the cascade
fills before those steps, so they print 0 there [measured: the same runs with a 48-entry log].

With exact figures (the model before #528's whole-MiB reports) 3 MiB read Normal with the 500k LM, 2.5 MiB Normal
and 2 MiB Small: on a console those games got a smaller model, or none (main `b024c93b`'s table).

## Running it

```sh
ctest --test-dir build/host -R 'owner_scenario|shell_worker_paf|alloc_sim'
python3 tools/owner_scenario.py --build build/host          # the table, and the 0.10.1 column
python3 tools/owner_scenario.py --build build/host --check  # what ctest owner_scenario runs
```

Without the release models (`VJO_VOCR_MODEL_DIR`) the game part is left out and says so; the fast gate's models
lane runs it, and there a game part that prints no rows fails (`--require-game`). `tools/owner_scenario.json` holds
the 0.10.1 columns (`before`, with each part's label) and the limits: a Shell scenario's `not_opened`, `silent`,
`anki_refused`, `peak` and `held`, and a game case's `failed` and `peak`, may only go down, and a game case's `told`,
`tier`, `lm`, `spot`, `helpers` and `lines` only up; a counter the limits hold that a run does not print fails. A
change that lowers one
runs `--lower` in the same pull request, which writes the new limits (it never loosens one), and pastes the table
in its body. Raising a limit is a deliberate edit the pull request explains, as raising a memory budget is
([memory-ledger.md](memory-ledger.md)).

A change to the replay's model (what the console does that the replay did not yet model, such as how it reports free
memory) records its part's baseline again with 0.10.1's code for that part, in a commit before the fix:
`--set-before "LABEL" --part game --reset-limits` (or `shell`) replaces that part's 0.10.1 column and limits, prints
every limit that rises, and leaves the other part's. Without `--reset-limits` it keeps the tighter of each old limit
and the run; with no rows for the part (the game part without the release models) it records nothing.

New console logs: `python3 tools/owner_scenario.py --extract LOG... --source "where they came from"` rewrites the
events, then `--lower` adds the new scenarios' limits.
