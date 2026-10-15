# Slimming the SceShell plugin

`VitaJPOverlay_Shell.suprx` runs in SceShell (`*main`), where every page it holds is taken from
SceShell's 50 MiB partition for as long as the console is on, and where the partition had 28-98 KiB
free beside a game [measured, [vita-memory.md 5.1](vita-memory.md)]. AGENTS.md rule 1: SceShell keeps
only what must run there. This document is the plan for moving everything else out, with the
measured starting point and the expected pages per move; each move's PR replaces its estimate with
the measured result.

Tags as in [vita-memory.md](vita-memory.md): **[measured]**, **[source]**, **[SDK: file]**,
**[wiki: page]**, **[estimate]**.

## 1. The reference design: PSVshellPlus_Shell

[GrapheneCt/PSVshellPlus](https://github.com/GrapheneCt/PSVshellPlus) at `b70f1c7` (2023-05-05,
release v1.4), read in full [source]:

| What | How | Where |
|---|---|---|
| Bootstrap | hooks SceShell's `sceAVConfigRegisterCallback` import and loads its RCO as a Paf plugin from inside it, once Paf runs; returns `NO_RESIDENT` when the RCO or the kernel module is missing | `PSVshellPlus_Shell/main.cpp:83-99`, `115-129`, `132-151` |
| Threads | none: every update runs on Paf's main thread through `common::MainThreadCallList::Register` | `tracker.cpp:151`, `hud.cpp:80,149,243,378` |
| UI objects | `new`ed on the Paf heap when the user opens a HUD, deleted when it closes | `impose.cpp:118-173` |
| State and work | in the kernel module: clocks, FPS, memory, Venezia load are kernel syscalls (`PSVshellPlus_KernelForUser`, `syscall_export`); the Shell only formats numbers into Paf text widgets | `PSVshellPlus_Kernel/lib.emd:8-24`; `hud.cpp:123,288` |
| Strings | in the RCO (six locales), not in the binary | `RES_RCO/locale/*.xml` |
| Files | one fixed-size binary profile per title through Paf's `LocalFile`; no text parsing | `profile.cpp:45-69` |
| Libraries | Paf, kernel, AppMgr, power, display and its own kernel stub; no libc (`--no-standard-libraries`), no network, no TLS | `PSVshellPlus_Shell.vcxproj:72` |

