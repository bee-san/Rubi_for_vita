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
card is one scenario: 24 of them, 591 events, from the collection's `current-device/rubi-log.txt` (gist
`5e67276`). An event is:

| Event | From the log | What the replay runs |
| --- | --- | --- |
| `boot`, `start`, `exit` | the ledger's `ledger boot/start/exit paf: current=… free=…` lines | at a start or an exit no game is active, so the worker gives its result arena back (`shell/worker.c` `on_game_exit` and the control thread's idle rule) |
| `press` | `overlay text arena Paf reserve guard: free=…` | the overlay opens (`shell/overlay_mem.h`, as `overlay.cpp`'s `open_page` takes its text arena), the cursor's word is looked up (`shell/worker.c` `mem_alloc`), and the overlay closes |
| `lookup` | a `result arenas Paf reserve guard` line that is not a press's | a background read's lookup: `run_job` gives the arena back, then asks for one after OCR |
| `anki` | `anki: save rc=0` | a press that saved a card: the Anki arena (`shell/anki.c`'s request, `VJO_BUDGET_PAF_ANKI`) while the overlay is open |

A press's lookup is the `result arenas` refusal within three lines of it at the same free size, when there is one.
Every request goes through the Shell's own guard (`shell/paf_guard.h`, which `shell/memory.cpp` asks before every
Paf allocation), against a model of the heap: 5,242,880 B (`VJO_PAF_HEAP`), of which others held what the log
showed was neither free nor Rubi's (the heap less `free=` less Rubi's `current=`, or the plugin's 8 KiB at a
refusal) until the next event. A build that holds less than 0.10.1 did leaves more free beside the same others.
The overlay page's own Paf, which `PageOpen` takes for its widgets outside Rubi's ledger, is 103,296 B while it is
open: the most of the 81 opens in the owner's logs where Rubi allocated again before the page closed (median
94,800 B) [measured, log]. With less free, `PageOpen` fails (`overlay page failed to open`) and the press counts as
not opened.

Each scenario prints a line, `owner-scenario shell NAME: …`:

- `presses`, `not_opened`, `page_failed`: toggles, those whose overlay did not open (the user sees nothing), and of
  these the ones whose page itself had no room;
- `lookups`, `refused`, `told`: result arena requests, the refused ones, and the refused ones in an open overlay,
  which shows why (`OOM_SHELL_RESULTS`);
- `anki_ok`, `anki_refused`: saved cards;
- `peak`, `held`: the most Paf Rubi held, and the most it held between jobs (at game starts and exits).

**The game's USER memory** (ctest `alloc_sim`, test `owner scenario`, with the release models). The owner's job as
the Shell asks for it with his `config.ini` (Large, `ocr_lm = 1000k`, `ocr_cascade_ms = 2000`, `ocr_threads = 3`,
the spotter and the other defaults) runs through the real worker and kernel (`game_ocr/main.c`, `kernel/`) on a
textbox and on the full screen, at the free USER sizes his games logged and at smaller ones. Each case prints a
line, `owner-scenario game LEVEL-REGION: …`: `rc`, `failed`, the `tier`, `lm` tier and `cascade` the memory check
chose, `spot` (the spotter found the lines), `helpers`, `peak` (the USER bytes of Rubi's blocks) and `told` (it read
text, or its refusal carries the memory check's figures, which the Shell shows and logs).

## The model's limits

- The normal build logs refusals, the ledger's lines and saved cards, not a press that opened the overlay: those
  are not in the events. The replay covers where 0.10.1 failed, the game starts and exits and the saved cards,
  not every press.
- Others' use is replayed as the log shows it; it does not react to Rubi's. The ~2.2 MB steps between one game's
  exit and the next start (#528: consistent with the device agent's 2,187,264 B JPEG buffer) are such use.
- The game's free USER was logged by the debug builds only. The native ports the owner ran under 0.10.1 (normal
  build) logged none: their levels are estimates. A helper thread's stack (16 KiB and its TLS) is not in `peak`:
  threads take it from the game's budget outside the worker's memory blocks. The frame is the simulator's
  three-line text box.

## Rubi 0.10.1's numbers

The replay of 0.10.1's code (main `1845471b`, whose Shell and game memory code is 0.10.1's) gives the log's own
counts: 24 overlays not opened and 28 result arenas refused in the API 57 boots, 10 and 23 in API 31's.

| Shell, by boot | Presses (cards) | Not opened | Lookups | Refused | Told | Peak B |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| API 31 (line 1735) | 10 (0) | 10 | 23 | 23 | 0 | 8,192 |
| API 57 (line 11156) | 11 (6) | 5 | 17 | 15 | 0 | 225,280 |
| API 57 (line 11385) | 10 (0) | 10 | 12 | 12 | 0 | 8,192 |
| API 57 (line 11446) | 1 (1) | 0 | 2 | 1 | 0 | 225,280 |
| API 57 (line 11563) | 9 (0) | 9 | 0 | 0 | 0 | 8,192 |
| all 24 scenarios | 273 (239) | 34 | 124 | 51 | 0 | 225,280 |

Rubi held the plugin's 8,192 B at every start and exit in every scenario, as the log's ledger lines show.

| Game, free USER | Tier | LM | Spotter | Helpers | Peak B (textbox) |
| --- | --- | --- | --- | ---: | ---: |
| 88,080,384 (CLANNAD), 29,360,128 (Adrenaline), 12,664,832 and 10,485,760 (PCSG00402) [measured]; 8 and 6 MiB [estimate] | Large | 1000k | yes | 2 | 4,648,960 |
| 5 MiB [estimate] | Large | 1000k | yes | 0 | 3,915,776 |
| 4 MiB [estimate] | Large | off | yes | 0 | 2,871,296 |
| 3 MiB [estimate] | Normal | 500k | no | 0 | 2,011,136 |
| 2.5 MiB [estimate] | Normal | off | yes | 0 | 1,560,576 |
| 2 MiB [estimate] | Small | off | no | 0 | 1,015,808 |
| 1.5 MiB [estimate] | refused, with the check's figures | | | | 61,440 |

The full screen takes 4,096 B more at every level that reads (12,288 B at 2.5 MiB).

## Running it

```sh
ctest --test-dir build/host -R 'owner_scenario|shell_worker_paf|alloc_sim'
python3 tools/owner_scenario.py --build build/host          # the table, and the 0.10.1 column
python3 tools/owner_scenario.py --build build/host --check  # what ctest owner_scenario runs
```

Without the release models (`VJO_VOCR_MODEL_DIR`) the game part is left out and says so; the fast gate's models
lane runs it. `tools/owner_scenario.json` holds the 0.10.1 column (`before`) and the limits: each scenario's
counts and peaks may only go down, and a game case's `told`, `tier` and `lm` only up. A change that lowers one
runs `--lower` in the same pull request, which writes the new limits (it never loosens one), and pastes the table
in its body. Raising a limit is a deliberate edit the pull request explains, as raising a memory budget is
([memory-ledger.md](memory-ledger.md)).

A change to the replay's model (what the console does that the replay did not yet model, such as how it reports free
memory) records its part's baseline again with 0.10.1's code for that part, in a commit before the fix:
`--set-before "LABEL" --part game` (or `shell`) replaces that part's 0.10.1 column and limits and leaves the other's.

New console logs: `python3 tools/owner_scenario.py --extract LOG... --source "where they came from"` rewrites the
events, then `--lower` adds the new scenarios' limits.
