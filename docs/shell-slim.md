# Slimming the SceShell plugin

`VitaJPOverlay_Shell.suprx` runs in SceShell (`*main`), where every page it holds is taken from
SceShell's 50 MiB partition for as long as the console is on, and where the partition had 28-98 KiB
free beside a game [measured, [vita-memory.md 5.1](vita-memory.md)]. AGENTS.md rule 1: SceShell keeps
only what must run there: the Paf UI, input, and a thin, bounded transport to the game worker. This
document is the plan for getting there, with the measured starting point and the expected pages per
step; each step's PR replaces its estimate with the measured result.

Tags as in [vita-memory.md](vita-memory.md): **[measured]**, **[source]**, **[SDK: file]**,
**[wiki: page]**, **[estimate]**.

**The owner's decision (2026-10-15 15:27Z): the overlay is offline only.** Google Lens and every
piece of network and TLS code are removed, not moved. The rollback point is the tag
`archive/online-last` and the branch `archive/online` (main `72c6148`). Section 3 keeps the
evaluation that preceded the decision, and the branch `archive/lens-in-game-worker` keeps the
design it led to.

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
bootstrap: PSVshellPlus keeps the UI in SceShell and the work in its kernel module; we kept the UI,
the work and a network stack in SceShell.

## 2. Our Shell, measured

From the Shell's linker map (`-Wl,-Map`; kept input sections, string literals by their pre-merge
size) [measured]. Main `ea7bcc3` had 216,496 B of code and rodata (217,088 B of pages) and
78,008 B of data and `.bss` (81,920 B): 299,008 B of module pages, and with the two permanent
threads (VjoNet 64 KiB + 4 KiB TLS, VjoControl 16 KiB + 4 KiB) **389,120 B permanent**. #117
(jpdb.io and jiten.moe removed) leaves 209,904 B and 71,564 B, 286,720 B of pages, by group:

| Group | code + rodata + strings (B) | after the offline removal |
|---|---:|---|
| BearSSL: TLS engine, X.509, EC/RSA, ciphers | 65,069 | removed |
| BearSSL: SHA-256 (`sha2small.c`, `dec32be.c`, `enc32be.c`) | 1,604 | stays: the Anki queue's IDs |
| root certificates (`core/tls_roots.c`) | 6,324 | removed |
| TLS, HTTP, sockets (`tls.c`, `http.c`, `net.c`, `shell/platform_vita.c`) | 6,471 | removed, but `platform_vita.c`'s file reads |
| Lens, protobuf, JPEG, base64, the kernel capture's reader | 6,719 | removed (base64 by overlay-apkg) |
| Hachidori, JSON, the network client, entry lists | 9,346 | Hachidori by overlay-remove-hachidori; the network lookup path removed |
| Anki | 20,054 | AnkiConnect by overlay-apkg; the queue writer stays |
| local dictionary, furigana, pitch | 27,682 | stays (section 4.4) |
| configuration: parser and the `config.ini` template | 9,855 | the template leaves the Shell (overlay-app-mismatch) |
| UI: overlay, strip, region selector, text | 18,043 | stays |
| control: worker, game OCR client, self-test, boot, guard, ledger, log, files | 32,594 | the network pipeline removed |
| libgcc and SDK stubs | 2,192 | SceNet's stubs removed |

The network (the first five rows and Hachidori) is 46 % of the code. `.bss` is 71,406 B:
`shell/worker.c` 43,860 (the 24 KiB control scratch, two `VjoConfig` copies, the job's
`VjoGameOcrResult`), `shell/strip.cpp` 19,326, `shell/overlay.cpp` 8,644.

## 3. Where the network would have gone: kernel or game process

The experts' relay (10:18Z) proposed the kernel; the evaluation chose the game process. The owner
then removed the network; this is the record of why it was not the kernel.