Its memory, from the v1.4 release SELFs [measured: segment headers of `PSVshellPlus_Shell.suprx`,
sha256 `3a1e9295…`, with `tools/check_memory_ledger.py`'s reader]: **45,056 B of Shell pages**
(39,572 B code+rodata in 40,960 B, 232 B data in 4,096 B), no stacks, and the 31,632 B RCO in the
Paf heap. The kernel module is 12,288 B of pages.

Our bootstrap already follows it (`shell/boot.c`). The difference is everything after the
bootstrap: PSVshellPlus keeps the UI in SceShell and the work in its kernel module; we keep the UI,
the work and a network stack in SceShell.

## 2. Our Shell today (main `ea7bcc3`)

The packaged SELF: 216,496 B code+rodata (217,088 B of pages) and 78,008 B data+bss (81,920 B):
**299,008 B of module pages**, plus two permanent threads, VjoNet (64 KiB stack + 4 KiB TLS) and
VjoControl (16 KiB + 4 KiB): **389,120 B permanent** in the ledger's "shell pages" pool
[measured: the build log's module row and ledger table]. By source group, from the Shell's linker
map (`-Wl,-Map`; kept input sections, strings by their pre-merge size; 31,788 B of merged string
literals in the binary) [measured]:

| Group | code + rodata + strings (B) |
|---|---:|
| BearSSL (TLS engine, X.509, EC, AES/ChaCha, SHA) | 66,673 |
| root certificates (`core/tls_roots.c`) | 6,324 |
| TLS, HTTP, sockets (`tls.c`, `http.c`, `net.c`, `shell/platform_vita.c`) | 6,471 |
| Lens, JPEG, protobuf, base64, kernel capture (`lens.c`, `pb.c`, `jpegsw.c`, `base64.c`, `shell/capture.c`) | 6,719 |
| online dictionaries and their client (`jiten.c`, `jpdb.c`, `hachidori.c`, `json.c`, `client.c`, `entries.c`) | 13,643 |
| Anki (`core/anki.c`, `core/anki_queue.c`, `shell/anki.c`, `shell/anki_queue.c`) | 20,232 |
| local dictionary, furigana, pitch (`local_dict.c` with the deinflection tables, `furigana.c`, `pitch.c`) | 27,714 |
| configuration: parser and the 5.6 KiB `config.ini` template (`config.c`) | 10,163 |
| UI: overlay, strip, region selector, text rows/hit, styling, UTF-8, arena | 19,659 |
| control: worker, game OCR client, self-test, boot, guard, ledger, log, files, games | 32,999 |
| libgcc division | 1,808 |

The network stack (the first five rows) is 99,830 B, 46 % of the code. `.bss` is 77,846 B:
`shell/worker.c` 47,028 (the 24 KiB control scratch, two `VjoConfig` copies, the job's
`VjoGameOcrResult`), `shell/strip.cpp` 19,326, `shell/overlay.cpp` 8,644.

## 3. Where the network goes: kernel or game process

The experts' relay (10:18Z) proposed the kernel. The evidence:

| | kernel (`*KERNEL`) | game process (the `*TITLEID` worker) |
|---|---|---|
| memory | `ScePhyMemPartKD`, 36 MiB shared by every driver, free size unmeasured; a 2,088,960 B `KERNEL_RW` block failed five times on 3.65 (#74) [measured, vita-memory.md 5.3]. Module pages there are permanent for every boot: ~100 KiB of code plus a 16.7 KB TLS buffer would nearly double our 114,688 B kernel module, used or not | the game's budget: 87-91 MiB free in CLANNAD [measured]. libnet, HTTP and SSL sysmodules "do not consume the memory from the game budget" [SDK: libsysmodule-Reference, 'Module IDs']; libnet runs on a caller pool of 16-32 KiB [SDK: libnet-Reference, 'SceNetInitParam'] |
| failure | a fault in kernel code takes the whole console down | a fault ends the game's process |
| network API | only SceNetPs's driver sockets (`SceNetPsForDriver`: socket, connect, send/recv) [wiki: SceNetPs]; the DNS resolver is user libnet, SceSsl and SceHttp are user modules [wiki: SceNet, SceLibSsl, SceLibHttp]: DNS would have to be written for the kernel | user libnet with its resolver, as in SceShell today |
| code constraints | kernel code is built without FPU/NEON and checked for it (`tools/check_kernel_instructions.py`); the JPEG encoder's DCT is float | none |
| attack surface | X.509, TLS and protobuf parsing of server-controlled bytes with kernel privileges | the same parsing with the game's privileges |
| transport | n/a | the existing job transport: the request names the engine, the text comes back in `VjoGameOcrResult.text` (≤ 4 KiB); no kernel change |

Prior art for network code inside a game: **rinCheat** (Rinnegatamante) loads its own network
module into the running game (`main_module/main.c:159`, `sceKernelLoadStartModule`), loads
`SCE_SYSMODULE_NET` and initializes libnet only when the game has not
(`net_module/main.c:122-141`: `sceNetShowNetstat() == SCE_NET_ERROR_ENOTINIT`, a 1 MiB pool of its
own), and never terminates it (`net_module/ftpvita.c:1120-1125`, commented out) [source,
`Rinnegatamante/rinCheat@d17fd2a`]. **ftpeverywhere** is a `*main` plugin, where SceShell's libnet
is already up (`src/main.c:19-22`: "Libc which is not available in main"), so it says nothing
about games. **vitacheat**'s repository has
the cheat database and binaries but no plugin source, so it cannot be cited. This project's own
Lens path has always run in SceShell (`shell/platform_vita.c`: "SceShell has already initialized
the network stack").

What can break a game: libnet allows one initialization per process; a second `sceNetInit` returns
`SCE_NET_ERROR_EBUSY` [SDK: libnet-Reference, 'sceNetInit']. If our worker initialized libnet and
the game called `sceNetInit` later, the game would get that error. The worker therefore

- loads `SCE_SYSMODULE_NET` only when `sceSysmoduleIsLoaded` says it is not, and never unloads it
  (it costs the game's budget nothing, and the SDK documents no reference count: a load of a loaded
  module returns `SCE_OK` [SDK: libsysmodule-Reference, 'sceSysmoduleLoadModule'], so an unload
  could remove a module the game loaded meanwhile);
- initializes libnet only when the game has not (`SCE_NET_ERROR_EBUSY` means the game's own is
  used, and never terminated), with its pool inside the job's workspace, for one Lens request,
  and terminates it before the workspace is freed;
- uses no libnetctl (the connection state check is the resolver's own failure).

A game that calls `sceNetInit` during a Lens request (about a second) is the remaining exposure.

**Decision: the game process.** Every measured or cited constraint favours it; the kernel's only
advantage, availability in every game, is not one here (a game needs the worker registered for any
OCR, `--title`).

## 4. The moves

Expected pages before → after; [estimate] until the move's PR measures it. "Shell" is the module's
code pages (data pages separately where they change), "permanent" the ledger's shell-pages pool.

| # | Move | PR | Shell code pages | other Shell memory | elsewhere |
|---|---|---|---|---|---|
| — | jiten.moe and jpdb.io removed (owner 09:17Z) | overlay-remove-online-dicts | −1 to −2 [estimate] | `VjoConfig`'s API keys | — |
| b | AnkiConnect removed (owner 12:02Z): the queue writer stays, no Anki network is left to move | overlay-apkg | −2 [estimate] | VjoAnki's transient 64 KiB stack can shrink | — |
| a1 | Lens, JPEG, protobuf, BearSSL's TLS and the root certificates into the game worker; Lens becomes an engine of the worker (the request's model `lens`), its text returns like the local engine's | this series | −19 to −20 [estimate: 80 KB] | VjoNet's stack 64 → 32 KiB (BearSSL was its reason): −8 pages permanent; Lens with a local dictionary uses the local pipeline's one 64 KiB arena instead of 2 × 256 KiB + a 47,600 B connection pool | worker +~21 code pages and 32 → 64 KiB stack, in each registered game's partition |
| d | the `config.ini` template out of the binary (ships as a file; written on first run as before); trimmed log and message strings | this series | −1 to −2 [estimate: 5.6 KiB + strings] | — | — |
| a2 | Hachidori (plain HTTP on the LAN, ~16 KB) | evaluated, not moved | — | — | its entries exceed the 4 KiB text field: it needs a bulk result transfer through the kernel |
| a3 | the kernel's full-frame JPEG capture (`vjoRequestCapture`, `vjoReadRaw`, `VjoCapture`), unused once a1 and b are in | later, kernel | — | — | kernel pool worst case −2,088,960 B, module pages down |
| c | local dictionary lookup, furigana and pitch game-side | evaluated, not moved | would be −7 [measured: 27,714 B] | the 64 KiB result arena would shrink | needs a kernel file broker for the 134 MiB dictionary (games may have no `ux0:`, the reason the model broker exists), a kernel round trip per read (a lookup reads the index about 19 times) and a result larger than 4 KiB for the entry (definitions reach 7,000 B) |

With a1 and d, the Shell's code would go from 217,088 B to about 135,000 B of pages and its
permanent footprint from 389,120 B to about 270,000 B [estimate].

## 5. What each PR shows

The measured Shell segment sizes and module pages before and after (the build log's module row),
the linker map's groups for what moved, the ledger table with the budgets lowered
(`include/vjo_ledger_budget.h`: `VJO_BUDGET_SHELL_MODULE_PAGES` and every item that shrank), host
tests for the moved code in its new place, CI, `tools/preflight.sh` GO, and an audit. Every moved
path writes its route to `log.txt` (for Lens: the job's steps and the worker's request lines), so
one console run confirms where it ran. Boot safety is unchanged: the guard, `*TITLEID`
registration, and no heavy work in SceShell.
