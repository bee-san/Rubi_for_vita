# The OCR worker in every enabled game: loaded by the kernel (#138)

test.3's tester could not use OCR in Adrenaline: a game whose title had no taiHEN line for the
game OCR worker (`*TITLEID` with `VitaJPOverlay_OCR.suprx`) gets `rc=-117`, shown as "OCR
worker is unavailable, register it for the game (install --title)". The owner's decision
(2026-10-16, issue #138) replaces the rule "the game worker loads per `*TITLEID`": the kernel
plugin loads the worker when a process starts and its title is ON in `games.ini`, never under
`*ALL`, never into SceShell or a system app (the PSP emulator, `NPXS10028`, and Adrenaline's
`PSPEMUCFW` count as games). This page is the analysis the plugin-safety rules ask for before
code: the prior art, the design, every point of the worker's life, its memory, and what only a
console run can show.

What the test.3 logs show (main's `tools/analyse_selftest.py --collect`, this boot's lines from
`log.txt` line 10630): four jobs ended `rc=-117`, all in AIR (`PCSG00940`), whose section in
`ur0:tai/config.txt` held only a commented line; CLANNAD (`*PCSG00415`, with the worker line)
registered its worker and ran every job. This boot's `log.txt` and `kernel.txt` have no
Adrenaline start (the `NPXS10028` and `PSPEMU005` starts in `log.txt` are older sessions'),
so the Adrenaline case is the same cause by inference: no `*PSPEMUCFW` line.

## Prior art (read from source, 2026-10-16)

Every plugin read that puts a user module into a process at its start does it in a hook on
`ksceKernelStartPreloadingModules`, after letting the original run; none loads from the process
event handler that the overlay's kernel already hooks.

| project (commit) | where | what it does |
|---|---|---|
| taiHEN (309b380) | `hen.c:360-388`, installed at `hen.c:454-460` | `start_preloaded_modules_patched`: `TAI_CONTINUE` first, then skips the process if L is held (`ksceCtrlPeekBufferPositive`, `SCE_CTRL_LTRIGGER \| SCE_CTRL_L1`, `hen.c:368-373`), reads the title with `ksceKernelGetProcessTitleId` and loads every config line of that section (`taiLoadPluginsForTitleForKernel` -> `plugin.c:165-175`: `ksceKernelLoadStartModuleForPid(pid, path, 0, NULL, 0, NULL, &result)`) |
| taiHEN (309b380) | `patches.c:389-418` | `hooks_add_hook`: the first hook on a function is the head of its chain, each later one is inserted right after the head. taiHEN hooks this function in `module_start` before it loads the `*KERNEL` plugins (`taihen.c:341-354`), so a kernel plugin's hook here runs inside taiHEN's `TAI_CONTINUE`: **before** taiHEN loads the `*TITLEID` plugins of that process |
| taiHEN (309b380) | `module.c:237-282` | `module_get_by_name_nid`, behind `taiGetModuleInfo`, walks the process's module list from its end (`for (i = count - 1 ...)`, the end being the main module), so a lookup by name finds the **oldest** module of that name |
| Adrenaline (7d382b7) | `kernel/main.c:123-136`, `200-203` | the same hook, 3.60 NIDs (`0xC445FA63`, `0x432DCC7A`), then 3.63+ (`0x92C9FFC2`, `0x998C7AE9`); after `TAI_CONTINUE`, `"main"` gets `adrenaline_vsh.suprx` and `"PSPEMUCFW"`, the PSP emulator's process, `adrenaline_user.suprx`; results unchecked |
| AdrBubbleBooter-oss (b603ca0) | `integration/adrenaline-v7/overlay/kernel/main.c:123-145`, `209-212` | the same hook and NIDs; a bubble's emulator process is matched by its own title ID (`PSPEMU*`, or a `boot.bin` it reads in the hook) |
| iTLS-Enso (52de568) | `kernel/main.c:27-46` | loads `old_dns_fix.suprx` into SceShell from a hook on the module loader, once SceShell has `libssl` |
| henkaku (784e483) | `plugin/kernel.c:236-242` | `taiLoadPluginsForTitleForKernel(shell_pid, "main")` for a SceShell that started before taiHEN |
| PSVshell (9a47b0e) | `src/main.c:144-207`, `348-358` | the process event handler (the overlay's hook) at start/exit/suspend/resume: classifies by title and by the `ScePspemu` main module; loads nothing. 3.60/3.65 NID pairs resolved with `module_get_export_func` |
| reVita (f35adc5) | `reVita/src/main.c:450-505` | the process event handler; `ksceSblACMgrIsPspEmu(pid)` tells the PSP emulator (`PSPEMUCFW` or an Adrenaline Bubble Manager bubble) from other apps; an `NPXS` allowlist; loads nothing |
| VitaTweaks NoTrophyMsg (4233c54) | `NoTrophyMsg/main.c` | a `*main` plugin that hooks SceShell by offset per SceShell module NID: no kernel loading; the other kernel plugins of the audit set ([plugin-audit.md](plugin-audit.md), 19 projects) load nothing into user processes either |

NIDs: `ksceKernelStartPreloadingModules` is `0x432DCC7A` in `SceModulemgrForKernel`
`0xC445FA63` on 0.931-3.61 and `0x998C7AE9` in `0x92C9FFC2` on 3.63-3.74 [wiki SceKernelModulemgr;
vita-headers `db/360`, `db/363`]. The calls the hook makes are in libraries whose NIDs did not
change: `ksceKernelSysrootGetProcessTitleId` `0xEC3124A3` (`SceSysrootForKernel` `0x3691DA45`,
which taiHEN, Adrenaline, PSVshell and reVita import on 3.60 and 3.65),
`ksceSblACMgrIsPspEmu` `0xFD00C72A` (`SceSblACMgrForDriver` `0x9AD8E213`, reVita) and
`ksceKernelLoadStartModuleForPid` `0x9D953C22` (`SceModulemgrForDriver` `0xD4A60A52`, Adrenaline,
iTLS-Enso). The kernel resolves all three at its start with taiHEN's `module_get_export_func`, as
`kernel/meminfo.c` does, so a firmware without one cannot stop the module from loading; without the
first and the last the loader stays off.

## Design

**The hook.** The kernel hooks `ksceKernelStartPreloadingModules` with the other hooks, with the
3.60 NIDs and then the 3.63+ ones, never in a safe boot. Unlike the display, input and process
event hooks it is optional: if it cannot be installed the others stay, the kernel logs it and the
per-title lines still work.

Not the process event hook the kernel already has (#138's spec names it as the obvious place): its
`startup` event is a foreground change that the kernel also sees for a process coming back to the
front (`kernel/foreground.c` treats `startup` and `resume` alike), it runs in the event's own
handler, and no plugin read loads a module from it. `ksceKernelStartPreloadingModules` runs once in
a process's life, when its own modules have started and before its code runs, which is where taiHEN
loads every `*TITLEID` line and Adrenaline its user module, with the same
`ksceKernelLoadStartModuleForPid` call: a worker loaded there is in the same state as one a line
loaded.

**At each process start**, after the original call (the process's preloaded modules are started),
in this order:

1. Nothing unless the Shell plugin has registered (SceShell's own start comes before that, and
   so does any process started before ux0 is final) and the process is not SceShell.
2. Its title ID (`ksceKernelSysrootGetProcessTitleId`). Nothing for a title that
   `vjo_game_is_candidate` refuses: SceShell (`main`), every `NPXS` system app but the PSP
   emulator's `NPXS10028`, VitaShell and the Overlay app (`VJOSET001`). These need no file and
   no log line.
3. Whether it is a PSP game: Sony's PSP emulator (`ksceSblACMgrIsPspEmu`) or a PSP ID's shape
   (`vjo_game_is_psp`: `NPXS10028`, `PSPEMU*`, a disc ID).
4. L held (taiHEN's own test, above): nothing. Holding L while an app starts still starts it
   with none of our plugins, as before.
5. Its policy, by the Shell's rules (`vjo_games_enabled_as`): its own row, for a PSP game the
   shared row `NPXS10028`, then the default. `games.ini` is read for every start, as the Shell
   reads it: the file, else `games.ini.bak`; neither present is the defaults; a file that cannot
   be read or parsed loads nothing (the Shell fails closed the same way). The read is done by the
   kernel's worker thread, which the hook waits for (at most 500 ms): the worker already opens
   every ux0 file games need, because some games cannot open ux0 themselves (`0x80010013`), and
   it is not known in which process's context this hook runs. The worker answers with the title's
   verdict both as a game and as a PSP game; the hook takes the one that applies.
6. ON: `ksceKernelLoadStartModuleForPid(pid, "ur0:tai/VitaJPOverlay_OCR.suprx", 0, NULL, 0, NULL,
   &status)`, as taiHEN loads a config line; `ux0:tai/` only when the ur0 file is missing.

One `kernel.txt` line per candidate start says what was decided and why (`worker for PCSG00940
(pid 0x...): ON in games.ini: loaded ...`; OFF, unreadable, L held, no file, the load's error).
The process's start is never changed: the hook returns the original call's result whatever
happens.

**One worker per process.** The per-title line stays harmless for one release (the installer
removes it after this change). taiHEN loads it right after the kernel's copy (the chain order
above), so a process can get two. The worker's `module_start` keeps only the oldest copy: it looks
its module up by name (`taiGetModuleInfo`, which finds the oldest of that name) and returns
`SCE_KERNEL_START_NO_RESIDENT` when none of that module's segments (`sceKernelGetModuleInfo`) holds
its own code, so a younger copy gives its pages back at once, whichever loader came first. A lookup
that fails or reports no segment counts as "no other copy", so the only copy never unloads itself.
That the lookup finds the oldest is read from taiHEN's source, not seen on a console; were it the
newest, both copies would stay. The pairing itself, `taiGetModuleInfo`'s `modid` handed to
`sceKernelGetModuleInfo` for the module's segments, is Adrenaline's user module's for the PSP
emulator (`user/main.c:530-548`: `segments[0].vaddr`/`memsz` its text). The kernel is the second line of defence either way: one registered
worker thread per process; another thread's `vjoOcrRegister` in the same process is refused
(`VJO_ERR_BUSY`, never a job, never weights) and logged once, so a console run shows a second copy
that stayed.

## Lifecycle

| event | what happens |
|---|---|
| a title ON in `games.ini` starts (by its own row, the PSP row or the default) | the kernel loads the worker into it; the worker's `module_start` checks its title and the boot mode as before, starts one idle thread at the lowest user priority, and registers once the Shell confirms the game (`vjoSetGameActive`) |
| a title OFF starts | nothing is loaded; the Shell leaves it alone as before |
| a system app, SceShell, VitaShell, the Overlay app | nothing, before any file is read |
| L held at the start | nothing (one line); the app runs without our worker |
| `games.ini` unreadable or corrupt | nothing (one line); the Shell disables the game too |
| the kernel's worker does not answer within 500 ms, or the 24 KiB read block is refused | nothing (one line); the game starts without the worker |
| the module file is missing, or the load fails (e.g. the game's memory) | nothing (one line with the error); the game starts without the worker |
| suspend (PS, a system app in front) | the hook does not run; the worker stays and keeps its weights; after 5 s of the game's running time without being the active game it gives them back, as before (`game_ocr/main.c` `INACTIVE_DROP_US`; the time stops while the game is suspended) |
| resume | the hook does not run; the Shell reads `games.ini` again and confirms the game, the worker registers again |
| the title turned OFF while it runs | its worker stays loaded (nothing unloads a module from a running game), idle: the Shell disables the game at its next resume, the worker gives its weights back 5 s later; at the next start nothing is loaded |
| the title turned ON while it runs (it started OFF) | no worker until the game starts again: the overlay says so in plain words: "OCR is not running in this game. Close the game and start it again." |
| the Overlay app beside the game (a system app) | not a candidate: nothing is loaded into it; what it saves takes effect at the next start (the worker) or resume (the Shell) |
| exit | the process's modules go with it, ours included, exactly as a module taiHEN loaded; the kernel's OCR transport forgets the process (`game_ocr_process_gone`). The loader keeps nothing per process |
| crash | as exit. A crash in the worker's own `module_start` ends that game's start, as with a `*TITLEID` line today; the next start of the same title loads it again (turn the title OFF, or hold L, to start it without) |
| Adrenaline | the emulator's process: `PSPEMUCFW` (Adrenaline's kernel keys `adrenaline_user.suprx` on it) or, from a bubble, the bubble's ID (AdrBubbleBooter); `NPXS10028` if the kernel sees the emulator's own ID. Each is a candidate and a PSP game: its own row, then the PSP row, then the default. A bubble's starter app (an ordinary process under the bubble's ID) also gets an idle worker while it runs, as its `*ID` line gave it |
| boot | SceShell starts before the Shell plugin registers: nothing. A safe boot installs no hooks, this one included, and the worker unloads itself anyway (`safe_boot`) |
| the kernel plugin stops | the loader's hook is released first, while the kernel worker still answers; a start still inside it is waited for (at most 3 s, else the stop fails and nothing of ours goes from under it) |

What the kernel sees as the PSP emulator's title is inferred from Adrenaline's and
AdrBubbleBooter's sources (their kernels key on it at the same hook); the Shell saw `NPXS10028`
from user space for every Adrenaline start, and keys its policy on AppMgr's name for the app
(`PSPEMUCFW`, the bubble's ID). The two agree when the kernel sees `PSPEMUCFW` or the bubble's ID.
Should it see `NPXS10028` instead, the kernel decides by the shared PSP row and the default only:
a bubble OFF by its own row would get an idle worker (the Shell still keeps the overlay off), and
one ON by its own row with the shared row OFF would get none. The `kernel.txt` line names the title
the kernel saw, so test.4 settles it.

Turning the default OFF (Triangle in the Overlay app: selected games only) keeps the worker out of
every title without a row of its own.

## Memory

- **Game partition.** Unchanged per process: the worker's module pages (90,112 B) and its idle
  thread (32,768 B stack + 4,096 B TLS) until a job, then the jobs' blocks as before
  ([memory-ledger.md](memory-ledger.md)). What changes is where: every title ON in `games.ini`,
  not only those with a line. With `default = enabled`, the installer's default, that is every
  app but SceShell, the system apps, VitaShell and the Overlay app: homebrew too. The expert's
  warning applies: homebrew often takes nearly all the memory it may (VitaSDK's newlib heap is a
  fixed 128 MiB unless the app sets `_newlib_heap_size_user` [measured: vitasdk 2026.08
  `libc.a`, `_init_vita_heap`]), so an app with a fixed heap near its limit may fail to start
  with these 126,976 B taken first. Such an app is turned OFF in the Overlay app (nothing is
  loaded at its next start) or started with L held. A default for retail titles only (by the
  `param.sfo` CONTENT_ID) was considered and not taken: the Overlay app shows such a title ON,
  and an ON title without OCR is what test.3 reported; it would also add a `param.sfo` read to
  every start, on a heuristic. It is the owner's call if a homebrew fails.
- **SceShell and the system apps.** 0 B: the kernel never loads the worker there (the candidate
  test), and a copy a hand-edited `*ALL` line loads still unloads itself there
  (`VJO_BUDGET_WORKER_IN_SHELL_PAGES` stays 0).
- **Kernel.** The loader's code and 48 B of state (and 8 B for the registration's one-thread check)
  in the module's pages: one text page more in both builds, 118,784 B in the debug build (40,648 B
  of text, 77,824 B of data/bss, whose pages it fills) and 114,688 B in the normal one [measured:
  the build log's module rows, main 3aa08a6 and #138; `docs/memory-ledger.md`]. Per candidate start
  one transient `KERNEL_RW` block of 24,576 B (`VjoPolicy`: the 12,288 B text limit of `games.ini`
  and its parsed table, 8,200 B), taken by the kernel worker and freed before the hook goes on:
  `VJO_BUDGET_KERNEL_POLICY` in the ledger. No copy of the policy stays in the kernel.
- **No second copy.** The younger copy's `NO_RESIDENT` gives its pages back inside its own load.

## Time and CPU3

A candidate start costs one `games.ini` read on the kernel's worker thread (an open, one read of at
most 12 KiB, a close) and the module's load, which taiHEN made for a `*TITLEID` line before: both
unmeasured. No polling, no thread and no hook on a hot path is added; the hook runs once per process
start. CPU3: unmeasured (AGENTS.md rule 8).

## Recovery

Nothing here runs at boot before SceShell, and nothing runs in a safe boot. A game that will not
start with the worker: turn it OFF in the Overlay app, or hold L while starting it. If the Vita
does not boot: hold L at power-on (taiHEN loads no plugins, our kernel included), as before
([boot-safety.md](boot-safety.md)).

## What only the console can show (test.4)

- `kernel.txt`: `hooks: ... worker loader 0` and, per game start, a `worker for` line; AIR
  (`PCSG00940`, no line of its own) loaded and registered; Adrenaline (`PSPEMUCFW` or what the
  kernel sees) loaded, registered and recognizing; CLANNAD with its old `*PCSG00415` line:
  one registration and no `second worker thread` line.
- A title turned OFF: no `loaded`; L held at a start: the `L held` line.
- The extra start time of a game with the worker (unmeasured).
