# PS Vita memory: the reference for this overlay

Written 2026-10-13. This is the permanent memory reference for Rubi: a kernel plugin
(`*KERNEL`), a SceShell UI plugin (`*main`) and an in-game OCR worker. It distills the Sony SDK
documentation (Runtime Library Release 3.55), the henkaku wiki, the taiHEN sources, the console
measurements this project has made, and the guidance of a PS Vita plugin developer (2026-10-11,
kept in the `vita-taihen-plugin` skill and in [AGENTS.md](../AGENTS.md)). The SDK files are
confidential: they are cited by file name and never copied here.

Every statement carries one of these tags:

- **[SDK: file]** a Sony SDK document, by file name (`SDK_doc__…_e.txt`), with the section.
- **[wiki: Page]** a page of the henkaku wiki mirror, with the section.
- **[measured]** a number read on the owner's PCH-1001 (firmware 3.65) or from the packaged
  binaries, with the note or document that holds it.
- **[expert]** the plugin developer's guidance of 2026-10-11.
- **[source]** a public source file (taiHEN, PSVshell).
- **[estimate]** or **[inference]**: not measured, not stated by a source. Treat as a hypothesis.

Companion documents: [issue #59's root cause](https://github.com/bee-san/Rubi_for_vita/pull/60)
(`docs/issue-59-root-cause.md`, PR #60: the measured SceShell footprint), [the OCR
architecture](vita-vn-ocr.md), the budget doc in vita-vn-ocr
([`docs/vita-memory-budget.md`](https://github.com/bee-san/vita-vn-ocr/blob/main/docs/vita-memory-budget.md))
and [the memory ledger](memory-ledger.md) (`docs/memory-ledger.md`: every allocation site, the budget per pool,
the CI check that enforces it).

## 1. The short version

1. Memory is partitioned at boot into fixed physical **budgets**: the game gets 256 MiB of main
   memory and 112 MiB of video memory, SceShell gets 50 MiB and 16 MiB, every kernel driver
   shares 36 MiB, and so on (section 3). A process can only take pages from its own budget; when
   they run out the kernel returns `0x80024302`, no matter how much is free elsewhere.
2. A taiHEN plugin lives in the budget of the process it is loaded into. `*main` is SceShell.
   `*ALL` means every user process, SceShell included. `*TITLEID` means that one game. Module
   pages, thread stacks, TLS and every `sceKernelAllocMemBlock` of the plugin are charged to that
   budget (section 4).
3. SceShell's 50 MiB are nearly full while a game runs: 28,672 B and 98,304 B free in two
   console samples [measured]. There is no safe non-zero budget for permanent plugin memory in
   SceShell; a 65,544 B static buffer alone has broken the boot (#42), and our 540,672 B of
   permanent Shell pages starved a firmware LiveArea thread (#59). The expert's limit for a whole
   Shell plugin is "genuinely under 2 MiB", and SceShell wants that headroom itself (section 5).
4. The heavy work belongs in the game's process: OCR workspace, model weights, dictionaries. CLANNAD
   had 87-91 MiB of its budget free [measured]; most games leave tens of MiB, homebrews and a few
   games take everything [expert]. Preflight with `sceKernelGetFreeMemorySize`, keep a reserve,
   allocate per job, free after (section 6).
5. The kernel plugin shares one 36 MiB pool with every firmware driver. Its free space is
   unmeasured; a 2 MiB full-frame capture buffer failed there (#14, #74; removed in API 24). Keep
   kernel buffers region-sized and transient (section 5.3).

## 2. The hardware

| Fact | Value | Source |
|---|---|---|
| Main memory | 512 MiB LPDDR2 on retail units and TestKits; the DevKit has 1 GiB plus 512 MiB of trace memory | [SDK: `SDK_doc__Development_Environment__Hardware-Overview_e.txt`, 'Memory', 'Testing Kit'] |
| Video memory (CDRAM) | 128 MiB, physically 0x20000000-0x27FFFFFF; all of it physically contiguous | [wiki: Physical_Memory, main table]; [SDK: `SDK_doc__Tutorial__Memory_Management-Tutorial_e.txt`, 'CDRAM'] |
| CPU | four Cortex-A9 cores, 32 KiB L1 I + 32 KiB L1 D each, NEON; one 2 MiB L2 shared by all cores (PL310, 16-way) | [SDK: Hardware-Overview, 'CPU']; [wiki: Caches, 'PL310 L2 Cache'] |
| Cache line | 32 bytes, L1 and L2 | [SDK: Memory_Management-Tutorial, 'CPU and GPU Caching Hardware'] |
| Cores an application may use | three of the four; kernel and system threads are meant to run on the fourth, but system work can still preempt on the application's cores | [SDK: `SDK_doc__Development_Environment__Programming-Startup_Guide_e.txt`, 'CPU'] |
| User affinity masks | only `SCE_KERNEL_CPU_MASK_USER_0/1/2` (and `_ALL`) exist; a user thread cannot name the fourth core | [SDK: `SDK_doc__System__Kernel-Reference_e.txt`, 'sceKernelCreateThread'] |
| Default clocks in the Shell | ARM 333 MHz, bus 222 MHz, GPU 41 MHz | [wiki: ScePower, 'Clocks'] |
| MMU page sizes | 4 KiB, 64 KiB, 1 MiB, and 16 MiB (LPDDR only); matching block sizes to them reduces TLB misses | [SDK: Memory_Management-Tutorial, 'CPU TLB Page Sizes'] |
| Caching | LPDDR can be allocated cached or uncached; CDRAM is always uncached for the CPU; the GPU caches everything and snoops the CPU L2 for cached LPDDR | [SDK: Memory_Management-Tutorial, 'Memory Caching Modes', 'CPU and GPU Caching Hardware'] |
| Virtual addresses | every user address is virtual, even in physically contiguous memory; a process's main memory is mapped in the `SceKernelUserMain` range (0x81000000-0xCFFFFFFF), which is why game and plugin addresses start with 0x81 | [SDK: Memory_Management-Tutorial, 'Virtual Addressing']; [wiki: Virtual_Memory, 'List of virtual memory partitions after boot'] |

The CPU3 cost of a plugin cannot be computed from source. It is unmeasured until PSVshell readings
exist, taken with matched clocks, plugin set and workload [expert].

## 3. The pools

### 3.1 Physical budgets (retail firmware)

Allocated at boot by SceProcessmgr; sizes vary by firmware and boot flags [wiki: Memory_budget,
'List']. Retail values:

| Budget | Size | Who allocates from it | Memblock types | Granularity | Source |
|---|---|---|---|---|---|
| `ScePhyMemPartGame` | 256 MiB (plus a Large Memory Mode extension, section 6.1) | the running game: its modules, stacks, TLS, libc heap, every `USER_RW` block, and every plugin loaded into it | `USER_RW` 0x0C20D060, `USER_RW_UNCACHE` 0x0C208060 (and kernel type `KERNEL_RW_GAME` 0x1050D006) | 4 KiB | [wiki: Memory_budget]; [wiki: SceSysmem_Types, 'Memory Block Type', 'Available memory types']; [SDK: Kernel-Reference, 'sceKernelAllocMemBlock'] |
| `ScePhyMemPartGameCdram` | 112 MiB | the game's GPU data: framebuffers, the libgxm parameter buffer, textures | `USER_CDRAM_RW` 0x09408060 | 256 KiB | [wiki: Memory_budget]; [SDK: Programming-Startup_Guide, 'Video Memory (CDRAM)']; [SDK: Kernel-Reference] |
| `ScePhyMemPartPhyCont` | 26 MiB | physically contiguous buffers: Codec Engine (JPEG/AVC), camera | `USER_MAIN_PHYCONT_RW` 0x0C80D060, `_NC_RW` 0x0D808060 | 1 MiB | [wiki: Memory_budget]; [SDK: Programming-Startup_Guide, 'Main Memory']; [SDK: Kernel-Reference] |
| `ScePhyMemPartShell` | 50 MiB | SceShell: its modules, its PAF heaps, LiveArea, screenshots, notifications; every `*main` plugin; every `*ALL` plugin's pages | the same `USER_*` types, resolved against the Shell's budget | 4 KiB | [wiki: Memory_budget]; [wiki: SceSysStateMgr, 'appspawn'] |
| `ScePhyMemPartShellCdram` | 16 MiB | SceShell's GPU data | `USER_CDRAM_RW` from SceShell | 256 KiB | [wiki: Memory_budget] |
| `ScePhyMemPartKD` | 36 MiB | every kernel driver and every kernel plugin: module pages and `ksceKernelAllocMemBlock` with the kernel types | `KERNEL_RW` 0x1020D006, `KERNEL_RX` 0x1020D005, `KERNEL_TMP_RW` 0x6020D006, their `_NC` variants | 4 KiB | [wiki: Memory_budget]; [wiki: SceSysmem_Types, 'kernel memory types'] |
| `ScePhyMemPartShared` | 8 MiB | shared modules | shared `USER_SHARED_*` types | 4 KiB | [wiki: Memory_budget]; [wiki: SceSysmem_Types] |
| `ScePhyMemPartGameCDialog` | 9 MiB | the common-dialog sub-budget (`CDLG`) of the game | `USER_MAIN_CDIALOG_RW` 0x0CA0D060 and kernel 0x10A0D006 | 4 KiB | [wiki: Memory_budget]; [wiki: SceSysmem, 'sceKernelGetSubbudgetInfo']; [wiki: SceSysmem_Types] |
| "Remaining memory" | 125 MiB | the Large Memory Mode extension of a game, and system use | — | — | [wiki: Memory_budget] |

Together with the 2 MiB of secure DRAM this accounts for the 512 MiB [inference from the listed
bases and sizes; wiki: Physical_Memory]. The partition sizes are chosen per hardware model at
boot, so a PS TV may differ; the sources list no PS TV-specific values (section 6.3).

How the memblock type selects the pool [wiki: SceSysmem_Types, 'memtype bit value']: the low
byte is the access permission (0x60 user read/write, 0x06 kernel read/write), the next byte the
cache policy (0xD0 cached, 0x80 uncached), bits 20-23 the **source physical partition** (0x2 =
the caller's own "normal" budget: the game's for a game, the Shell's for SceShell, KD for kernel
code; 0x4 CDRAM; 0x5 the game budget from kernel code; 0x8 PhyCont; 0xA CDialog) and bits 24-27
the virtual partition it is mapped into (0xC `UserMain`). Only the tabulated combinations are
accepted; a user process asking for a kernel type gets `0x80024B07` [inference from the error name].

### 3.2 Pools inside a process

These are carved out of the budgets above; running out of them gives a different symptom than
running out of pages.

| Pool | Size | Notes | Source |
|---|---|---|---|
| libc heap of a game | 256 KiB by default; the title sets `sceLibcHeapSize` | from the game's budget. A plugin must not rely on the host's `malloc` having room; bring an own allocator over an own `USER_RW` block | [SDK: Memory_Management-Tutorial, 'Configuring the libc Default Heap'] |
| System reservations in a game before `main()` | kernel 3 MiB; preloaded libc 312 KiB, libfios2 160 KiB, apputil 44 KiB; plus the program image | explains why a freshly started game never shows the full 256 MiB free | [SDK: Programming-Startup_Guide, 'Main Memory Size that Can Actually Be Allocated by Applications'] |
| Thread stack and TLS | stack as requested, rounded up to 4 KiB; TLS at least 4 KiB (2 KiB reserved plus the module's `_thread` data, rounded to 4 KiB) | taken from the owning process's budget at `sceKernelCreateThread` time | [SDK: Kernel-Reference, 'sceKernelCreateThread' note]; [SDK: `SDK_doc__System__Kernel-Overview_e.txt`, 'Threads'] |
| Modules loaded with libsysmodule | each module's steady-state size (for example libSceUlt 160 KiB, libScePgf 44 KiB, libSceSqlite 520 KiB, libSceJson 32 KiB) plus 128 KiB transiently while loading; Net, Http, Ssl and Np modules are outside the game budget | from the game's budget | [SDK: `SDK_doc__System__libsysmodule-Reference_e.txt`, 'Module IDs'] |
| ScePaf global heap in SceShell | 5,242,880 B, mapped once at Shell start [measured]; 2.4 MiB free beside CLANNAD, 355-441 KiB and 190 KiB in later sessions [measured] | `sce_paf_memalign` takes from this heap, not new pages. Exhausting it starves firmware's own PAF users (allocations return NULL); it does not produce `0x80024302`. Firmware heaps that *grow* do ask the kernel for pages, and that is what failed in #59 | [measured: `docs/meiki-ocr.md` on branch `experiment/meiki-ocr`; vita-vn-ocr `docs/vita-memory-budget.md` section 2]; [inference; PR #60, `docs/issue-59-root-cause.md` section 1] |
| libheap (`sceHeapCreateHeap`) | caller-chosen; `SCE_HEAP_AUTO_EXTEND` adds further `USER_RW` blocks and returns them when they empty | a libc `mspace` over a `USER_RW` memblock; all calls thread-safe; several small heaps resist fragmentation better than one | [SDK: `SDK_doc__System__libheap-Overview_e.txt`, 'Basic Usage Procedure'; `SDK_doc__System__libheap-Reference_e.txt`, 'sceHeapGetTotalFreeSize'] |
| Kernel per-process heaps | created by SceProcessmgr for every process (one 256 KiB kernel heap among them) | kernel bookkeeping, not under plugin control | [wiki: SceProcessmgr, 'SceUIDProcessObject'] |

### 3.3 Limits per process

- At most 2048 memory blocks and 128 threads per process; exceeding them fails with
  `SCE_KERNEL_ERROR_UID_RL_OVERFLOW`. Objects created by libraries the process uses count too
  [SDK: Kernel-Overview, 'Limitation on Kernel Resources'].
- 64 file descriptors [SDK: Programming-Startup_Guide, 'Number of File Descriptors'].
- Thread stack between 4 KiB and 32 MiB; the start-argument block is copied onto the new stack
  [SDK: Kernel-Reference, 'sceKernelCreateThread'].
- A module's `module_start` may return `SCE_KERNEL_START_NO_RESIDENT`: the module is unloaded at
  once, its pages are freed, and `module_stop` is never called [SDK: Kernel-Overview,
  'module_start Function'].

## 4. Where a plugin's memory comes from

### 4.1 taiHEN sections and process context

- `*KERNEL` loads kernel plugins when taiHEN starts. Any other section name is the title ID of the
  process to load into. SceShell's title is the special name `main` [source: taiHEN README,
  'config.txt'].
- `*ALL` is matched for every section except `KERNEL`, so an `*ALL` plugin is loaded into SceShell
  and into every application and system app that starts [source: taihen-parser `parser.c`,
  `taihen_config_parse`].
- taiHEN loads a user plugin with `ksceKernelLoadStartModuleForPid(pid, path, …)` [source: taiHEN
  `plugin.c`, `plugin_load`]. The module's segments become memory blocks in that process; the
  kernel's memory block service is what loads program modules and allocates thread stacks [SDK:
  Kernel-Overview, 'System Memory Manager']. So a plugin's text, data, bss, stacks, TLS and
  allocations are all charged to the host process's budget. PR #60 measured exactly this for our
  Shell plugin [measured: PR #60, `docs/issue-59-root-cause.md` section 3].
- A plugin that is in several sections must find out itself which process it is in [source:
  taiHEN README, note 3]. Our worker does this with the title ID and gives its pages back
  (`NO_RESIDENT`) outside a candidate game. Since #138 no section names it: the kernel plugin loads
  it into each game that is ON in `games.ini`, from the same hook and in the same way as taiHEN
  loads a section's lines (section 4.3).

| Context | Pool for module pages, stacks, TLS | Pool for `sceKernelAllocMemBlock(USER_RW)` | Other heaps available |
|---|---|---|---|
| `*KERNEL` | `ScePhyMemPartKD`, 36 MiB shared with every driver | `KERNEL_RW` and the other kernel types: KD; `KERNEL_RW_GAME`: the game's budget; CDRAM and PhyCont types: those pools | kernel heaps (`ksceKernelAllocHeapMemory`) [wiki: SceSysmem_Types; wiki: SceSysmem] |
| `*main` (SceShell) | `ScePhyMemPartShell`, 50 MiB, shared with all of SceShell | the same 50 MiB | ScePaf heaps, including the 5 MiB global heap [measured] |
| `*TITLEID` (one game) | that game's `ScePhyMemPartGame`, 256 MiB less what the game uses | the same | the game's libc heap (do not use it) |
| `*ALL` | all of the above, one copy per process | — | — |

SceShell is a user-mode system application spawned with the system-application budget
(`SHELL_BUDGET_ID` 0x4000000) [wiki: SceSysStateMgr, 'appspawn'; wiki: SceProcessmgr, 'Types'].

### 4.2 What the overlay allocates today (API 16, main `e759187`)

| Component | Context | What | Pool | Size | Source |
|---|---|---|---|---|---|
| `VitaJPOverlay_Kernel.skprx` | `*KERNEL` | module pages (`.bss` 78,128 B at API 16, up from 53,008 B at API 13) | KD | about 0.1 MiB [measured] | [PR #60, `docs/issue-59-root-cause.md` section 4] |
| | | capture buffer `VjoCapture`, removed in API 24: region width × height × 4 B, rounded to 4 KiB; after API 19 only the Shell's JPEGs took it, Google Lens's (until #130) and the Anki screenshot's (until #125) (OCR and, since API 23, an Anki card's screenshot read the frame in the game: [ingame-capture.md](ingame-capture.md)); at most 960×544×4 = 2,088,960 B, which #74's 3.65 console refused five times with `0x80024302`. Since API 24 the kernel's only block for a job is the capture ladder's strip, at most 64 KiB for one call | KD, `KERNEL_RW` | 458,752 B at 917×125 until API 24 [measured] | [`kernel/buffers.c`]; [vita-vn-ocr `docs/vita-memory-budget.md` section 1] |
| | | model staging buffer (32 KiB), worker thread stack 16 KiB | KD | static | [`kernel/game_ocr.c`, `kernel/main.c`] |
| `VitaJPOverlay_Shell.suprx` | `*main` | module pages: text 225,280 B + data/bss 180,224 B | Shell partition | 405,504 B permanent [measured] | [PR #60, `docs/issue-59-root-cause.md` section 3.1] |
| | | `VjoNet` stack 16,384 B + 4,096 B TLS (65,536 B until #132); `VjoControl` 16,384 B + 4,096 B TLS | Shell partition | 40,960 B permanent, the ledger's (90,112 B until #132 [measured]) | same; [memory-ledger.md](memory-ledger.md) |
| | | result arena 64 KiB (the local dictionary's; the network's 2 × 256 KiB and connection pool went with Google Lens), Anki save 128 KiB, each behind a 1.5 MiB PAF reserve guard, transient | ScePaf global heap | transient | [`shell/memory.cpp`]; [guide.md, 'Memory and troubleshooting'] |
| | | Anki thread stack 16 KiB + TLS (64 KiB until the network went), only while an Anki job runs (with `anki_mining = on`: counting the queue on every overlay open, saving) | Shell partition | transient | [`shell/anki.c`] |
| `VitaJPOverlay_OCR.suprx` | loaded by the kernel into each game ON in `games.ini` since #138 (`*TITLEID`, one section per game, before; `*ALL` up to the installer's `--title`) | module pages in each enabled game only. Under `*ALL` they were in **every** process, SceShell included: 45,056 B each, until the #59 fix made `module_start` return `NO_RESIDENT` outside a candidate game (section 4.3) | the host's budget | 45,056 B at API 16; 0 outside candidate games since PR #68 [measured]; not loaded outside the registered games since `--title` [source: taihen-parser `parser.c`], nor outside the enabled ones since #138 [source: `kernel/loader.c`] | [PR #60, `docs/issue-59-root-cause.md` section 3.3] |
| | in an enabled game | an Anki card's screenshot (API 23): one block of 450,560 B (`VjoGameShot`: the JPEG encoder, a strip of 64 colour rows, the JPEG) only while it runs, never beside an OCR job's blocks; worker thread 32 KiB stack + TLS; resident weights `VjoGameVocrWeights` 823,296 B (VOCR5-S: Small), 1,187,840 B (VOCR5-N: Normal) or 2,478,080 B (VOCR5-L: Large, with its head gate since API 39; 2,473,984 B before; 659,456, 1,159,168 and 1,560,576 B for VOCR3-FL10, VOCR3-H15 and VOCR4-W20 before VOCR5); per-job workspace `VjoGameVocr`, region-sized (900,896 B at 917×125 for FL10, 1,457,312 B for H15); a reserve kept free before any block: 4 MiB, and since API 21 4, 8 or 16 MiB by the largest model size the job holds [estimate, vita-vn-ocr `docs/model-tiers.md`] | the game's budget, `USER_RW` | per job [measured] | [`game_ocr/main.c`]; [guide.md, 'Memory and troubleshooting'] |

Permanent pages held in SceShell's partition: 540,672 B at API 16 against 503,808 B at API 13;
the 36,864 B growth coincides with the build that stopped booting (#59) [measured: PR #60,
`docs/issue-59-root-cause.md` section 3.1].

**API 19 (#74).** OCR's capture moved into the game: the kernel's `VjoCapture` served only the
Shell's JPEGs, Google Lens's and the Anki screenshot's (after their removal, #130 and #125, nothing took it, and API 24 removed it; since API 23 an Anki card's screenshot is read and encoded in the game, [anki.md](anki.md#the-screenshot)), and the game worker holds a capture block of at most 64 KiB (`USER_RW`, else
CDRAM, else phycont) instead of a region-sized grey plane in its workspace. The H15 workspace is
188,416 B at any region (712,704 B at 960×544 and 303,104 B at 917×125 before); a line taller
than a strip takes its own block only when it occurs. The worker's module is 61,440 B
[measured, packaged SELF]. Figures from the host build's `vjo_vocr_job_bytes` and
`vjo_vocr_capture_bytes`; [ingame-capture.md](ingame-capture.md) has the table.

### 4.3 What this means for `*ALL`

`*ALL` was chosen so one registration covers every game. Its price was the worker's 45,056 B of
pages sitting in SceShell and in every system app, because the title check in `module_start`
returned `SCE_KERNEL_START_SUCCESS` and kept the module resident. Since the #59 fix (PR #68)
`module_start` returns `SCE_KERNEL_START_NO_RESIDENT` outside a candidate game, which unloads the
module at once without `module_stop` [SDK: Kernel-Overview, 'module_start Function'], so `*ALL`
costs SceShell and the system apps no pages. It still loaded the module there at every process
start, only to unload it again.

Before #138 the installer followed the expert's rule: one `*TITLEID` section per game, never `*ALL`
[expert]. `install-vita.sh --title PCSG00415 VITA_IP` (repeatable) registered the worker
under `*PCSG00415`, kept the games registered by an earlier install, and commented out an
`*ALL` worker line; with no game named or registered it changed nothing and said so
(`tools/patch_tai_config.py`'s `lines` mode keeps those rules). taiHEN therefore loaded the worker only into
those games, never into SceShell or a system app [source: taihen-parser `parser.c`,
`taihen_config_parse`], which removes the load itself, not only its pages. `NO_RESIDENT` stays
as the guard for a configuration edited by hand. For Adrenaline's PSP games the guide's Install names
`PSPEMUCFW` and each bubble's own ID, the title IDs of the processes Adrenaline's kernel module
and AdrBubbleBooter's patch [source: Adrenaline `kernel/main.c`, `ksceKernelStartPreloadedModulesPatched`;
AdrBubbleBooter-oss `integration/adrenaline-v7/overlay/kernel/main.c:123-145`]; that taiHEN matches
them is inferred from those sources, not measured. A bubble's `*TITLEID` also loads the worker into the
bubble's own starter app (AdrBubbleBooter's eboot, an ordinary process under that ID) when it runs,
where the worker stays idle until it exits [inference]. The Rubi app appended such a `*TITLEID` section
for a bubble the user turned ON (#124), never `*ALL`. A game added later needed `--title` (or the app)
and a reboot.

**Since #138 the kernel loads the worker.** test.3's Adrenaline run, and AIR without a line of its
own, got `rc=-117`: a game the user had turned ON in the Rubi app had no OCR until a line named
it. The owner's decision (2026-10-16) replaces the per-title rule: the kernel plugin hooks
`ksceKernelStartPreloadingModules`, the function taiHEN loads a process's sections from, and loads
the worker into a starting process whose title is ON in `games.ini` by the Shell's own rules (its
row, a PSP game's shared `NPXS10028` row, the default), with the call taiHEN uses for a config line
[source: taiHEN `hen.c:360-388`, `plugin.c:165-175`; Adrenaline `kernel/main.c:123-136` loads its user
module into the PSP emulator the same way]. It never loads it into SceShell, a system app
(`NPXS*` but `NPXS10028`), VitaShell or the Rubi app, nor while L is held at the start, so the
worker's pages go only where they did with the lines, and into games without one. A game that a
line still names keeps one copy (the worker checks for an older copy of itself); the installers write
no worker line and remove the ones older installs left ([one-vpk-install.md](one-vpk-install.md)). The prior art, the
lifecycle (start, suspend and resume, exit, crash, a title turned OFF or ON while it runs, the
Rubi app beside the game) and the memory are in [worker-loading.md](worker-loading.md).
With `default = enabled` homebrew gets the worker too (an estimate of the cost: its 98,304 B of
pages, 102,400 B in the debug build since API 32, and 36,864 B of stack and TLS, from the game's own budget); one that cannot start with them is
turned OFF or started with L held.

Permanent pages in SceShell's partition after the fix: 389,120 B (Shell module 299,008 B, the two
thread stacks with TLS 90,112 B, worker 0 B), against 540,672 B at `e759187` and 462,848 B at
v0.7.4, the last build that ran beside a game [measured: `docs/memory-ledger.md`]. Removing the
online dictionaries (jpdb.io, jiten.moe) brought it to 376,832 B (Shell module 286,720 B), and
removing the Hachidori relay client to 372,736 B (Shell module 282,624 B), AnkiConnect to
356,352 B (Shell module 266,240 B) and Google Lens, with the Shell's capture path and the worker's
network pipeline, to 266,240 B (Shell module 176,128 B) [measured: the build log's module row], and
VjoNet's stack, 64 KiB until #132 (it was for TLS), is 16 KiB: 217,088 B [the ledger's sum]. The ledger
([memory-ledger.md](memory-ledger.md)) fails CI if any of these grow.

### 4.4 How other plugins stay inside the budgets

The 19-project audit ([plugin-audit.md](plugin-audit.md), per-project notes in
`docs/plugin-audit/notes/`) read each plugin's allocations. The patterns that have shipped for
years, by context [source: the audit notes, with file:line]:

| Plugin | Context | What it holds, where | Pattern |
|---|---|---|---|
| PSVshell | `*KERNEL`; draws into the game's framebuffer from its display hook | one 308×344×4 = 416 KiB `KERNEL_RW` block allocated once in `module_start`, a 12 KiB kernel thread; nothing in SceShell or the game, nothing after boot | one fixed kernel block at boot; the game's budget is untouched |
| VitaGrafix | `*ALL`, patches the game; inert in VitaShell and unsupported titles | about 8 KiB of statics, no dynamic allocation, no thread; its OSD hook releases itself after five seconds | zero allocation; the plugin gets out of the way |
| pngshot | `*main`, replaces SceShell's screenshot encoder | PAF heap (`sce_paf_private_malloc`) only while a screenshot is encoded, freed at once; nothing between screenshots | transient PAF use, zero resident |
| VitaTweaks | `*main` | 56 B of statics, offset hooks and injects | zero resident |
| vitacompanion | `*main`, no hooks | a 1 MiB `taipool` heap and a 64 KiB thread from `module_start` (about 1.1 MiB resident), up to 3.4 MiB at peak during `install` | the largest `*main` resident footprint in the set: consistent with the expert's "well under 2 MiB" ceiling, and with that ceiling being nearly used up by one such plugin |
| ds4vita | `*KERNEL`, the same SceCtrl/SceTouch hooks the overlay uses | a 256 B kernel heap and a 4 KiB thread; patches the caller's buffers in place | nothing in SceShell or games |
| gpues4 profiler | `*KERNEL` + client library in the app | allocates nothing; state in static `.bss`; the client's rings are the app's own arrays; counts drops instead of allocating | zero allocation, fail-safe on full |

The common rule: in-game overlays either own one kernel block from boot (PSVshell) or allocate
nothing (VitaGrafix); `*main` plugins either hold nothing permanent (pngshot, VitaTweaks) or one
fixed pool from start (vitacompanion), and none of them allocates in SceShell while a game runs.
The overlay is the only one in the set that combined a resident SceShell footprint, PAF
allocations during a game, dynamic kernel blocks and in-game allocations; the ledger and the fix
move it toward the common pattern (zero worker pages in SceShell, the UI arena transient), and
the follow-up (dictionary and Anki into the game process, `*TITLEID`) completes it.

## 5. Safe budgets per context

### 5.1 SceShell (`*main`): there is no spare memory

Facts:

- The partition is 50 MiB; the system owns it. Two console samples beside a running game:
  98,304 B free of 52,428,800 B, and 28,672 B free (ownership of the second sample not verified)
  [measured: vita-vn-ocr `notes/26`, `notes/08`].
- Our eager 2 × 384 KiB `USER_RW` result allocation in SceShell failed with `0x80024302` (fixed by
  #11) [measured: vita-vn-ocr `notes/06`].
- Adding one 65,544 B static buffer to the Shell plugin hung the boot twice; removing it fixed it
  (#42) [measured].
- The LiveArea daily refresh (six most recently started titles, once a day when enabled) runs in
  SceShell [SDK: `SDK_doc__System__LiveArea-Specifications_e.txt`, 'Update Timing']; LiveArea
  images are decoded into memory (up to 1 MiB per screen) and the specification itself warns that
  fragmentation can make them fail to load [same, 'LiveArea Image Data']. The #59 thread
  `SceLiveAreaDailyCronJob` is this job [inference].
- The system screenshot (PS + Start) is taken by the system software as a 960×544 JPEG [SDK:
  `SDK_doc__System__Screenshot-Overview_e.txt`, 'Operation of the Screenshot Feature']. A
  960×544 frame is 2,088,960 B; JPEG encoding on the Codec Engine needs physically contiguous,
  256-byte-aligned output and colour-conversion buffers of about one byte per pixel plus the
  YCbCr planes [SDK: `SDK_doc__Audio_Video__scejpegenc-Overview_e.txt`, 'Buffer Restrictions',
  step (1)]. Where SceShell takes that from is undocumented; it is the headroom the expert says
  SceShell wants for itself [expert].
- A Shell plugin competing with SceShell has a budget of genuinely under 2 MiB, and even that is
  not ours to use [expert].

Rules:

- No OCR workspace, model weights, dictionary pages, screenshots or JPEG work in SceShell. Ever
  [expert].
- The permanent footprint (module pages + stacks + TLS) is the only thing we hold there, and it
  must shrink, not grow: every kilobyte of `.bss` is a page SceShell's own threads cannot get.
  Current: 540,672 B [measured]; target: the UI's code and a few KiB of state, with page-sized
  buffers allocated only while a page is open and freed when it closes.
- No `sceKernelAllocMemBlock` from the Shell plugin after boot. Transient needs go through the PAF
  global heap behind the reserve guard (1.5 MiB today), stay small (64 KiB) and are freed before
  the thread ends.
- One control thread. Every extra thread costs at least 8 KiB (4 KiB stack + 4 KiB TLS) of
  partition pages for its whole life.
- Any growth of the Shell module is a boot-path change and needs the boot-safety guard and the
  ledger (vita-memory-deep items 3 and 4) before it ships.

### 5.2 The game process (`*TITLEID`): preflight, reserve, free after

Facts:

- The budget is 256 MiB minus the game's own use. CLANNAD had 91,226,112 B free before a job
  (one sample) and 87 MiB in another session [measured: vita-vn-ocr `notes/27`, `notes/26`].
  Stardew Valley in the +109 MiB mode: about 56 MiB free (a third-party PSVshell screenshot, not
  our measurement).
- Few games allocate the full 256 MiB unconditionally; some get close; homebrews often do
  [expert]. The free figure is the game's, changes while it runs, and can be consumed by the game
  between our check and our allocation.
- `sceKernelGetFreeMemorySize` reports `sizeMain`, `sizeCdram` and `sizePhycont` separately, and
  each is the real free size minus a system margin [SDK: Kernel-Reference,
  'sceKernelGetFreeMemorySize', 'SceKernelFreeMemorySizeInfo']. Check the field for the pool you
  will allocate from (`sizeMain` for `USER_RW`).
- Allocation granularity is 4 KiB, so round every request up before comparing [SDK:
  Kernel-Reference, 'sceKernelAllocMemBlock'].
- The game worker today: free `sizeMain` − the reserve (4, 8 or 16 MiB by model size since API 21)
  must cover the page-rounded weights and workspace, or that size is refused before anything is
  allocated and the next smaller one is tried [`game_ocr/main.c`].

Rules:

- Preflight every allocation, keep a reserve (4 MiB today; [estimate] it has not been tuned on
  more than one game), allocate the whole job's memory in as few blocks as possible, free it when
  the job ends, and treat a failed `sceKernelAllocMemBlock` after a passed preflight as normal
  (the game moved first).
- Resident data (the weights, the LM, the cascade's model) must be dropped once the game has not
  been the active one for 5 s of its running time (`game_ocr/main.c` `inactive_long`: another app in
  front of a running game, the title disabled, the Shell restarted), on any refused allocation and
  at exit [guide.md, 'Memory and troubleshooting']. The 5 s are process time, which stops while the
  process is suspended [SDK: Kernel-Reference, `sceKernelGetProcessTimeWide`: "Process time stops
  during process suspension and system suspension periods"], so a suspend and resume keeps them,
  as a moment's change of foreground does; they stay in the game's own budget, which system apps
  and the common dialogs do not draw on (their own budgets: sections 5.4 and 3.1). Before
  test.3 they went at once, and #81's run read H15 four times in one game process
  (docs/first-ocr.md). A job that found no line reads them if the job's memory check allowed it
  (the cascade's model included, before any line is doubted): they are held from then on.
- Never use the host game's `malloc` or libc heap (256 KiB default) [SDK:
  Memory_Management-Tutorial, 'Configuring the libc Default Heap'].
- Worker threads: default priority and `SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT` put a thread
  in the common ready queue where the scheduler balances it across the game's three cores; to
  pin it, name exactly one core and use an individual-queue priority. Mixed settings cause
  priority inversion and hard-to-reproduce bugs [SDK: Kernel-Overview, 'Notes on Priority and
  CPU Affinity Mask Combinations'].
- Never busy-wait: it blocks the idle thread's power saving, can deadlock lower-priority driver
  threads under fixed-priority scheduling, and heats the device; thermal load itself triggers an
  automatic suspend [SDK: `SDK_doc__System__Power-Overview_e.txt`, 'CPU Busy Loop', 'Automatic
  Suspension'].

Per-job sizes for the recognizer and the tiers that follow from them are in vita-vn-ocr's
[`docs/vita-memory-budget.md`](https://github.com/bee-san/vita-vn-ocr/blob/main/docs/vita-memory-budget.md);
the short platform note there is
[`docs/vita-platform-memory.md`](https://github.com/bee-san/vita-vn-ocr/blob/main/docs/vita-platform-memory.md).

### 5.3 The kernel plugin (`*KERNEL`): a shared 36 MiB pool of unknown free size

Facts:

- Kernel module pages and `KERNEL_RW` blocks come from `ScePhyMemPartKD`, 36 MiB for all kernel
  drivers together [wiki: Memory_budget; wiki: SceSysmem_Types]. How much is free on a retail
  3.65 console is unmeasured.
- Our 2,088,960 B full-frame capture buffer failed there once with `0x80024302` (fixed by the
  region-sized capture, #14) [measured: vita-vn-ocr `docs/vita-memory-budget.md` section 2], and
  again five times on the owner's PCH-1001 on 3.65 (#74) [measured]. Since API 19 OCR takes no
  kernel buffer: the display hook writes the region's grey rows into the game worker's own
  `USER_RW` block, at most 64 KiB, a strip at a time ([ingame-capture.md](ingame-capture.md)).
  API 24 removed the full-frame buffer. The screenshot-saving fix (#207) adds a separate
  16 KiB kernel file-worker stack, with no new image buffer or Shell pages: the kernel pool's
  declared worst case is 241,664 B (debug build), 237,568 B (normal build)
  [source: `include/vjo_ledger_budget.h`; memory-ledger.md]. These are accounted sizes,
  not console headroom measurements.
- PSVshell reads the foreground process's budget (main, CDRAM, phycont: free and total) from the
  kernel's address-space information of the current context [source: PSVshell `src/perf.c`,
  `psvs_perf_poll_memory`]. Its numbers are the game's or the Shell's, whichever is in front;
  they are not the KD pool.

Rules:

- Allocate capture buffers per job, region-sized, and free them when the reader is done. Never
  hold a full-frame buffer.
- Keep static kernel state small; the kernel module's `.bss` is permanent KD memory.
- Prefer copying into the game's `USER_RW` memory (the game owns and frees it) over kernel-held
  copies.
- Kernel code must not dereference user pointers; copy with the `ksceKernelMemcpy*` helpers
  [wiki: Kernel, 'Memory Domains'].
- The user API `sceKernelGetFreeMemorySize` reports only the caller's own budget. KD headroom
  has to be read kernel-side, from the partition information (`sceKernelGetPhyMemPartInfo*`
  on the wiki; PSVshell reads the address-space information the same way) [wiki: SceSysmem;
  source: PSVshell `src/perf.c`]. The self-test (vita-memory-deep item 5) is where that sample
  belongs.

### 5.4 The Rubi app: a system application

The Rubi app (`settings/`, `VJOSET001`) is its own process, not a plugin. Its SELF is a system
application (`vita-make-fself -at 0x0E -m 0xC800`, `vita/CMakeLists.txt`), as
[VitaShell-sysapp](https://github.com/shoui520/VitaShell-sysapp),
[jmdict-vita](https://github.com/shoui520/jmdict-vita) and
[vita-content-manager-pro](https://github.com/shoui520/vita-content-manager-pro) are: it starts
beside a suspended game instead of ending it. The kernel keeps a separate budget for system
applications [wiki: SceProcessmgr, 'Types': the system-application budget pseudo-UID 3], and the
SELF says how much of it the app asks for [source: vita-toolchain `vita-make-fself`, whose `-m` is
0x1000-0x12800 KiB for a system-mode app]. libvita2d treats CDRAM as unavailable there [source:
libvita2d a8f15ab `vita2d.c`, `vita2d_init_internal`]; whether the kernel grants such a process
any CDRAM was not checked [inference]. The SELF requests 50 MiB (`-m 0xC800`) and no physically
contiguous budget (no `-pm`). The memory outside the fixed budgets serves both a game's extended
mode and the system [wiki: Memory_budget, "Remaining memory (for Game +109MiB mode and system)"];
that the system-application budget comes from it, so that a game in an extended mode may have to
close first, is an inference [inference].

- **CPU and priority.** The main thread runs on CPU3 (`SCE_KERNEL_CPU_MASK_SYSTEM`) at priority
  `0x10000100`, set in `settings/sysapp.c`. Every other thread the app creates does too: the link
  wraps `sceKernelCreateThread` (`--wrap`), and the wrapper gives each thread CPU3 and, for an
  application priority, the main thread's [source: as VitaShell-sysapp's `sysapp.c`]. CPU3 use is
  unmeasured (rule 8).
- **The build checks it.** `tools/check_settings_self.py` (the Vita build's `vjo_settings_self`,
  which the release needs) reads the built `eboot.bin` [measured: in the packaged binary]: control
  info 6's attribute 0x0E, its 0xC800 KiB budget and no phycont, the process parameters' priority
  and CPU mask, and, in the ELF, the heap, its sum with the segments and the table's fixed rows
  (an [estimate]) within the budget, and every `sceKernelCreateThread` call inside the wrapper.
  Each of #109's audit mutants (no `--undefined`, no `ATTRIBUTE`/`MEMSIZE`, no `sysapp.c`, a
  64 MiB heap, priority 0xA0, a worker off CPU3) used to build and package; each now fails the
  build, or for the worker, gets CPU3 from the wrapper.
- **What the console gives it.** At start the app writes one line to `log.txt` (with
  `log_file = on`, the default): the budget `sceAppMgrGetBudgetInfo` reports (its mode, main
  memory total and free, phycont, CDRAM) and the heap. After its first 60 frames it writes their
  interval and the main memory then free. No console has run this build: those lines are the
  measurements for this section's estimates.
- **Heap.** newlib's default heap is 128 MiB [measured: `_init_vita_heap` in the toolchain's
  `libc.a`], which does not fit; `settings/sysapp.c` fixes it at 8 MiB. The app's own code does
  not `malloc`; the heap serves libvita2d's structures and GXM host memory, libpng and ScePvf's
  library. The PVF glyph atlas is a 512 x 512 one-byte texture, a 256 KiB `USER_RW` block, not
  heap [source: libvita2d a8f15ab `vita2d_pvf.c`, `texture_atlas.c`].
- **Graphics.** libvita2d detects the system-application budget (`sceAppMgrGetBudgetInfo`), draws
  to the shared framebuffer through `sceGxmVshInitialize`, and allocates its textures in
  `USER_RW` [source: libvita2d a8f15ab `vita2d.c`, `vita2d_init_internal`]. The app's icon textures (at most 9 of
  at most 256 KiB, `settings/icons.h`) therefore come from main memory, not CDRAM.
- **Frame rate.** There `vita2d_end_drawing` waits for a vblank and the app's loop waits for
  another, so it likely draws at 30 Hz [inference: libvita2d a8f15ab `vita2d.c`; the start log's
  frame line measures it]. The left stick's repeat goes by the clock (`core/stick_scroll.h`: a row,
  166.7 ms, then 60 rows a second), so it moves as fast at 30 Hz as at 60; the d-pad's repeat
  always did. The icons still load one a frame.

Main memory, by source [estimate: libvita2d's buffer sizes and the app's static arrays, not
measured on the console]:

| Use | Size |
| --- | ---: |
| GXM rings (VDM 128 KiB, vertex 2 MiB, fragment 512 KiB, fragment USSE 16 KiB), depth and stencil (2 x 2,088,960 B), shader patcher (3 x 64 KiB), index and clear buffers (132 KiB), parameter buffer (2 MiB), temporary pool (1 MiB): 10,424,320 B | about 9.9 MiB |
| newlib heap (reserved whole) | 8 MiB |
| Icon textures (five current-page icons, at most 256 KiB each, `settings/icons.h`) | at most 1.25 MiB |
| Anki export arena (2 MiB, `settings/anki_export.h` `VJO_EXPORT_MEM`; the app has no SceNet pool since the AnkiConnect page was removed), config scratch, game list | about 2.2 MiB |
| PSP rows (`settings/psp.c`): a bubble target's blocks and PARAM.SFO, two taiHEN configurations | 306,800 B |
| Code, stacks, PVF atlas | about 2 MiB |
| **Total** | **about 25 MiB of 50 MiB** |

The Games search uses a bounded 257-byte UTF-8 query and two 65-unit UTF-16 buffers on the
app's stack (`settings/search.c`), not the Shell or a game's memory. The native IME module is
loaded only during text entry when not already loaded, and released afterwards; its additional
runtime memory is unmeasured on a console. These sizes come from the source, not console samples.

Not in the table: libgxm's own memory for the render target (libvita2d passes no driver memory
block, `driverMemBlock = -1`) is unmeasured, and where `sceGxmVshInitialize` places the 2 MiB
parameter buffer was not checked; for a game it must be CDRAM (§8).

#### Starting with no game running

The owner (2026-10-16 21:33Z): "I can't open the Rubi app first. First I have to open a game,
run the overlay, and then open the Rubi app." test.3 (v0.8.0-test.3) is the first test build
whose app is a system application. Its app writes `Rubi app: start: …` at the top of `main()`,
before its graphics start (`settings/main.c`, then and now; since #158 the Install page's line
follows it), so a start that reaches `main()` leaves that line. test.3's collect (07:17Z, 14 h before the report) has one start line in its boot
(`log.txt` 11345, beside CLANNAD), followed by its frames line, and no start line without one:
no start of that build with no game had reached `main()` by then, and the collect cannot show the
owner's failure either way [measured: test.3's collect]. Whether the owner's opens with no game
reach `main()` at all is not known: test.4's start line answers it. None of the ten crash dumps
on the card is from a test build (all ten were there at test.1's collect); the app's two are
v0.7.7's, fixed in v0.7.8 (`t3-crash-dumps`). The lines are written once per start: an app that
is only resumed writes none (test.3's was in front six times, `log.txt` 11344-12132, and wrote one
`start:` line; `kernel.txt` has one process for it), so each check needs a new start, the app
closed from its LiveArea page first.

The app's own code waits on no game: the kernel's worker loader and input and display hooks pass
it by (`kernel/loader.c` `vjo_game_is_candidate`, `kernel/input.c`, `kernel/display.c` act for
`g.game_pid` only), and the Games list, the OCR & lookup page, the Anki export and the Install
page read and write the memory card alone [source]. libvita2d's start as a system application
does wait on SceShell, in three places [source: libvita2d a8f15ab `vita2d.c:233-262` and
`:859-860`, and the pinned `libvita2d.a`'s disassembly: flags `0x0A`, 2 pending swaps, a 2 MiB
parameter buffer, a read loop with no pause between its tries]. The first two come before its
first frame, and a possible cause of the owner's report, not seen on a console, is in them:

- **The GPU library** starts with `SHARED_SYNC | SHAREDPB_OPEN` (`0x0A`): it opens the
  parameter buffer another process made, and libvita2d keeps no result. Refused, the library
  stays down and the rings, the render target and the shaders fail after it [source:
  shoui520/psp2 `src/os0/us/libgxm_es4/source/init.c` and `gpu_es4/source/client_render.c`, a
  reimplementation of the firmware: `SHAREDPB_OPEN` allocates no buffer and a render context
  without a shared one fails; it is a guide, not proof: it refuses `CREATE | SHAREDPB_OPEN`
  together, which vitaGL has used for system applications since 4305277, 2026-03]. Who creates
  that buffer, and whether it exists when no game runs, is not known [unmeasured].
- **The shared framebuffer**: libvita2d opens it, reads it and, while its buffer index is 1,
  closes it and tries again, with no pause. The index is SceShell's selected buffer, one-based,
  from the render info SceShell hands over (appmgr's `sceSharedFbUpdateProcessBegin` copies it
  into the framebuffer's information, index at 0x34) [source: shoui520/psp2
  `appmgr/source/sharedfb.c`], so it moves only when SceShell's thread that hands it over runs.
  That the app's own read returns it as it is, and that it is 0 with no second buffer before
  SceShell's first hand-over, are inferences: the reimplementation leaves the user calls as
  stubs, and SceShell may hand over from boot, so that case may never occur [inference]. The
  app's main thread runs on CPU3 at 96 (`0x10000100` is 96 in a mini application [wiki:
  SceKernelThreadMgr, "Thread Priority"; source: psp2 `threadmgr/source/thread_create.c`]; that a
  system application is one is an inference, which the start line below measures). Of SceShell's
  113 thread names in the eight SceShell dumps in test.3's collect (builds of Oct 10-13; all on
  CPU3, the Shell's default, but three that may run on any CPU and SceMtpUdcdMonitoring, at 96, on
  CPUs 0-2 only), 33 run above 96 (SceGxmDisplayQueue 64, SceShellMain 76, SceShellSvcServer 80,
  ScePafTextThread0 86), 30 at 96 and 50 below (SceShellAppExecChk and ScePafJobDefault at 106)
  [measured], and the scheduler gives a CPU to the highest-priority ready thread, with no time
  slicing between equals [SDK: Kernel-Overview, "Thread Scheduling"]: while the loop spins,
  SceShell's threads at or below 96 on CPU3 do not run. Which SceShell thread hands over is not
  known: if it runs above 96, the spin does not stop it [unmeasured]. A read before any hand-over
  ends the loop with a NULL buffer to draw into. GrapheneCt's libvita2d_sys (6c3edf6,
  `vita2d.c:989-996`) pauses 40 us in the same loop.
- **The frame** (the third, after the start): `vita2d_start_drawing` waits in
  `sceSharedFbBegin` for SceShell's turn at every frame, with no bound and no log line. It
  blocks in the kernel, so SceShell's threads run meanwhile [inference: the reimplementation
  leaves the user call a stub; its kernel side has an event flag that SceShell's update sets,
  `sharedfb.c`]; a start stopped there leaves the hand-over's line below and, in the debug build,
  no `frames:` line.

Beside a game the start worked [measured: test.3's app drew its 60 logged frames]; how many reads
its hand-over took was not logged, and which of these stops a start with no game, if any, is not
known [unmeasured].

The app now wraps libvita2d's two calls before its first frame (`settings/sysapp_gfx.c`; the
link's `--wrap=sceGxmVshInitialize,--wrap=sceSharedFbGetInfo`, which `tools/check_settings_self.py`
checks the built ELF for):

- a refused shared parameter buffer is followed by one of the app's own, the same flags with
  `PB_LPDDR` instead of `SHAREDPB_OPEN` (`0x03`): main memory, as a system application has no
  CDRAM (test.3 logged its CDRAM as `0 of 16777216 B` free), the 2 MiB already in the table above
  [estimate: that it fits; test.3's app had 28,286,976 B free 60 frames after its start,
  measured]. Refused again, the app closes with a `log.txt` line instead of drawing with a library
  that never started;
- a read libvita2d would take but cannot draw with (no framebuffer, a failed read, no second
  buffer: index 0 before SceShell's first hand-over), or one it retries (buffer 1 still the last
  one), is given to it as buffer 1 after a 1 ms pause, in which SceShell's threads at or below the
  app's priority have CPU3; libvita2d then closes the framebuffer and opens it again. A wait over
  a second is logged. After 10 s the app closes with a line that names the last read: below
  appmgr's own 20 s, after which SceShell aborts an app that does not end its frame
  (`sharedfb.c` `sceSharedFbUpdateProcess`), so the app's line is written first [inference]. A
  hand-over slower than 10 s, which the unchanged start would have waited for, is closed too; the
  line says so, and test.4's figures will say whether the bound should move;
- every read libvita2d takes and can draw with goes to it unchanged, so beside a game nothing
  changes but one `log.txt` append, the hand-over's line.

That line, in both builds, says how the start went, with the main thread's priority and CPU mask
as the console reports them:

```text
Rubi app: graphics: system application; GPU library 0x0A: 0x00000000; display: buffer 2 at read 1 after 0 ms; main thread priority 96, CPU mask 0x00080000
```

That is the host model's line beside a game (`host/tests/test_settings_sysapp_gfx.c`); a console
has not logged it yet. What a start with no game leaves in `log.txt`, and what each says:

| `Rubi app:` lines of the open | Where it stopped |
| --- | --- |
| no `start:` line | before its first line: the launch, or reading `games.ini` and `config.ini` (a crash dump would say); not libvita2d or these wrappers |
| `start:`, no `install page:` line | in the Install page's read of the card, before the graphics start |
| `start:` and `install page:`, no `graphics:` line | in the version check's reads (`settings/app_check.c`), the GPU library's start or the framebuffer's first open (or a crash there: a dump would say) |
| `graphics: waiting for the display …`, then `graphics: no display from the system in 10 s …; closing` | no hand-over in 10 s: the line names the last read |
| `graphics: waiting for the display …`, then nothing | closed, by the tester or the system, before its 10 s |
| `graphics: the GPU library refused the app …; closing` | the shared and the app's own parameter buffer refused |
| `graphics: system application; …`, then (debug build) no `frames:` line | after the hand-over: most likely at the first frame's `sceSharedFbBegin`; else in the rest of libvita2d's start (its results unchecked), the font's load (which closes the app with no line, `settings/main.c`) or the Games list's scan. Whether the app closed by itself or stayed on its start-up picture helps tell them apart |
| `graphics: system application; GPU library 0x0A: 0x00000000; display: buffer 2 at read 1 after 0 ms; …`, and the app opens | it opened, but neither wrapped dependency applied: the owner's failure is then neither reproduced nor explained |
| `… then its own parameter buffer 0x03 …` or a large read count or time, and the app opens | the dependency these wrappers handle: the shared buffer missing, or the hand-over waiting on SceShell |

The host tests run libvita2d's start (a copy of the pinned one's,
`host/tests/sysapp_gfx_model.c`) through the same wrappers against a model of SceShell and the GPU
library: no hand-over for 5 ms, none yet (index 0), a framebuffer that does not open, no shared
parameter buffer, no first buffer, all of them, none for ever, both buffers refused, a wait of 1.5 s
and of 7 s after a slow start. Without the wrappers 11 of their 14 fail; the three that pass pin
what must not change (beside a game, a read libvita2d takes and can draw with, an ordinary
application) [measured: on the host]. Vita3K never takes this path (its
`sceAppMgrGetBudgetInfo` refuses), so its settings smoke also builds the app with that call
answered as a system application and the hand-over and the shared buffer stood in for
(`tools/vita3k_settings_smoke.sh`, `VJO_SETTINGS_SMOKE_SYSAPP`): a crash check of the app's code on
that path, not evidence about the console (AGENTS.md rule 5). CPU3: the wait adds a pause, not
load; its cost is unmeasured (rule 8).

## 6. Game memory modes, free memory, and the PS TV

### 6.1 Memory modes

- An application on a retail console gets 282 MiB of main memory: 256 MiB ordinary plus 26 MiB
  physically contiguous, and up to 112 MiB of CDRAM [SDK: Programming-Startup_Guide, 'Main
  Memory (LPDDR2 DRAM)', 'Video Memory (CDRAM)'].
- The Large Memory Mode in `param.sfo` (App Setting (2), the `ATTRIBUTE2` field) extends the main
  memory by +29 MiB, +77 MiB or +109 MiB. The mode is fixed per title and cannot change at run
  time [SDK: Programming-Startup_Guide, 'Memory Expansion Mode']. The owner's "333 MiB mode" is
  the +77 MiB mode (256 + 77); +109 MiB gives 365 MiB, which matches the "365 MB" PSVshell showed
  for Stardew Valley [inference].
- The extension comes from the 125 MiB "remaining" region, which the system also uses [wiki:
  Memory_budget]. While a title in an extended mode runs, the home screen refuses to launch
  most other applications; launching one anyway ends the running title after a confirmation;
  +77 MiB excludes the modal browser, +109 MiB also excludes the Settings application and the
  store [SDK: Programming-Startup_Guide, 'Limitations When Using the Memory Expansion Mode';
  `SDK_doc__1st_read__System_Software-Overview_e.txt`, 'Mini Settings for QA Application'].
- CLANNAD's `param.sfo` has `ATTRIBUTE2 = 0`: the default 256 MiB mode [measured: vita-vn-ocr
  `notes/26`]. The overlay does not change any game's mode.
- If memory problems persist even with every title forced into the 333 MiB mode, the code is in
  the wrong process context [expert].

### 6.2 What games leave free

| Title | Mode | Free main memory seen | Source |
|---|---|---|---|
| CLANNAD (PCSG00415) | default | 91,226,112 B before a job; about 87 MiB in another session | [measured: vita-vn-ocr `notes/27`, `notes/26`] |
| Stardew Valley | +109 MiB | about 56 MiB (309 of 365 MB used) | third-party PSVshell screenshot, PSVshell README; not our measurement |
| other Japanese visual novels | — | unmeasured | — |

Two samples are not a distribution. The self-test (vita-memory-deep item 5) and the per-job log
line `game vocr memory: free USER=…` are how the table grows.

### 6.3 PlayStation TV

The DevKit has a "PS TV Emulation" boot parameter and the Power service's Mode C brightness limit
does not apply on PS TV [SDK: System_Software-Overview, 'Boot Parameters - PS TV Emulation';
Power-Overview, 'Frequency of the GPU Clock']. No PS TV-specific memory budget appears in the
SDK documents or the wiki; the Shell partition is sized from the model information at boot
[wiki: SceSysmem, 'sceKernelSysrootGetModelInfoForDriver']. Until measured, assume the retail
figures above and mark PS TV results as unverified.

## 7. Physical pages, granularity and fragmentation

- A memory block is a reservation of virtual addresses in a virtual partition, backed by physical
  pages taken from the type's physical partition, then mapped. The physical partition running
  dry is what fails, even though `UserMain` spans more than a gigabyte of virtual addresses
  [inference from wiki: SceSysmem, 'sceKernelAllocPhyPageForKernel'; wiki: Virtual_Memory].
- `0x80024302 SCE_KERNEL_ERROR_NO_FREE_PHYSICAL_PAGE` (short code C1-2569-2) means "cannot
  allocate a physical page": the caller's partition has no free page [SDK: Kernel-Reference,
  'Error Codes'; `SDK_doc__1st_read__Error-Overview_e.txt`]. It is not a heap-full condition and
  not a virtual-address problem. In SceShell it means the 50 MiB are gone; in a game, the 256 MiB.
- Main-memory blocks are 4 KiB-granular and not necessarily physically contiguous (the kernel can
  report up to 16 physical ranges for one block) [SDK: Kernel-Reference; wiki: SceSysmem_Types,
  'SceKernelMemBlockInfoEx']. So fragmentation of the physical partition rarely blocks a normal
  `USER_RW` request: any free pages will do. It does block 1 MiB physically contiguous blocks and
  256 KiB CDRAM blocks, which need runs of free space.
- Heap fragmentation is the other kind: inside a PAF heap, libheap heap or libc heap, free bytes
  can be present but not contiguous. LiveArea's own specification names this as a reason images
  fail to load in SceShell [SDK: LiveArea-Specifications, 'LiveArea Image Data']; libheap's
  answer is several heaps rather than one [SDK: libheap-Reference,
  'sceHeapGetTotalFreeSize']. Our PAF allocations in SceShell add to this pressure even when they
  are freed.
- Allocation through the kernel is slow and coarse, so the SDK's advice is to allocate large
  blocks up front and sub-allocate [SDK: Memory_Management-Tutorial, 'Allocation Strategies'].
  For a plugin in someone else's process the same advice reads: one block per job, not one per
  object, and never a growing pool.

## 8. GPU-mapped memory and the display path

What the capture hook touches:

- The displayed framebuffer must be in CDRAM, its base 256-byte aligned, its pitch a multiple of
  64 pixels, in the only documented format A8B8G8R8 (4 bytes per pixel), at 960×544 or one of the
  smaller resolutions the hardware scales (480×272, 640×368, 720×408) [SDK:
  `SDK_doc__Graphics__Display-Overview_e.txt`, 'Basic Usage Procedure';
  `SDK_doc__Graphics__Display-Reference_e.txt`, 'sceDisplaySetFrameBuf', 'SceDisplayFrameBuf'].
  A 960×544 frame with pitch 1024 is 2,228,224 B of CDRAM; the pixels alone are 2,088,960 B.
- The CPU reads CDRAM uncached, with long latency; copy regions out in bulk, never pixel by pixel
  [SDK: `SDK_doc__Graphics__libgxm-Overview_e.txt`, 'Allocating Memory', Table 9;
  `SDK_doc__Graphics__GPU-Users_Guide_e.txt`, 'Avoid Frequent Video Memory Access from the CPU'].
- `sceDisplayGetFrameBuf` returns only the caller's own last framebuffer [SDK: Display-Reference,
  'sceDisplayGetFrameBuf']. A SceShell plugin cannot see the game's frame that way; the kernel
  can, from the `sceDisplaySetFrameBufInternal` hook (our `kernel/display.c`) or from
  `sceDisplayGetProcFrameBufInternalForDriver(pid, …)` and the DMAC/IFTU capture functions
  [wiki: SceDisplay]. SceDisplay keeps two framebuffer slots per head: 0 for the game, 1 for the
  LiveArea/PS-button overlay [wiki: SceDisplay, 'Types'].
- libgxm's display-queue callback runs on an internal libgxm thread, must flip and then block
  until the old buffer is off screen, must not call any libgxm context or synchronization
  function, and its copied callback data is limited to 512 B in total [SDK:
  `SDK_doc__Graphics__libgxm-Reference_e.txt`, 'SceGxmInitializeParams',
  'sceGxmDisplayQueueAddEntry']. A hook there inherits those limits; the display-service hook
  gives a standard structure instead.

What the game's GPU memory costs (so nobody mistakes it for free memory):

- The parameter buffer must be in CDRAM; its default size is 16 MiB [SDK:
  Memory_Management-Tutorial, 'CDRAM'; libgxm-Reference, constants].
- Default ring buffers: VDM 128 KiB, vertex 2 MiB, fragment 512 KiB, fragment USSE 16 KiB [SDK:
  libgxm-Reference, constants].
- `sceGxmMapMemory` maps whole 4 KiB pages of `USER_RW`, `USER_RW_UNCACHE`, `USER_CDRAM_RW` or the
  PhyCont types; render targets need their own unmapped block; USSE-mapped (shader code) address
  space is limited to 32 MiB, 8 MB per mapping [SDK: Memory_Management-Tutorial, 'Memory
  Mapping'; libgxm-Overview, 'Mapping Memory'; libgxm-Reference, 'sceGxmMapFragmentUsseMemory'].
- One immediate rendering context per process [SDK: libgxm-Overview, 'Rendering Contexts']. A
  plugin inside a game cannot render through its own context; the Shell UI renders through PAF
  in SceShell, which drives the Shell's framebuffer flip [wiki: ScePaf,
  'scePafGraphicsSetFrameBufInternal'].
- Common dialogs render inside the application process, on the application's colour surface,
  with its libgxm [SDK: `SDK_doc__System__Common_Dialog-Overview_e.txt`, 'libgxm
  Initialization']. They are not free either.

## 9. Error codes to recognise

| Code | Name | Meaning for us | Source |
|---|---|---|---|
| `0x80024302` | `SCE_KERNEL_ERROR_NO_FREE_PHYSICAL_PAGE` (C1-2569-2) | the caller's physical partition has no free page: SceShell's 50 MiB (#59, #11), KD (#14) or the game's 256 MiB | [SDK: Kernel-Reference, 'Error Codes'; Error-Overview] |
| `0x80024303` | `…NO_FREE_PHYSICAL_PAGE_UNIT` | same family; a page unit could not be allocated | [SDK: Kernel-Reference] |
| `0x80024305`, `0x80024306` | `…NO_PHYMEMPART_LPDDR2`, `…_CDRAM` | the requested type's partition does not exist for this process | [SDK: Kernel-Reference] |
| `0x80024B05` | `…ILLEGAL_MEMBLOCK_SIZE` | size not a multiple of the type's granularity (4 KiB / 1 MiB / 256 KiB) | [SDK: Error-Overview] |
| `0x80024B07` | `…MEMBLOCK_TYPE_FOR_KERNEL_PROCESS` | a user process asked for a kernel-only type [inference from the name] | [SDK: Error-Overview] |
| `0x8002710B` | `SCE_KERNEL_ERROR_NO_MEMORY` | the kernel's generic "insufficient memory" | [SDK: Kernel-Reference] |
| `0x8002D005`-`0x8002D008` | `…MODULEMGR_NOMEM_LIB/STUB/SELF`, `…NOMEM` | the module loader ran out of memory loading a module (a plugin, for instance) | [SDK: Error-Overview] |
| `0x80028024` | `…ILLEGAL_STACK_SIZE` | thread stack outside 4 KiB-32 MiB | [SDK: Error-Overview] |
| `SCE_KERNEL_ERROR_UID_RL_OVERFLOW` | — | more than 128 threads or 2048 memory blocks (or 2048 sync objects) in the process | [SDK: Kernel-Overview, 'Limitation on Kernel Resources'] |
| `0x8001000C`, `0x80010013` | `ENOMEM`, `ENODEV` (errno facility) | `ENODEV` from a file open means the mount is not reachable from this process: games cannot see `ux0:`, hence the kernel model broker | [SDK: Error-Overview; wiki: Error_codes] |
| `0x805B0002` | `SCE_GXM_ERROR_OUT_OF_MEMORY` | libgxm could not get memory (parameter buffer and friends) | [SDK: Error-Overview] |
| `0x80102F02` | `SCE_SCREENSHOT_ERROR_NO_MEMORY` | the system screenshot library ran out of memory | [SDK: `SDK_doc__System__Screenshot-Reference_e.txt`] |
| `0x8010113D` | — | not in the SDK 3.55 table nor the wiki; VSH facility (0x10), sub-facility 0x11 is unlisted. Seen when installing a VPK with RGBA LiveArea art (guide.md, 'Build and validation') | [wiki: Error_codes, '0x8010XXXX']; [measured] |

Error word layout: bit 31 error, bits 27-16 facility (0x001 errno, 0x002 kernel, 0x010 VSH,
0x04F libheap), bits 15-0 the code [wiki: Error_codes, 'Format']. Our own API 13+ codes keep
bit 30 clear because the console returned `-3` as `0xBFFFFFFD` (guide.md, 'Memory and
troubleshooting').

## 10. Suspend and resume

- Pressing the PS button in a full-screen application switches to LiveArea and suspends the
  application's process; tapping the gate resumes it; peeling the LiveArea kills it [SDK:
  System_Software-Overview, 'Screen Transitions'].
- An application cannot detect that it is being suspended; it learns afterwards from the
  `SCE_APPMGR_SYSTEMEVENT_ON_RESUME` event. While system UI is overlaid, the application loses
  controller and touch input [SDK: `SDK_doc__System__Application_Manager-Overview_e.txt`,
  'Resume Event', 'UI Overlaid State of System Software'].
- A process's `sceKernelGetProcessTime` clock stops while the process is suspended [SDK:
  Kernel-Overview, 'Process Time']. Timeouts computed from it do not expire during a suspend.
- System suspend (power button, idle, low battery, thermal limit) keeps main DRAM powered; the
  CPU is powered off, its context saved, and the boot loaders re-run on resume [wiki: Suspend;
  SDK: Power-Overview, 'Automatic Suspension']. Memory block contents in LPDDR survive; whether
  CDRAM contents survive is not documented: treat GPU-side data as stale after a resume
  [inference].
- Kernel drivers see an ordered sequence of suspend and resume phases; user processes get power
  callbacks, but a game process only receives a reduced mask of them while system processes get
  the full set [wiki: SceSysmem, 'SysEvent'; wiki: ScePower, 'Power callbacks'].
- For the overlay: a job is tied to its game, capture sequence and foreground generation, and
  suspend invalidates old work (guide.md, 'Memory and troubleshooting'). Resident weights, the LM and the cascade's model are
  released once the game has not been the active one for 5 s of its running time (the rule in
  section 5.2; the time stops while the process is suspended, so a resume keeps them). Nothing may
  assume the frame it captured is still the one on screen after a resume.

## 11. Threads

- Stack: at least 4 KiB, at most 32 MiB, rounded up to 4 KiB; TLS at least 4 KiB more; both from
  the process budget [SDK: Kernel-Reference, 'sceKernelCreateThread'].
- Priorities 64 (highest) to 191 (lowest); a game's default is 160. Default priority with the
  default affinity mask puts the thread in the common ready queue, which the scheduler balances
  across the process's cores; an individual-queue priority should be paired with exactly one
  core [SDK: Kernel-Reference, 'sceKernelCreateThread'; Kernel-Overview, 'Thread Scheduling'].
- The scheduler re-runs when the owning process is suspended, when a thread waits, wakes or
  changes core [SDK: Kernel-Overview, 'Thread Scheduling'].
- Block, never spin [SDK: Power-Overview, 'CPU Busy Loop'; `SDK_doc__Tutorial__Multi_Thread-Tutorial_e.txt`,
  'Synchronization'].
- 128 threads per process, libraries included [SDK: Kernel-Overview].

## 12. Do and don't

Do:

- Run inference, weights, workspaces, dictionaries and screenshots in the game's process, never
  under `*ALL` [expert]: since #138 the kernel plugin loads the worker into each game ON in
  `games.ini` ([worker-loading.md](worker-loading.md)); a `*TITLEID` section per game did it before.
- Preflight with `sceKernelGetFreeMemorySize().sizeMain`, round to 4 KiB, keep a reserve, allocate
  per job, free after, and accept that a game may take the memory first.
- Return `SCE_KERNEL_START_NO_RESIDENT` from any process the plugin does not serve.
- Keep SceShell to the UI: small code, tiny static state, transient PAF allocations behind the
  reserve guard, one thread.
- Keep kernel buffers region-sized and transient; keep kernel `.bss` small.
- Measure on the console: free `sizeMain` per game, SceShell headroom, KD headroom, CPU3 with
  PSVshell. Log the numbers on every refusal.
- Deploy one change at a time, with backups of both taiHEN configurations and the owner's explicit
  OK; verify that the Vita reboots before the next change [expert].

Don't:

- Don't run OCR, hold weights, build dictionaries, encode JPEGs or keep screenshots in SceShell
  [expert].
- Don't register under `*ALL` [expert].
- Don't add static buffers to the Shell plugin; a 64 KiB `.bss` increase has broken the boot
  (#42) [measured].
- Don't treat `sce_paf_memalign` success as proof that SceShell has memory: the PAF heap is
  pre-mapped; the pages SceShell's own threads need come from the partition [measured; inference].
- Don't read "free memory" from one pool and allocate from another; `sizeMain`, `sizeCdram` and
  `sizePhycont` are separate [SDK: Kernel-Reference].
- Don't use the game's `malloc`, and don't create many small memory blocks (2048 limit, 4 KiB
  each) [SDK: Memory_Management-Tutorial].
- Don't busy-wait or poll with 1 ms sleeps in a hot loop [SDK: Power-Overview].
- Don't treat Vita3K as evidence for plugin boot or memory safety; it does not model the
  budgets, SceShell or taiHEN [expert].
- Don't automate repeated reboots after a failed boot. Recovery is: hold L at power-on to skip
  plugins, then comment the plugin out of the taiHEN configuration in VitaShell [expert].
- Don't compute a CPU3 percentage from source; report it as unmeasured until PSVshell readings
  exist [expert].

## 13. Before you allocate: the checklist

1. Which process will run this code: kernel, SceShell, or the game? If SceShell, stop and move it.
2. Which pool does the memblock type select, and what is its granularity (4 KiB / 1 MiB /
   256 KiB)? Round the size first.
3. How long will it live: a job, a session, the process? Everything that lives longer than a job
   needs a release path (game change, suspend, disabled title, refused allocation, `module_stop`).
4. Preflight: `sceKernelGetFreeMemorySize`, the right field, minus the reserve, against the
   page-rounded total of everything the job will hold at its peak (weights + workspace + stacks).
5. What happens when the allocation fails anyway? The feature degrades, the UI says which pool
   ran out (kernel capture, game workspace, game weights, Shell results), nothing crashes, and
   the failure is logged with the free figure.
6. Does this add a thread? Count 4 KiB stack + 4 KiB TLS at minimum, from the host's budget, for
   the thread's lifetime. Is a thread needed at all?
7. Does this add static data to a module? Pages are permanent for the process's life. In SceShell
   that is a boot-path change.
8. Does this add a module to another process (`*ALL`, a new section)? Its pages land there too.
9. Is the memory GPU-mapped or CDRAM? Then it is uncached for the CPU, 256 KiB-granular, and may
   be stale after a resume.
10. Add the allocation to the ledger with its pool and worst-case size, and log current and peak
    per pool at run time.
11. Is there a console measurement for the number you are relying on? If not, write
    "estimate" next to it, and plan how the self-test will measure it.

## 14. Open questions

- The free size of `ScePhyMemPartKD` on a retail 3.65 console with the usual plugin set.
- SceShell's free pages at boot and at game launch, in both the API 13 and API 16 builds (the
  measurement that would turn #59's explanation into a proof).
- Free `sizeMain` across the owner's Japanese visual novels; today there is one title.
- Whether CDRAM contents survive a system suspend.
- PS TV budget sizes.
- CPU3 cost of the worker, with PSVshell.
- The system-side memory the screenshot and LiveArea refresh need, which bounds what any Shell
  plugin may ever hold.