| | kernel (`*KERNEL`) | game process (the `*TITLEID` worker) |
|---|---|---|
| memory | `ScePhyMemPartKD`, 36 MiB shared by every driver, free size unmeasured; a 2,088,960 B `KERNEL_RW` block failed five times on 3.65 (#74) [measured, vita-memory.md 5.3]. Module pages there are permanent for every boot | the game's budget: 87-91 MiB free in CLANNAD [measured]. libnet, HTTP and SSL sysmodules "do not consume the memory from the game budget" [SDK: libsysmodule-Reference, 'Module IDs']; libnet runs on a caller pool of 16-32 KiB [SDK: libnet-Reference, 'SceNetInitParam'] |
| failure | a fault in kernel code takes the whole console down | a fault ends the game's process |
| network API | only SceNetPs's driver sockets (`SceNetPsForDriver`) [wiki: SceNetPs]; the DNS resolver is user libnet, SceSsl and SceHttp are user modules [wiki: SceNet, SceLibSsl, SceLibHttp] | user libnet with its resolver |
| code constraints | kernel code is built without FPU/NEON (`tools/check_kernel_instructions.py`); the JPEG encoder's DCT is float | none |
| attack surface | X.509, TLS and protobuf parsing of server-controlled bytes with kernel privileges | the same parsing with the game's privileges |

Prior art for network code inside a game: **rinCheat** loads its own network module into the
running game (`main_module/main.c:159`), loads `SCE_SYSMODULE_NET` and starts libnet only when the
game has not (`net_module/main.c:122-141`), and never terminates it (`net_module/ftpvita.c:1120-1125`,
commented out) [source, `Rinnegatamante/rinCheat@d17fd2a`]. **ftpeverywhere** is a `*main` plugin
(`src/main.c:19-22`), which says nothing about games; **vitacheat**'s repository has no plugin
source. libnet allows one initialization per process: a second `sceNetInit` returns
`SCE_NET_ERROR_EBUSY` [SDK: libnet-Reference, 'sceNetInit'], so a game-side client must start libnet
only when the game has not and never end the game's.

The design built on that evaluation (Lens as a job of the game worker over the game's libnet, with
host tests) is on the branch `archive/lens-in-game-worker`. Its measurement on main `ea7bcc3`
(Vita build) [measured]: the Shell's code 216,496 → 209,400 B, the worker's 65,068 → 155,276 B
(+22 pages in every registered game), the Lens job's deepest stack chain 6,988 B.

## 4. The plan

Expected pages before → after; **[estimate]** until the step's PR measures it.

### 4.1 Removed by other PRs first

| Step | PR | Shell |
|---|---|---|
| jpdb.io and jiten.moe | #117 (overlay-remove-online-dicts) | 299,008 → 286,720 B of module pages [measured] |
| the Hachidori relay client | #126 (overlay-remove-hachidori) | 286,720 → 282,624 B [measured by its author, on #117 `6b04af0`] |
| AnkiConnect: the send, the audio URL, the picture upload, the app's Anki network page | #125 (overlay-apkg) | 282,624 → 266,240 B [measured by its author, on main `5e68ecd`] |
| the `config.ini` template (the Shell no longer writes `config.ini`) | #122 (overlay-app-mismatch) | 189,104 → 186,416 B of text (−2,688 B), 266,240 → 262,144 B of module pages (266,240 B still declared) [measured: the build log's module rows, main `0b95c9d` and `ac905b1`] |

### 4.2 Step 1: the network, all of it (this series, after the PRs above)

In two PRs. **PR-A, Google Lens** [measured]: the engine, the engine choice in the Overlay app
(VOCR is the only engine; the model-size picker stays), `ocr_backend` (a saved line loads as VOCR,
a removed engine's with one config warning; the app's save and the installer drop the line, the
installer saying so once), `core/lens.c`, `core/pb.c`, the worker's capture → JPEG → Lens job and
its connection warm-up, the host's Lens CLI, replay, fixtures and benchmark; and, with #126's relay
gone too, the Shell worker's network pipeline (the two 256 KiB arenas, the pool, the cached-sentence
comparison; `VJO_BUDGET_PAF_RESULTS_NET` goes). With overlay-apkg's #125 merged first (the Anki
screenshot gone), PR-A also deletes the Shell's capture → JPEG path: `shell/capture.c`,
`core/jpegsw.c`, the capture lock a local OCR job held, and their tests and tool. Lens was the
Shell's last TLS connection, so its link no longer keeps BearSSL's TLS, the roots or the HTTP
client (`--gc-sections`; only SHA-256 stays, the queue's IDs), though their sources stay for PR-B.
On main `6020c0b` the Shell's text segment goes from 188,000 to 100,792 B (`.text` −66,728 B,
`.rodata` −20,424 B) and data and `.bss` from 70,132 to 70,060 B: 262,144 → 176,128 B of module
pages, 21 text pages, and the declared budget from 266,240 to 176,128 B [measured: the build log's
module rows and `arm-vita-eabi-size`; by object in [memory-ledger.md](memory-ledger.md)]. The kernel's
capture syscalls (`vjoRequestCapture`, `vjoReadRaw`) stay for step 2. **PR-B**: the rest of this
step.

