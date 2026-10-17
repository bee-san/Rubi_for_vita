# Agent instructions

## This fork stands on its own

`bee-san/Rubi_for_vita` (named `bee-san/vita-jp-overlay` before 2026-10-17; GitHub redirects the old
name) is bee-san's personal software, forked from
[Dartv/vita-jp-overlay](https://github.com/Dartv/vita-jp-overlay). It is not
maintained as a contribution to that project.

- Upstream is pull-only. Its changes may be merged into this fork, never the
  other way round.
- Never open pull requests, issues or discussions on `Dartv/vita-jp-overlay`,
  never comment there, and never push branches or tags to it.
- All work lands in `bee-san/Rubi_for_vita`, through pull requests against
  its `main`.
- In a fork, `gh` defaults to the upstream parent. Always pass
  `-R bee-san/Rubi_for_vita`, for example
  `gh pr create -R bee-san/Rubi_for_vita --base main`.

To pull in upstream changes, merge `upstream/main` into a branch of this fork,
keep this fork's behaviour wherever the two differ, and open a pull request
against this fork's `main`:

```sh
git remote add upstream https://github.com/Dartv/vita-jp-overlay.git  # once
git remote set-url --push upstream DISABLED                            # fetch only
git fetch upstream
git switch -c sync/upstream origin/main
git merge upstream/main
```

Build and test instructions are in [the guide](docs/guide.md#build-and-validation).

## Memory and process context (binding)

These rules come from a PS Vita plugin developer (2026-10-11), the Sony SDK documentation and
this project's console failures (#11, #14, #42, #59). The reference with every pool, size and
citation is [docs/vita-memory.md](docs/vita-memory.md). Read it before touching any allocation,
thread, static buffer or taiHEN registration.

1. **No heavy work in SceShell.** `*main` is SceShell. Its 50 MiB partition is shared with all of
   SceShell and was measured at 28-98 KiB free while a game ran. A Shell plugin's whole budget is
   genuinely under 2 MiB, and SceShell wants that headroom for screenshots and LiveArea. Never
   run OCR, hold model weights or workspaces, build dictionaries, encode JPEGs or keep
   screenshots in SceShell. The Shell plugin is the UI and nothing else; its permanent footprint
   (module pages, stacks, TLS) may only shrink, and it makes no `sceKernelAllocMemBlock` after
   boot.
2. **Heavy work runs in the game's process, never in SceShell, never under `*ALL`.** The kernel
   plugin loads the game OCR worker into each game that is ON in `games.ini` as the game starts
   (owner's decision, 2026-10-16, #138; this replaces "register one `*TITLEID` section per enabled
   game"): never into SceShell, a system app (the PSP emulator's `NPXS10028` and Adrenaline's
   `PSPEMUCFW` are games), VitaShell or the Rubi app, and nothing while L is held at the start
   ([docs/worker-loading.md](docs/worker-loading.md)). Installers write no worker line and remove
   the ones older installs left (the Rubi app's Install page and `install-vita.sh` share one
   editor, `core/taicfg.c`, checked with taiHEN's own parser: [docs/one-vpk-install.md](docs/one-vpk-install.md));
   a leftover line is harmless, as the worker keeps one copy per process. `*ALL` loads
   a module into SceShell and every system app as well, where its pages are permanent: never use
   it. Any module that can still be loaded elsewhere returns `SCE_KERNEL_START_NO_RESIDENT` there,
   and a process keeps one copy of the worker.
3. **Preflight, reserve, free.** In the game, check `sceKernelGetFreeMemorySize` (`sizeMain` for
   `USER_RW`), round to 4 KiB, keep a reserve, allocate per job and free when the job ends; a
   failed allocation after a passed check is normal and must degrade, never crash. Few games
   allocate the full 256 MiB unconditionally; if memory problems persist even with every title
   forced into the 333 MiB mode, the code is in the wrong process context.
4. **The kernel pool is shared and small.** Kernel buffers (36 MiB `ScePhyMemPartKD`, shared
   with every driver) are region-sized and transient; kernel `.bss` stays small.
5. **Vita3K is not evidence.** A Vita3K pass says nothing about plugin boot or memory safety on a
   console: it does not model the budgets, SceShell or taiHEN. Only a console run counts, and
   every unmeasured console number is marked as an estimate. What can run off the console, and
   what each result does and does not show, is in [docs/offline-testing.md](docs/offline-testing.md);
   `tools/shell_budget_model.py` ranks a change's SceShell footprint against the measured
   sessions before it is built.
6. **Recovery path.** The boot-safety guard ([docs/boot-safety.md](docs/boot-safety.md)) turns
   the boot after an incomplete one into a safe boot: no kernel hooks, the Shell and game
   plugins unload themselves, a note in `status.txt`. If a plugin still stops the Vita from
   booting: hold L while powering on to skip plugins, start VitaShell, and comment the plugin
   out of the taiHEN configuration (`tools/install_ftp.sh --uninstall VITA_IP` restores the
   backed-up configuration). Never automate repeated reboots after a failed boot. Keep
   `boot_init` the first call in the kernel's `module_start`, and keep the safe-mode paths
   free of allocations.
7. **One change at a time.** Build and inspect the plugin first (ELF, imports, module metadata).
   Before installing, show the exact files and taiHEN configuration edits, back up both
   configurations, state the recovery steps, and get the owner's explicit OK. Install one change,
   confirm the Vita reboots and comes back, then the next. Boot-path changes (anything that adds
   pages, threads or allocations to SceShell or the kernel) merge only after the boot-safety guard
   and the memory ledger exist and have host tests.
8. **CPU3 is unmeasured until measured.** Do not compute a CPU3 percentage from source. Report it
   as unmeasured until PSVshell readings with matched clocks, plugin set and workload exist.
9. **Cite or mark.** Every memory number in docs and PR bodies is either measured on the console
   (say where), cited to a source, or marked as an estimate.
10. **Every allocation goes through the ledger.** New memory (a block, a thread, a PAF
   allocation) is taken only through the wrappers in [docs/memory-ledger.md](docs/memory-ledger.md),
   with its worst case added to `include/vjo_ledger_budget.h`; `tools/check_memory_ledger.py`
   fails CI otherwise, and when a pool's worst case exceeds its limit or a packaged module has
   more pages than declared. Raising a budget number is a deliberate act that the PR must justify.
