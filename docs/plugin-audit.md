# taiHEN plugin audit: how other plugins stay safe, and where the overlay departs

Date: 2026-10-13. Overlay commit audited: `599dd9c` (main; the same code as release
`e759187`, API 16). Nineteen projects from the plugin-safety skill's audit set were
cloned and read from source, end to end, by four agents; the per-project notes with
`path:line` citations are in [docs/plugin-audit/notes/](plugin-audit/notes/). Segment
sizes of the overlay's three modules were measured with `readelf -lW` on a build of
this commit; everything else is read from source. Nothing here was run on a console,
and no CPU3 number is claimed (see [What this audit cannot settle](#what-this-audit-cannot-settle)).

Motivation: the owner's console failed to boot twice after game launches with the
API 16 build (issue #59: SceShell thread `SceLiveAreaDailyCronJob` dies in ScePaf with
`0x80024302 SCE_KERNEL_ERROR_NO_FREE_PHYSICAL_PAGE`), and a Vita plugin developer's
guidance is that a `*main` plugin competes with SceShell for well under 2 MiB, that
heavy work belongs in the game's process, and that `*ALL` should be avoided in favour
of `*TITLEID`.

## The three questions

**Does anything still allocate in SceShell?** Yes. Permanently, from the moment SceShell
starts: the Shell module image, 221,984 B of code and 178,628 B of data/bss
(measured), two thread stacks of 65,536 B (`VjoNet`) and 16,384 B (`VjoControl`)
(`shell/worker.c:1731-1732`), the Paf plugin and RCO loaded by `Plugin::LoadSync`
(`shell/main.cpp:18-26`), four mutexes and an event flag, and, because `*ALL` also
matches SceShell, the 35,872 + 4,480 B image of `VitaJPOverlay_OCR.suprx`
(`tools/patch_tai_config.py:48`; taiHEN parser `parser.c:242`). While a game runs with a
network dictionary: 2 x 262,144 B result arenas plus two TLS slots from SceShell's Paf
heap (`shell/worker.c:25-31, 337`, `shell/memory.cpp:39`), allocated at game start
(`shell/worker.c:1506-1507`) and kept until the game exits. With the local pipeline:
one 65,536 B Paf arena per lookup session (`shell/worker.c:26, 337`). Per Anki job:
131,072 or 393,216 B from the Paf heap plus a 65,536 B thread stack
(`shell/anki.c:39-44, 147, 708`). While the overlay or the strip is open: Paf UI pages
(`shell/overlay.cpp:507`, `shell/strip.cpp:278`), unsized in source. OCR weights,
workspace and the capture buffer are no longer in SceShell (they are in the game's
`USER_RW` and the kernel's `KERNEL_RW`), but the dictionary, UI, Anki, networking and
all job control still are.

**Is the game worker loaded through `*TITLEID` or `*ALL`?** `*ALL`
(`tools/patch_tai_config.py:48`, README "Install"). taiHEN loads an `*ALL` entry into
every user process whose section is not `KERNEL`, which includes SceShell (`main`), every
`NPXS` system app, VitaShell and the settings app (taiHEN `hen.c:382-386`,
taihen-parser `parser.c:242-245`). The worker's `module_start` declines to start its
thread outside candidate titles (`game_ocr/main.c:383-392`, `core/games.c:15-21`) but
always returns `SCE_KERNEL_START_SUCCESS` (`game_ocr/main.c:395`), so the 39.4 KiB image
stays resident in every process, and every candidate game gets a 32 KiB polling thread
(`game_ocr/main.c:8, 345`) before the Shell has said whether that game is enabled. No
plugin in the reference set runs a thread in a game it does nothing for; the only other
`*ALL` plugin read (VitaGrafix) allocates nothing and creates no thread. (Status
2026-10-13: the installer now registers the worker per game; see departure 2.)

**Is every hook released on a partial failure?** Kernel: no. `module_start` installs the
display hook, the 16 input hooks and the process-event hook and only logs their results
(`kernel/main.c:479-480`); `input_hooks_install` keeps going after a failed install and
returns `-1` (`kernel/input.c:486-504`), which the caller ignores. The hooks that did
install stay installed and the plugin reports success. Every hook is released when the
worker thread cannot be created (`kernel/main.c:484-492`) and in `module_stop`
(`kernel/main.c:497-515`). Shell: one hook; if `vjo_worker_start` fails inside it the hook
releases itself but the Paf plugin stays loaded and the module stays resident
(`shell/boot.c:21-30`). Game worker: no hooks. The reference pattern is taiHEN's own
`hen_add_patches` (`hen.c:395-528`): a zeroed uid array, a check after every install, and
one `fail:` label that releases everything installed so far and makes `module_start`
return `SCE_KERNEL_START_FAILED`. Most plugins in the set do not reach that bar either
(PSVshell, ds4vita, nonpdrm, NoPsmDrm, pngshot and all four VitaTweaks never check an
install), but iTLS-Enso (`kernel/main.c:52-55`), the gpues4 profiler (`kernel.c:557-606`)
and taiHEN do.

## Per-project table

Columns: where the plugin runs; whether `module_start` fails closed (returns FAILED or
NO_RESIDENT when a prerequisite is missing); what happens when hook k of n fails; whether
`module_stop` undoes everything; threads (count, stack bytes, period); what the plugin
keeps resident and from which pool; what it ever puts in SceShell or in a game process;
whether its boot path does file I/O. Full detail, with citations, is in the linked note.

| project (note) | section / process | start fails closed | partial-failure unwind | stop complete | threads | resident memory, pool | in SceShell / in game | boot-path I/O, logging |
|---|---|---|---|---|---|---|---|---|
| [taiHEN](plugin-audit/notes/taiHEN.md) (framework) | kernel; loads `*KERNEL` at boot and every other section into the started process | yes: every init step and `hen_add_patches` -> START_FAILED (`taihen.c:322-346`) | reference pattern: zeroed uid array, check after each of 11 hooks, `fail:` releases all (`hen.c:395-528`) | best effort ("TODO: release everything") | none | 64 KiB patch heap + map heap + config block; one 4 KiB `USER_RX` slab page in every process that has a user hook (`patches.c:98`, `slab.c:60-116`) | one slab page per hooked user process | config.txt read at boot (`plugin.c:70-144`); `LOG` compiled out |
| [henkaku](plugin-audit/notes/henkaku.md) (plugin/) | kernel (`*KERNEL`) + SceShell (`*main`, also `*NPXS10015`) | no: always SUCCESS; config read falls back to defaults | kernel: unchecked, released unguarded; user: `-1` pre-set, `default:` skips unknown SceShell (`user.c:450, 508-510`) | user yes (guarded); 8 lazy settings hooks released in the unload hook (`user.c:399-412`) | one 4 KiB kernel thread per config reload, joined at once | 24 B config + uid arrays; XML/language tables in the image | user image in SceShell; nothing in games | config file read at both starts; `LOG` compiled out in release |
| [gpues4 profiler](plugin-audit/notes/psp2-cex-gpues4-prof.md) | kernel (`*KERNEL`), 5 offset hooks into SceGpuEs4 + syscalls; client libs linked into the developer's own app | yes: wrong module/signature -> START_FAILED before any hook; after the first hook, failure releases in reverse and stays resident DISABLED ("hook release does not prove every in-flight callback has returned", `kernel.c:586-606`) | reference-quality: signature re-check before each patch, reverse release guarded, error kept for every syscall | no module_stop release by design (resident until reboot) | none | static .bss only; no allocation anywhere | none; the app's own ring buffers | none |
| [iTLS-Enso](plugin-audit/notes/iTLS-Enso.md) | kernel (`*KERNEL`); loads a tiny user module into SceShell itself | yes: missing module or failed hook -> START_FAILED (`kernel/main.c:52-55`) | one hook, n/a | yes (uid default 0, spurious release) | none | module image only | old_dns_fix image (36-line module) + 1 hook in SceShell; nothing in games | 5 file stats at boot; one `ksceDebugPrintf` |
| [0syscall6](plugin-audit/notes/0syscall6.md) | kernel (`*KERNEL`), F00D payload | no: exploit result ignored, always SUCCESS | none | standalone/hfw: nothing released; renga variant symmetric | none | hfw variant: 40 KiB KERNEL block, never freed (`for_hfw/main.c:25`) | none | none |
| [yamt-vita](plugin-audit/notes/yamt-vita.md) | kernel via Enso `boot_config.txt` (before taiHEN) + `*KERNEL` helper + `*NPXS10015` settings plugin | fail-inert: unknown firmware or disabled config -> SUCCESS having touched nothing (`main.c:160-161`) | no hooks: raw DACR-off writes, originals not saved | nothing to undo by design | one 4 KiB thread at prio 0 that remounts and self-deletes (`main.c:33-56, 176`) | ~1.6 KiB statics | settings plugin lives in SceSettings only; nothing in SceShell or games | 140 B config read at boot; mounts moved to a thread "to save time" |
| [ref00d](plugin-audit/notes/ref00d.md) | kernel (`*KERNEL`), 5 import hooks into SceKernelModulemgr's auth calls | no: any init failure calls `SCE_KERNEL_PANIC()` (`src/main.c:153-163`), never FAILED | none; hook installs unchecked | no module_stop at all | one 8 KiB RSA thread at prio 0x5E, event-driven | ~12.8 KiB kernel heap + stack at boot, never freed; no memblocks | none | `ksceKernelPrintf` only |
| [VitaTweaks](plugin-audit/notes/VitaTweaks.md) (4 plugins) | SceShell (`*main`), offset/import hooks + injects | no: always SUCCESS, unknown SceShell -> inert | none; uid arrays zero-initialised (3 of 4) | yes, reverse order, `>= 0` guarded | none | 56 B statics; CustomWarning: one Paf-heap block of the warning file's size, never freed (`CustomWarning/main.c:123`) | all four images in SceShell (single small .c each); nothing in games | CustomWarning reads a file inside SceShell's PAF-load hook; no logging |
| [kubridge](plugin-audit/notes/kubridge.md) | kernel (`*KERNEL`), 5 syscalls | no: unresolved exports leave NULL pointers, always SUCCESS | no hooks | trivially (nothing to release) | none | 20 B statics; hands out caller-chosen memblocks it never tracks | none of its own | none |
| [nonpdrm](plugin-audit/notes/nonpdrm.md) | kernel (`*KERNEL`) | no: 5 installs unchecked, always SUCCESS | none (`-1` skipped at stop) | yes, reverse order guarded | none; 16 KiB `ksceKernelRunWithStack` per app launch (`main.c:308`) | 44 B statics | none | none; file scans and writes inside the app-launch hook |
| [NoPsmDrm](plugin-audit/notes/nonpdrm.md) | kernel (`*KERNEL`) | no | none; `hooks[4]` holds 5 uids (overflow, `main.c:40`) | yes | none; 16 KiB RunWithStack x2 sites | 2 KiB static registry buffer | none | DEBUG-only file log |
| [ds4vita](plugin-audit/notes/ds4vita.md) | kernel (`*KERNEL`) | partly: missing SceBt -> START_FAILED; 12 installs unchecked | uids -1 via one macro, release guarded `> 0` (`main.c:104-107, 718-724`) | yes: thread, heap, hooks | 1 x 4 KiB, 200 ms `DelayThreadCB` | 256 B kernel heap (grows per request) + 4 KiB stack | none | debug builds only (compiled out with RELEASE) |
| [pngshot](plugin-audit/notes/pngshot.md) | SceShell (`*main`), 3 offset hooks + 2 injects into SceShell | no: unknown SceShell -> inert, returns 0 | none; uids discarded | no: releases nothing (`main.c:243-245`) | none | nothing at boot; per screenshot a transient libpng/zlib working set (~270 KiB est.) from `sce_paf_private_malloc` (`main.c:14-20, 149`) | libpng+zlib image in SceShell; Paf heap only during a screenshot | none |
| [vitacompanion](plugin-audit/notes/vitacompanion-vitadeck.md) | SceShell (`*main`), no hooks | yes: `taipool_init` or net thread failure -> START_FAILED (`main.c:25-36`) | n/a (no hooks); threads unwound on failure | yes | net 64 KiB; cmd 64 KiB; FTP server 64 KiB; up to 8 client threads x 64 KiB | 1 MiB taipool at boot + 64 KiB stack (~1.08 MiB idle); peak ~3.4 MiB incl. a 1.5 MiB Paf heap loaded for `install` | everything in SceShell; nothing in games | none at start; network only when Wi-Fi is up |
| [BGFTP](plugin-audit/notes/BGFTP.md) | not a plugin: a background app in its own process | n/a | n/a | `ftpvita_fini` exists but is never called | server 64 KiB + 64 KiB per client; 10 ms power-tick loop | own process: 14 MiB libc heap, 6 MiB transfer buffer | none | none |
| [VVNZRunner](plugin-audit/notes/VVNZRunner.md) | kernel (`*KERNEL`), 24 syscalls, Venezia bridge | no: missing modules -> early SUCCESS with NULL pointers (`main.c:312-324`) | no hooks; raw writes into the Venezia image, originals not saved | no: handler, patches and mappings stay (`main.c:366-369`) | none in the built config | ~80 B statics; maps the caller's CDRAM block, allocates nothing | none; apps bring their own CDRAM | none |
| [PSVshell](plugin-audit/notes/PSVshell.md) | kernel (`*KERNEL`); draws into the game's framebuffer from its display hook | no: not one of 18 hooks, the GUI alloc, mutexes, inject or thread is checked (`main.c:267-397`) | none | yes: thread -> hooks -> inject -> mutexes -> memory (`main.c:400-422`) | 1 x 12 KiB, prio 0x3C, 50 ms poll | 416 KiB `KERNEL_RW` GUI buffer allocated once at start (`gui.c:703-713`) + 12 KiB stack | none; writes pixels into the game's own framebuffer with `ksceKernelMemcpyKernelToUser` (`gui.c:720-739`) | mkdir + profile read at boot (ur0); profile read inside the process-event hook; no log |
| [VitaGrafix](plugin-audit/notes/VitaGrafix.md) | game (`*ALL`), patches the game's main module | no: always SUCCESS, inert in VitaShell/unsupported titles | none: a parse error leaves earlier injects applied until stop | yes, reverse order guarded | none | ~8 KiB .bss + ~6 KiB rodata in the game; no dynamic allocation | nothing in SceShell (image loads there too, inert); in game: statics only | config + patchlist read AND created on every game start; 1 KiB log buffer flushed to ux0 (`main.c:230-297`) |
| [Adrenaline kernel](plugin-audit/notes/Adrenaline-kernel.md) | kernel (`*KERNEL`); loads its user modules into SceShell and the PSP emulator itself | partly: missing SceCompat -> returns the raw error; 7 installs unchecked | none; unknown SceCompat NID makes `module_stop` pair uids with the wrong refs and read `hooks[-1]` (`kernel/main.c:211-221`) | releases 7 hooks unguarded; never unloads the user modules | none | ~76 B statics | `adrenaline_vsh.suprx` image in SceShell (loaded by the kernel plugin, `kernel/main.c:130, 206`) | none |
| **vita-jp-overlay** ([note](plugin-audit/notes/vita-jp-overlay.md)) | kernel `*KERNEL` + SceShell `*main` + game worker `*ALL` | kernel: sync objects and worker thread yes, hooks no (`kernel/main.c:473-492`); Shell: RCO, kernel module and hook yes -> NO_RESIDENT (`shell/boot.c:45-67`), worker start no; game: never (`game_ocr/main.c:395`) | kernel: hooks that installed stay after a failed one (`kernel/main.c:479-480`, `kernel/input.c:504`); all released only if the thread fails | kernel yes (refuses while a job is claimed); Shell yes (STOP_FAIL on a failed join); game yes | kernel 16 KiB @16 ms; Shell 64 KiB + 16 KiB @50 ms + 64 KiB per Anki job; game 32 KiB @100 ms | kernel: 78 KiB bss + 16 KiB stack + capture block up to 2,088,960 B `KERNEL_RW` per capture (`kernel/buffers.c:13, 27`); SceShell: 400.6 KiB image + 80 KiB stacks + Paf heap 64 KiB..~560 KiB + Anki 128/384 KiB; game: 39.4 KiB image + 32 KiB stack + weights 659,456/1,159,168 B + workspace `USER_RW` behind a 4 MiB reserve (`game_ocr/main.c:19, 160-161, 274`) | both, see above | Shell: mkdir/remove/rename/truncate of `status.txt` and a log line before anything else (`shell/boot.c:43-44` -> `shell/log_vita.c:70-82`); three more lines inside the SceShell hook (`shell/boot.c:20-27`); every `vjo_log` is an open/append/close on ux0 (`shell/log_vita.c:93-108`); kernel: 4 KiB ring flushed to ux0 by the worker (`kernel/main.c:126-157`) |

## Where the plugins that draw inside games get their memory

The question the expert asked is where each overlay's bytes come from, and in which
process. The set contains two real in-game overlays and several near relatives.

- **PSVshell** draws from the kernel. Its whole UI is a 308 x 344 x 4 = 416 KiB
  `KERNEL_RW` block allocated once in `module_start` (`gui.c:703-713`), rendered by a
  12 KiB kernel thread every 50 ms, and copied into the game's framebuffer from inside the
  game's own `sceDisplaySetFrameBufInternal` call with `ksceKernelMemcpyKernelToUser`
  (`main.c:60-91`, `gui.c:720-739`). It never allocates anything in the game or in
  SceShell, never allocates after boot, and skips drawing when SceShell's own overlay is
  up (`main.c:67-68`) or in the PSP emulator (`main.c:64-65`). A game that uses every byte
  of its budget is unaffected.
- **VitaGrafix** runs in the game (`*ALL`) and draws into the game's framebuffer from the
  game's `sceDisplaySetFrameBuf` import, pixel by pixel with no off-screen buffer at all
  (`main.c:64-183`, `osd.c`). It has no dynamic allocation: about 8 KiB of statics and
  stack frames of at most 1 KiB (`osd.c:177`). Its OSD hook releases itself after five
  seconds (`main.c:52-62`), so nothing of it runs in the game afterwards. It is inert in
  VitaShell and unsupported titles (`main.c:219-221, 273-275`).
- **pngshot** is a `*main` plugin that replaces SceShell's screenshot encoder with libpng,
  taking its working memory from SceShell's Paf heap through `sce_paf_private_malloc`
  (`main.c:14-20, 149`) only while a screenshot is being encoded, and freeing it at once
  (`main.c:162-167`). It holds nothing between screenshots. This is the concrete form of
  "SceShell wants its headroom for screenshots": the stock encoder and this replacement
  both draw several hundred KiB from the Paf heap at the moment the user presses the
  buttons.
- **ds4vita** is a kernel input plugin with the same SceCtrl/SceTouch hooks the overlay
  uses (`main.c:654-693`); it patches the caller's buffers in place and owns a 256 B
  kernel heap and a 4 KiB thread. Nothing in SceShell or games.
- **vitacompanion** is the one `*main` plugin in the set that keeps a lot resident in
  SceShell: a 1 MiB `taipool` heap and a 64 KiB thread from `module_start`
  (`main.c:24-36`), up to 3.4 MiB at peak during `install` (including a 1.5 MiB Paf heap
  it loads on demand). It draws nothing and hooks nothing. It shows that ~1.1 MiB resident
  in SceShell has shipped for years in a widely used plugin, which is consistent with the
  expert's "well under 2 MiB" ceiling and also consistent with that budget being almost
  used up by one such plugin.
- **The gpues4 profiler** is not an overlay either, but it is the plugin in the set that
  runs inside games with the least footprint: a kernel module that allocates nothing, keeps
  its state in static .bss, and client libraries the developer links into their own app
  whose ring buffers are the app's own static arrays (`client_capture.c:45-66`); when the
  ring is full it counts drops instead of allocating (`client_capture.c:45-48`). Its
  `module_start` re-verifies an 8-byte signature before each of its five offset hooks and
  releases in reverse order on failure (`kernel.c:557-606`).
- **VVNZRunner** and **Adrenaline's kernel module** allocate nothing themselves: the
  calling app brings its own CDRAM blocks (VVNZRunner `util.c:20`), or the kernel plugin
  loads a user module into SceShell at the right moment (`kernel/main.c:130, 206`).

Taken together: the overlays that work inside games either own one kernel block
allocated at boot (PSVshell) or allocate nothing at all (VitaGrafix); the `*main` plugins
either allocate nothing permanent (pngshot, VitaTweaks) or one fixed pool at start
(vitacompanion). None of them combines a resident SceShell footprint, Paf-heap
allocations while a game runs, dynamic kernel blocks, and in-game allocations the way the
overlay does; the overlay's design is more ambitious than anything in the set, which is
why its departures below matter more.

## Where the overlay departs from the common safe pattern, ranked

Ranked by boot risk first (can it stop the console from starting, or leave it in a state
that needs recovery), then by out-of-memory risk. "Reference" names the plugin in the set
that shows the safer pattern. Line numbers are for commit `599dd9c`.

### 1. Permanent SceShell footprint of ~480 KiB plus Paf-heap allocations while a game runs (OOM, highest)

`VitaJPOverlay_Shell.suprx` keeps 221,984 B of code and 178,628 B of data/bss resident in
SceShell (measured), plus 81,920 B of thread stacks (`shell/worker.c:1731-1732`), the
Paf plugin and RCO (`shell/main.cpp:18-26`), and the `*ALL` worker's 40,352 B image
(see 3). The bss is dominated by statics that exist whether or not the overlay is open:
`s_ui_mem` 98,304 B (`shell/overlay.cpp:129`), `scratch_mem` 24,576 B
(`shell/worker.c:63`), three `VjoConfig` copies of 3,516 B (`shell/worker.c:89-90`,
`shell/anki.c:54`), strip buffers 16,674 B (`shell/strip.cpp:35, 38`), `s_cursor_text`,
`local_text` and `s_rng_*` 12,288 B (`shell/overlay.cpp:95, 99`, `shell/worker.c:72`), a
4,384 B `VjoGameOcrResult` (`shell/worker.c:623`) and `g_view` (~8 KiB). On top of that,
whenever a game is active with a network dictionary, 2 x 262,144 B result arenas plus two
TLS slots are taken from SceShell's Paf heap at game start and held until the game exits
(`shell/worker.c:25-31, 337, 1506-1507`); the local pipeline takes 65,536 B per lookup
session; an Anki job takes 131,072 or 393,216 B (`shell/anki.c:39-40, 147`).

Reference: pngshot allocates from the Paf heap only for the duration of one screenshot
(`main.c:149, 162-167`); VitaTweaks keeps 56 B of statics; PSVshell keeps its 416 KiB in
the kernel, not in SceShell. The crash in issue #59 is a SceShell cron thread failing a Paf
allocation with `NO_FREE_PHYSICAL_PAGE`; every byte above is part of what SceShell no
longer has at that moment. The 1.5 MiB reserve guard (`shell/memory.cpp:10, 33-37`)
checks `GetFreeSize()` of the Paf heap, which cannot see physical-page exhaustion, and the
static image and stacks are not behind any guard.

Direction: move the dictionary/UI/Anki code out of the `*main` module into a module that
is loaded only while a game is enabled (the game process, as the expert advised, or a
user module the kernel plugin loads on demand, as iTLS-Enso `kernel/main.c:41-44` and
Adrenaline `kernel/main.c:130, 206` do), and shrink what stays in SceShell to the Paf UI
and the kernel bridge. Short term: drop the per-game Paf arenas for pipelines that do
not need them, and make `s_ui_mem` a Paf allocation that exists only while a page is open.

### 2. The game worker is loaded with `*ALL`, into every process, with a thread in every candidate title (OOM in SceShell and system apps; CPU; pattern)

`tools/patch_tai_config.py:48` registers `VitaJPOverlay_OCR.suprx` under `*ALL`. taiHEN
loads it into SceShell, every NPXS app, VitaShell, the settings app and every game
(taiHEN `hen.c:382-386`; taihen-parser `parser.c:242-245`). `module_start` always returns
`SCE_KERNEL_START_SUCCESS` (`game_ocr/main.c:395`), so the 35,872 + 4,480 B image stays
resident everywhere, and `vjo_game_ocr_worker_start()` creates a 32 KiB thread that polls
`vjoOcrRegister` every 100 ms in every candidate title (`game_ocr/main.c:331, 345, 392`),
including games the user has disabled in `games.ini`, because the policy is only known to
the Shell (`shell/worker.c:1491-1499`).

Reference: no plugin in the set uses `*ALL` except VitaGrafix, which allocates nothing,
creates no thread and is inert in VitaShell (`main.c:219-221`). Every other in-game or
per-title plugin is `*TITLEID` or kernel-side. The expert's rule is `*TITLEID`.

Direction: have the installer and the settings app write one `*TITLEID` section per
enabled game (the Shell already knows the list from `games.ini`), and return
`SCE_KERNEL_START_NO_RESIDENT` from the worker's `module_start` in any process that is
not a candidate, as the Shell plugin already does when its prerequisites are missing
(`shell/boot.c:49, 57, 67`). Until then, at least start the thread only after the Shell
has confirmed the policy, not at module start.

Status (2026-10-13): `module_start` returns `NO_RESIDENT` outside a candidate game
(PR #68), and the installer registers the worker per game, one `*TITLEID` section for
each `--title`, never `*ALL` ([vita-memory.md 4.3](vita-memory.md)). The settings app
does not write taiHEN sections; a game is added with the installer. The idle worker
thread in candidate games is unchanged.

### 3. File writes on SceShell's boot path and inside a SceShell hook (boot, highest of the boot risks)

The first thing the Shell module does is `vjo_status_reset()`: `sceIoMkdir`, `sceIoRemove`,
`sceIoRename` and an open-truncate of `ux0:data/VitaJPOverlay/status.txt`
(`shell/boot.c:43` -> `shell/log_vita.c:70-82`), then a `vjo_log` line, which is another
open/append/close (`shell/boot.c:44`, `shell/log_vita.c:93-108`). Three more lines are
written from inside the hooked `sceAVConfigRegisterCallback` while SceShell is still
starting (`shell/boot.c:20-27`), and every later `vjo_log` call is a synchronous file
write, including the ones made on the Paf main thread every frame the overlay is open or
closed (`shell/overlay.cpp:473, 580, 659, 758, 767, 866`). The kernel plugin's own code
explains the hazard: "earlier in boot ux0: can still be another device (SD2Vita remaps
it)" and defers its first file open until the Shell has registered
(`kernel/main.c:115-121, 133-141`); the Shell plugin does not apply the same caution.

Reference: no `*main` plugin in the set writes a file at `module_start` (pngshot, the
four VitaTweaks, vitacompanion: none). The kernel plugins that log do so into a RAM buffer
and flush rarely (VitaGrafix 1 KiB, `log.c:11, 21-40`; ds4vita 16 KiB and only in debug
builds, `log.c:8-49`), or not at all (PSVshell, nonpdrm, kubridge, VVNZRunner). The one
plugin that reads a file inside a SceShell hook (VitaTweaks CustomWarning,
`CustomWarning/main.c:105-144`) is flagged in its note as the anti-pattern.

Direction: no file I/O before the kernel plugin and Paf are confirmed; a RAM ring for the
Shell like the kernel's (`kernel/log.c`), flushed by the control thread, never by the Paf
main thread or from inside a hook; `status.txt` written once the worker is up.

### 4. Kernel hook installs are not checked and a partial install is kept (boot/consistency)

`kernel/main.c:479-480` installs the display, input and process-event hooks and logs the
three results; `input_hooks_install` installs all 16 even after one fails and returns `-1`
(`kernel/input.c:486-504`), which nobody reads; `module_start` returns success. The
plugin then runs with, for example, a display hook but no process-event hook (it would
never see a game start), or 11 of 12 pad hooks (one pad variant would deliver the
overlay's trigger chord to the game while the others hide it). Only a worker-thread
failure releases everything (`kernel/main.c:484-492`).

Reference: taiHEN `hen.c:395-528` (check every install, `fail:` releases all, start
fails); iTLS-Enso `kernel/main.c:52-55`; the gpues4 profiler `kernel.c:557-606`, which
also notes why a released hook may still have a caller in flight and therefore stays
resident but disabled instead of returning FAILED after the first hook has been published. Most plugins in the set do not check either
(PSVshell `main.c:309-358`, ds4vita, nonpdrm, NoPsmDrm, pngshot, VitaTweaks), so this is
the common pattern, not the safe one.

Direction: treat the three install groups like the sync objects two lines above them:
on any failure `release_hooks(); delete_sync_objects(); return SCE_KERNEL_START_FAILED;`.
The input table already initialises uids to -1 and releases `>= 0` (`kernel/input.c:486-487,
507-514`), so the unwind is one `if`.

### 5. A failed Shell worker start leaves the Paf plugin loaded and the module resident (boot/OOM)

Inside the one-shot hook, `vjo_plugin_load()` loads the RCO as a Paf plugin and then
`vjo_worker_start()` runs; if the worker fails (kernel API mismatch, refused registration,
no mutex, no thread) the failure is logged and nothing is undone (`shell/boot.c:21-27`);
`Plugin::LoadSync`'s result is not examined (`shell/main.cpp:25`). SceShell keeps the Paf
plugin, the RCO and the 400 KiB module for nothing.

Reference: the Shell's own `module_start` already returns `SCE_KERNEL_START_NO_RESIDENT`
for a missing RCO or kernel module (`shell/boot.c:45-57`); VitaGrafix's self-releasing OSD
hook (`main.c:52-62`) shows the hook-time equivalent. The expert's rule "fail closed on
any allocation error at boot: degrade, never crash" also applies here.

Direction: load the Paf plugin only after `vjo_worker_start()` succeeded, and on failure
unload what was loaded (or never load it) so the module can be stopped and unloaded.

### 6. Kernel capture block of up to 2,088,960 B allocated per request from kernel memory (OOM, shared with every kernel plugin)

Every capture allocates a fresh `KERNEL_RW` block the size of the region, up to
960 x 544 x 4 B (`kernel/buffers.c:13, 27`), from a syscall made by the Shell
(`kernel/capture_request.c:36`); an Anki screenshot always takes the full-frame size
(`shell/anki.c:341`). It is freed after the last row is read (`kernel/buffers.c:59-70`)
or on game exit, so it is transient, but it is the largest single kernel allocation in
the set by far.

Reference: PSVshell allocates its 416 KiB `KERNEL_RW` once at boot and never again
(`gui.c:703-713`); no other plugin allocates kernel memory dynamically at all (ds4vita's
heap is 256 B). Kernel memory failures are the ones that take the console down rather than
one process.

Direction: allocate one region-sized block when a game is confirmed and keep it until the
game exits (the PSVshell pattern), sized from the saved region rather than per request;
cap full-frame captures or take them in bands.

### 7. In-game weights and workspace: the right process, but allocations none of the reference overlays make (OOM in the game, medium)

The `*ALL` worker allocates 659,456 or 1,159,168 B for weights and a region-sized
workspace in the game's `USER_RW` (`game_ocr/main.c:161, 274`), each behind a check that
leaves 4 MiB free (`game_ocr/main.c:19, 160`), and frees the workspace per job and the
weights on refusal, exit or inactivity. This is where the expert says the work belongs,
and the check and the reserve are more careful than anything in the set.

Reference: PSVshell and VitaGrafix allocate nothing in the game at all; the only in-game
allocations in the set are the game's own (VVNZRunner samples). A game that allocates
after the check passes can still fail; the overlay's README says so.

Direction: keep; make the reserve configurable per title in `games.ini`, and log the
game's free USER memory at game start (the worker already has `free_user()`,
`game_ocr/main.c:135-143`) so the per-title headroom is measured rather than assumed.

### 8. Five threads and three polling loops where the reference plugins have one or none (CPU3 unmeasured, SceShell stacks)

The overlay runs a 16 KiB kernel thread waking every 16 ms (`kernel/main.c:19, 484`), a
64 KiB and a 16 KiB SceShell thread, the latter waking every 50 ms (`shell/worker.c:42,
1731-1732`), a 64 KiB SceShell thread per Anki job (`shell/anki.c:44, 708`), a 32 KiB game
thread waking every 100 ms in every candidate game (`game_ocr/main.c:331, 345`), and a Paf
main-thread callback every frame (`shell/overlay.cpp:814, 894`; cheap when closed). The
`VjoNet` stack is sized for BearSSL and exists even when the local pipeline needs no
network (`shell/worker.c:1731`).

Reference: PSVshell: one 12 KiB kernel thread at 50 ms; ds4vita: one 4 KiB thread at
200 ms; VitaGrafix, VitaTweaks, pngshot, nonpdrm, kubridge, VVNZRunner: none. Thread
stacks in SceShell are SceShell memory (gap 1). The CPU3 cost of the polling cannot be
computed from source; it is unmeasured until PSVshell readings exist.

Direction: event-driven waits where the kernel already signals (the control thread could
wait without the 50 ms timeout except while classifying a new process), the Anki stack
allocated only when `anki_mining = on`, and the game thread created only after the policy
is confirmed (gap 2).

### 9. Work done inside the game's hooked calls (frame-time, medium)

In the game's `sceDisplaySetFrameBufInternal` the kernel computes a region signature every
8th frame and, when a capture is pending, copies the whole region, up to 2 MiB, before the
game's call returns (`kernel/display.c:70-83`, `kernel/capture.c:104-142`). In the game's
pad calls it also reads the rear touch panel and may run a capture copy for Adrenaline
(`kernel/input.c:389-390`, `kernel/display.c:129-141`).

Reference: PSVshell does the same kind of work in the same hook, a copy of its 416 KiB
buffer into the framebuffer (`gui.c:720-739`), and reVita reads touch in its ctrl hooks;
the overlay's comments cite both (`kernel/capture.c:78-85`). This is the common pattern
for a kernel overlay and the only way to copy another process's framebuffer safely. The
difference is scale: a full-frame copy is five times PSVshell's per-frame copy.

Direction: keep the same-process rule; bound the per-frame copy (bands over several
frames) if frame-time measurements on the console show stutter.

### 10. Kernel log flushed to ux0 from the 16 ms worker (I/O cadence, low)

`kernel_file_flush()` runs every worker tick with data: an open/append/close on
`ux0:data/VitaJPOverlay/kernel.txt` (`kernel/main.c:126-157, 209`), capped at 64 KiB per
boot, with the first flush deferred until the Shell registers (good, see gap 3).

Reference: ds4vita and VitaGrafix buffer and flush once (`log.c`); PSVshell does not log.

Direction: flush on a timer (once a second) or on specific events, not per tick.

## What the overlay already shares with the safe patterns

- Same display, pad, touch and process-event hook NIDs as PSVshell, reVita and ds4vita,
  and the same same-process framebuffer rule (`kernel/capture.c:78-85`).
- Kernel `module_start` fails closed for sync objects and the worker thread, and
  `module_stop` is complete and ordered thread -> hooks -> memory -> sync objects
  (`kernel/main.c:473-476, 484-515`), as PSVshell `main.c:400-422`.
- Shell `module_start` returns `SCE_KERNEL_START_NO_RESIDENT` when the RCO, the kernel
  module or the bootstrap hook is missing (`shell/boot.c:45-67`), which no `*main` plugin in
  the set does; the one-shot self-releasing hook is VitaGrafix's pattern (`main.c:52-62`).
- Weak imports with a stub-pattern check before calling (`include/vjo_import.h`), the
  user-space twin of taiHEN's `TAI_ERROR_STUB_NOT_RESOLVED` test (`taihen.c:117-121`).
- 4 KiB result structures kept static instead of on syscall stacks (`kernel/game_ocr.c:14`,
  `game_ocr/main.c:39`), the concern nonpdrm solves with `ksceKernelRunWithStack`.
- No firmware-specific offsets in the kernel module; title IDs are read in user space
  because the kernel NIDs differ between 3.60 and 3.63+ (`kernel/foreground.h:24-27`).
- Capture, weights and workspace are freed when the game leaves the foreground, with the
  state machine host-tested.

## What this audit cannot settle

- Nothing was run on a console. Which of the gaps above caused the two failed boots in
  issue #59 is not proven by reading; the ranking is by exposure, and gaps 1 and 3 are the
  ones that act on SceShell at the moment the crash signature points to.
- Vita3K is not a test bed for taiHEN plugins and no Vita3K result is used here.
- CPU3 cost of the polling threads and hooks is unmeasured; the static review above names
  the pressure points (gaps 8-10) and no percentage.
- Paf heap sizes, the RCO's resident size and the Paf page objects are not visible in
  source; the Shell numbers above are the measured ELF segments, the stacks and the
  explicit Paf allocations.
- The sizes of the reference plugins' module images were not measured (no builds), only
  their statics and allocations.

## Method

Clones: taiHEN, henkaku, iTLS-Enso, 0syscall6, yamt-vita, ref00d, VitaTweaks, kubridge,
nonpdrm, ds4vita, NoPsmDrm, pngshot, vitacompanion-vitadeck, psp2-cex-gpues4-prof,
VVNZRunner, BGFTP, PSVshell, VitaGrafix, Adrenaline (kernel/ only), at the commits named at
the top of each note. Every `.c/.h/.cpp/.S/CMakeLists.txt/exports.yml/Makefile` in scope
was read; each note lists its files and line counts. The schema every note follows is
[docs/plugin-audit/SCHEMA.md](plugin-audit/SCHEMA.md). Overlay segment sizes:
`readelf -lW build/vita/VitaJPOverlay_{Kernel,Shell,OCR}.velf` on a build of `599dd9c`
(Kernel `R E` 0x6744 / `RW` 0x13130; Shell 0x36320 / 0x2b9c4; OCR 0x8c20 / 0x1180).
Struct sizes (`VjoConfig` 3,516, `VjoGameOcrResult` 4,384, `SceneTracker` 15,056) from a
host compile of the pointer-free headers.

The notes written by helper agents were spot-checked against the clones; the facts held
in every case checked, but some of their line numbers drift by one to three lines (for
example `CustomWarning/main.c:120` is line 123). Where this document cites a helper note's
line, the line was re-read in the clone.