- **Google Lens** (PR-A, above).
- **The network stack**: `core/net.c`, `core/http.c`, `core/tls.c`, `core/tls_roots.c` and
  `third_party/roots`, BearSSL but its SHA-256 and SHA-1 (the queue's IDs, the models' hashes,
  the `.apkg`'s checksums), the sockets, resolver and cancellation in `shell/platform_vita.c`, the
  UDP log (`log_host`, `tools/udp_log_listener.py`), the SceNet, SceNetCtl, SceHttp and SceSsl
  imports in the Shell, the worker and the Overlay app.
- **The Shell worker's network pipeline**: the double-buffered 2 × 256 KiB result arenas, the
  connection pool, the warm-up and its 50 ms timer path, the cached-sentence comparison; one 64 KiB
  arena remains, as the local pipeline always used.
- **VjoNet's stack, 64 → 16 KiB** (PR-A2, #132): 64 KiB was "BearSSL needs it for TLS"
  (`include/vjo_ledger_budget.h`); the thread's deepest chain without TLS is 5,624 B (the job's log
  batch) [measured: `arm-vita-eabi-gcc -fstack-usage -fcallgraph-info=su` of the Shell's sources,
  main `6020c0b` with #130]. The thread paints its stack and logs its high water at game exits.

| | before (#117) | after #126 | after #125 (main `6020c0b`, with #123, #122, #127 and #124) [measured] | after PR-A [measured] | after PR-A2 [measured] |
|---|---:|---:|---:|---:|---:|
| Shell code + rodata | 210,184 B | 206,192 B | 188,000 B | 100,792 B | 101,256 B |
| Shell module pages | 286,720 B | 282,624 B | 262,144 B (266,240 B declared) | 176,128 B (declared too) | 176,128 B |
| Shell permanent (the ledger: declared module + stacks + TLS) | 376,832 B | 372,736 B | 356,352 B | 266,240 B | 217,088 B |
| PAF worst case (network configuration) | 731,632 B | 707,840 B | 707,840 B | gone: 225,280 B is the only one | 225,280 B |

PR-B now removes sources the link already drops, plus the sockets the UDP log and the cancel
paths keep (SceNet, SceNetCtl): a few pages at most [estimate], against this plan's earlier
"about 196,000 B" for both PRs together. PR-A2 (#132) lowers VjoNet's stack from 64 to 16 KiB:
SceShell's permanent pages 266,240 → 217,088 B.

### 4.3 Step 2: the kernel's full-frame capture

Once Lens and the Anki screenshot are gone, nothing reads the kernel's RGBA capture
(`vjoRequestCapture`, `vjoReadRaw`, `VjoCapture`): local OCR reads its own screen in the game since
API 19. Removing it takes the kernel pool's worst case from 2,220,032 B to about 200,000 B (the
2,088,960 B full-frame block is the one #74's console refused) and shrinks the kernel module; it
changes the syscall table, so it raises `VJO_API_VERSION`.

### 4.4 Step 3: the rest

- **Log strings**: handed to overlay-release-variants (2026-10-15), which gates the debug-only
  lines, the self-test and the ledger tables behind a `VJO_DEBUG` build option (the release's
  Normal zip leaves them out, its `-debug` zip keeps them) and shortens only the lines that stay in
  Normal and that no tool parses (`tools/analyse_selftest.py`, `tools/collect_vita.py` and their
  tests pin the parsed ones). Both edit the same call sites, so one owner.
- **Local lookup in the game**: evaluated, not moved. Its code is 27,682 B with its tables, and its
  64 KiB arena is in the PAF heap only while a game runs. In the game it would need a kernel file
  broker for the 134 MiB dictionary (some games have no `ux0:`, the reason the model broker exists),
  a kernel round trip per read (90 lookups in the v0.8.0-test.1 and test.2 consoles' logs read the
  dictionary 15.9 times each and took 80.5 ms on average [measured: their `lookup … reads` lines]),
  and a result transfer for entries over 4 KiB (definitions reach 7,000 B). The pages it
  would free cost a kernel API, latency per key press, and a kernel copy of every definition.

## 5. What each PR shows

The measured Shell segment sizes and module pages before and after (the build log's module row),
the linker map's groups for what changed, the ledger table with the budgets lowered
(`include/vjo_ledger_budget.h`), host tests, CI, `tools/preflight.sh` GO, and an audit. Boot safety
is unchanged: the guard, `*TITLEID` registration, and no heavy work in SceShell.
