# Testing without the console: what runs where, and what cannot

The owner's words: "we have no way to properly test this without a live device". This page
says exactly what is testable off the console, what CI runs, what Vita3K can and cannot tell us,
and what only the console can answer. The two things the console kept failing on, boot and
memory, are the two things no emulator models: a plugin's pages inside SceShell's 50 MiB
partition, and taiHEN loading our modules into firmware processes
([vita-memory.md](vita-memory.md), AGENTS.md rule 5).

## 1. The matrix

| Our code | host x86_64 (CI `host` job) | ARMv7 under qemu-arm (`tools/arm_host_tests.sh`) | Vita3K | the console |
|---|---|---|---|---|
| kernel: scene detection, triggers, foreground state machine, capture buffers, the game-OCR broker, the boot guard, the ledger | real code, SDK calls stubbed (`host/tests/vita_stubs`) | real code, same stubs, 32-bit, LDREX/STREX atomics | not loadable: the kernel plugin needs `SceDisplay`/`SceCtrl`/`SceTouch`/`SceProcessmgr` ForDriver exports to hook and `SceSysmem`/`SceIofilemgr` ForDriver calls; Vita3K has no firmware kernel to export them (its taiHEN HLE implements "all functions taiHEN itself relies on", nothing of a kernel plugin's imports) | the only place its hooks, its partition and its pool exist |
| kernel: the hooks themselves (display, pad, touch, process events) | no | no | no | only here |
| Shell plugin: control policy, memory lifetime, Anki miner and queue, strip, cursor, dictionary paging | real `shell/worker.c`, `anki.c`, `anki_queue.c`, `capture.c`, `strip_view.c`, `files.c` with stubs (16 suites) | not built (needs BearSSL and the core library; vita-vn-ocr's ARM lane covers the recognizer) | not loadable in its real context: Vita3K has no SceShell ("Shell support is pretty spotty" is about its own replacement UI); ScePaf, SceShell's partition and the LiveArea daily job do not exist there | only here |
| Shell plugin: the Paf UI (`overlay.cpp`, `strip.cpp`) | header-level tests (`test_header_highlight.cpp`, `test_word_navigation.cpp`) | no | no | only here |
| game worker: memory check, resident weights, model I/O through the broker, cancellation, the safe-boot exit | real `game_ocr/main.c` with stubs, real FL10/H15 when the models are present | no (the vocr runtime has its own ARM parity lane in vita-vn-ocr) | could load into a game through Vita3K's taiHEN HLE, but its `vjoOcr*` syscalls come from our kernel plugin, which cannot load: `module_start` would find the imports unresolved and start nothing | only here |
| packaged modules: syscall ABI, firmware-independent kernel imports, ARM-only instructions, page budgets | `check_vita_syscall_abi.py`, `check_kernel_instructions.py`, `check_memory_ledger.py` in the `vita` job | | | |
| SceShell's memory under our footprint | the replay model (section 4), from console samples | | not modelled: no budgets, no SceShell | the self-test ([self-test.md](self-test.md)) |

## 2. What CI runs (and why it is real code)

The host suites compile the plugin sources themselves (`#include "../../shell/worker.c"`,
`kernel/boot.c`, `game_ocr/main.c`), not copies, and replace only the SDK boundary: memory
blocks, threads, mutexes, files, the clock, and the kernel syscalls. Allocation failures,
failed joins, cancelled jobs and crashes between steps are injected there. 28 ctest suites
plus the Python tool suites run in the `host` job; `test_jpegsw.py` and the Hachidori relay
contract test complete it.

## 3. ARM under qemu-arm: the same C on the Vita's CPU

`tools/arm_host_tests.sh` builds the pure-C kernel and core suites with a Linux ARM toolchain
(`-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -static`, the flags vita-vn-ocr's runtime lane
uses) and runs them with `qemu-arm -cpu cortex-a9`:

| suite | cases | what the ARM build adds over x86_64 |
|---|---|---|
| boot_guard | 11 | 32-bit `uint32_t`/`int` arithmetic of the record, byte-identical record text |
| ledger | 2 | `__sync` atomics compile to LDREX/STREX; 32-bit counters |
| kernel_capture | 7 | 32-bit stride and capacity arithmetic |
| kernel_input | 14 | the trigger state machine |
| kernel_game_ocr | 25 | the broker's 32-bit offsets and `SceOff` (64-bit on a 32-bit CPU) |

Measured on the fleet box (Arm GNU Toolchain 14.2.Rel1, qemu-arm 10.0.13): all 59 cases pass.
In CI it is the `arm` step of the host job (`gcc-arm-linux-gnueabihf`, `qemu-user-static`). It
is not a Vita: no firmware, no taiHEN, no SceShell, no budgets; a pass here says the C is
32-bit clean, nothing more.

## 4. The Shell budget replay model

`tools/shell_budget_model.py` replays the boot and game-launch allocation sequence of the
current build against the console's measured SceShell memory:

- every item's size comes from `include/vjo_ledger_budget.h`, which CI ties to the packaged
  modules ([memory-ledger.md](memory-ledger.md));
- every sample comes from `docs/benchmarks/shell-budget-samples.json`, with its date, source,
  and the overlay build that was loaded when it was taken, measured from that release's SELF
  segments. A sample taken with build B loaded means SceShell had `free + pages(B)` without
  us; the headroom the current build leaves at each step is that minus the pages it holds.

Current build (`--anki` adds a card save), against the 11 October session (v0.7.4, CLANNAD in
front, 98,304 B free, the build holding 462,848 B):

```
step                                        held      headroom
Shell module loaded into SceShell        299,008       262,144
OCR worker pages in SceShell             299,008       262,144   (0: registered per game, not loaded there)
VjoNet thread                            368,640       192,512
VjoControl thread (boot complete)        389,120       172,032
game start: result memory (local)        389,120       172,032
Anki card saved: job thread              458,752       102,400
```

What this says, and what it does not:

- Before the fix (main at `ae94879`) the build held 532,480 B at the end of boot and would have
  left 28,672 B in that session: seven pages, less than the 65,544 B change that broke the boot
  in #42; an Anki save would have found no pages for its thread. The fix (worker `NO_RESIDENT`,
  the overlay's text arena out of `.bss`) holds 389,120 B, 73,728 B below the v0.7.4 build that
  ran beside a game, and leaves 172,032 B at the end of boot in the same session.
- `e759187`, the build that failed in #59, would show 20,480 B of headroom in the same
  scenario, positive. The model cannot prove that a build fits: SceShell's own use differs
  between sessions (a second sample, ownership unverified, had 28,672 B free), the daily
  LiveArea job's request size is unknown, and a private testing agent was resident in SceShell
  in both sampled sessions. It ranks builds and changes, and it says which step would fail
  first. The console decides.
- The PAF heap scenarios (2,538,568 B and 2,557,952 B free after a reboot; 355,256 B with a
  leaked 2 MiB pool) show every allocation of ours refused at the low-water mark and all of
  them fitting after a reboot, which is what the console showed.

`tools/tests/test_shell_budget_model.py` checks the arithmetic on a synthetic budget, that the
real tree's boot completes in the v0.7.4 scenario (the regression this guards), and that the
pre-fix footprint (`--shell-pages 397312 --worker-pages 45056`) ranks worse than the tree's.

## 5. Vita3K, precisely

Vita3K gained a taiHEN HLE in its PR #3885 (wiki page "Using taiHEN plugins", revised May
2026): `config.txt` is read from `ux0:tai`/`ur0:tai`, `*KERNEL`, `*TITLEID`, `*ALL` and `*main`
are parsed, and "all functions taiHEN itself relies on are implemented. Plugins that use
functions Vita3K does not implement likely will not work." The page's own note on `*main`:
"Shell support is pretty spotty". For us:

- the kernel plugin imports `SceDisplayForDriver`, `SceCtrlForDriver`, `SceTouchForDriver`,
  `SceProcessmgrForDriver` (hook targets), `SceSysmemForDriver`, `SceIofilemgrForDriver`,
  `SceThreadmgrForDriver` and `taihenModuleUtils`; none is a function "taiHEN itself relies
  on", and there is no firmware kernel behind them: it does not load;
- the Shell plugin needs SceShell and ScePaf, which do not exist in Vita3K; its memory
  question (pages in a 50 MiB partition shared with firmware) is the one Vita3K does not
  model at all;
- the game worker could be injected into a game, but without the kernel plugin its weak
  `vjoOcr*` imports stay unresolved and `module_start` starts nothing (by design: that is also
  what happens on a console whose kernel plugin failed).

So Vita3K can run none of the three plugins as they are, and it would say nothing about boot
or memory if it could: no `ScePhyMemPartShell`, no `ScePhyMemPartKD`, no `0x80024302`
[expert; AGENTS.md rule 5]. It is not wired into CI. vita-vn-ocr uses qemu-arm, not Vita3K,
for the recognizer's ARM parity and cycle estimates, for the same reason.

## 6. What only the console answers

1. Whether the boot completes with our pages present (the self-test's `boot`/`ok` steps and
   the boot guard's record).
2. SceShell's free partition at boot and at game launch with the current build, and the
   kernel pool's free size (the self-test's `mem`/`kernel` lines). These replace the model's
   samples and the ledger's ratchet.
3. The game's free `USER` per title (the self-test's `game` line; the fleet's model-limit
   work asks for every title tried).
4. The hooks: input filtering, capture, process events; and the PAF UI.
5. CPU3 load, by PSVshell readings with matched clocks, plugin set and workload (unmeasured
   until then; AGENTS.md rule 8).
