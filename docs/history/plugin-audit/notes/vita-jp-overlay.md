# vita-jp-overlay (bee-san/vita-jp-overlay, main 599dd9c = release e759187 + docs, 2026-10-13)
Files read: kernel/ (18 files, 3288 lines), shell/ (22 files, 5697 lines), game_ocr/ (3 files, 417 lines), include/ (4 files), core/games.c, core/vocr_ocr.h, vita/CMakeLists.txt, tools/patch_tai_config.py. Segment sizes measured on the sibling build of the same commit (`readelf -lW` on build/vita/*.velf in clones/overlay-load, built from 599dd9c).

## 1. Process context
Three modules (vita/CMakeLists.txt):
| module | section | process | installed by |
|---|---|---|---|
| VitaJPOverlay_Kernel.skprx | `*KERNEL` | kernel | tools/patch_tai_config.py:32 |
| VitaJPOverlay_Shell.suprx | `*main` = SceShell | SceShell (UI, dictionary, Anki, network, job control) | patch_tai_config.py:35 |
| VitaJPOverlay_OCR.suprx | `*ALL` | every user process; a worker thread only in "candidate" titles | patch_tai_config.py:48 (README "under *ALL") |

Status 2026-10-13 (after this audit of `599dd9c`): the installer registers the worker per game, one `*TITLEID` section for each `--title`, never `*ALL`, and comments out an `*ALL` worker line ([vita-memory.md 4.3](../../../vita-memory.md)).
- Measured resident image: Kernel text 26,436 B + RW 78,128 B (bss 78,108); Shell text 221,984 B + RW 178,628 B (bss 178,472) = 400.6 KiB in SceShell; OCR worker text 35,872 B + RW 4,480 B = 39.4 KiB in every process it is loaded into.

## 2. Module start / stop
Kernel module_start (kernel/main.c:458-495): zero state, event flags x3 + mutexes x2 (checked: on failure delete + START_FAILED, :471-476); then `display_hook_install(), input_hooks_install(), lifecycle_hooks_install()` whose results are only LOGGED (:479-480) — a failed hook does not fail the start and the successful ones stay installed; worker thread create/start checked: on failure `release_hooks(); delete_sync_objects(); return START_FAILED` (:484-492). module_stop (:497-515): refuses (STOP_FAIL) while a game job is claimed; stops+joins+deletes the worker, releases all hooks, frees the capture block, deletes sync objects — complete.
Shell module_start (shell/boot.c:36-71): `vjo_status_reset()` = mkdir, remove, rename, open-trunc on ux0 (log_vita.c:70-82) and a status line (= open/append/close, log_vita.c:93-108) BEFORE anything else; `sceIoOpen(RCO)` probe (:45); `_vshKernelSearchModuleByName("VitaJPOverlay_Kernel")` (:53); one import hook on SceShell's `sceAVConfigRegisterCallback` (:62). Each failure -> `vjo_status_close(); return SCE_KERNEL_START_NO_RESIDENT`. The hook body (:16-33) loads the RCO as a Paf plugin (`Plugin::LoadSync`, main.cpp:19-26, result not checked beyond the callback) and runs `vjo_worker_start()` (creates 4 mutexes + 1 event flag + 2 threads, worker.c:1702-1749; `apply_config()` reads config.ini and may open a UDP socket, :1728); a failed worker start is only logged (:24-27) — the Paf plugin stays loaded and the module stays resident doing nothing. The hook releases itself (:29-30). module_stop (:73-81) releases the hook if still installed and calls `vjo_worker_stop()` (joins threads, stops Anki, frees result memory, deletes sync objects; STOP_FAIL if a join fails).
Game worker module_start (game_ocr/main.c:380-396): reads its process title ID, `vjo_game_is_candidate(title)` (core/games.c:15-21: not NPXS* except NPXS10028, not `main`, not VITASHELL, not the settings app) -> `vjo_game_ocr_worker_start()` (:392); ALWAYS returns START_SUCCESS (:395), so the image stays resident in SceShell, system apps and VitaShell too. module_stop (:397-401) stops the worker, frees workspace/weights, drains an outstanding claim.

## 3. Hooks
| # | target | API | installed | released |
|---|---|---|---|---|
| K1 | SceDisplay 0x9FED47AC::0x16466675 sceDisplaySetFrameBufInternal | ExportForKernel | kernel/display.c:143-151 | display.c:153-158 |
| K2-13 | SceCtrl 0xD197E3C7:: 12 Peek/Read variants (same 8 NIDs as PSVshell + 4 Ext/Ext2 as reVita/ds4vita) | ExportForKernel | input.c:451-495 | input.c:507-514 |
| K14-17 | SceTouch ANY:: 0xBAD1960B, 0x70C8AACE, 0x9B3F7207, 0x9A91F624 (same as ds4vita) | ExportForKernel | input.c:496-503 | same |
| K18 | SceProcessmgr import 0x887F19D0::0x414CC813 (proc event handler; same as PSVshell) | ImportForKernel | lifecycle.c:79-86 | lifecycle.c:88-93 |
| S1 | SceShell import 0x79E0F03F::0xFB5E3E74 sceAVConfigRegisterCallback (same bootstrap as PSVshellPlus) | taiHookFunctionImport | boot.c:62 | self: boot.c:29; stop: boot.c:78 |
| G | none | | | |
- Partial failure: input.c zeroes nothing but sets `hooks[i] = -1` first (:488-489) and releases only `>= 0` (:507-514); install failures are counted and `-1` returned (:493, :501, :504) but main.c ignores it (:479-480). lifecycle/display return their uid error, also ignored. No install-all-or-release-all.
- Hook bodies: display hook does the region signature (every 8th frame) and, when a capture is pending, the whole region copy (up to 960x544x4 = 2,088,960 B) into the kernel buffer inside the game's frame-submit call (display.c:61-88, capture.c:107-138). Ctrl hooks rewrite the game's pad data with user<->kernel memcpy, call `rear_sample()` (a kernel touch peek) and `display_static_fb_sample()` (a possible capture copy for Adrenaline) in the game's pad call (input.c:329-345). Proc-event hook takes `g.game_lock` and `g.lock` (lifecycle.c:41-56). No allocation or file I/O in any kernel hook.

## 4. Threads
| name | process | prio | stack | affinity | created | stopped | waits on | period |
|---|---|---|---|---|---|---|---|---|
| VjoWorker | kernel | 0x3C | 0x4000 (16 KiB) | 0x10000 | main.c:484 | main.c:503-507 | `ksceKernelWaitEventFlag(g.ievf, ..., POLL_US)` | 16 ms poll (:19) |
| VjoNet | SceShell | 0x10000100 (default) | 0x10000 (64 KiB) | default | worker.c:1731 | worker.c:1760-1768 | event flag | on demand |
| VjoControl | SceShell | default | 0x4000 (16 KiB) | default | worker.c:1732 | same | `vjoWaitEvent(..., 50 ms)` | 50 ms poll |
| VjoAnki | SceShell | default | 0x10000 (64 KiB) | default | anki.c:708 per job | joined by control thread (anki.c:670-678, 789-797) | n/a | per job |
| VjoGameVocr | game | default | 32 KiB (:8) | default | game_ocr/main.c:345 | :355-376 | `sceKernelDelayThread(100 ms / 50 ms)` | 100 ms poll (:331) |
| ScePaf main thread | SceShell | SceShell's | SceShell's | | `MainThreadCallList::Register(frame)` overlay.cpp:894 | never | every frame | per frame (cheap early-out when closed, :666-668) |

## 5. Allocations
Kernel (all kernel memory):
| site | API | size | lifetime |
|---|---|---|---|
| buffers.c:27 | ksceKernelAllocMemBlock "VjoCapture" KERNEL_RW | region w*h*4 page-rounded, max RAW_SIZE = 2,088,960 B (:13) | per capture request (capture_request.c:39), freed after the last row is read (buffers.c:58-65) or on game exit/stop |
| main.c:484 | thread stack | 16,384 B | module life |
| statics (measured RW segment 78,128 B): `g` (SceneTracker 15,056 + hook_sig 2x524 + capture_sig 524 + …) vjo_kernel_h; game_ocr.c:13-14,32 request 344 + result/incoming 2x4,384 + model_stage 32,768; capture.c:110-111,150-151 2x SceneAcc (1,068) + 3x 3,840 row buffers; log.c:10 ring 4,096 | .bss | | module life |
SceShell (the `*main` plugin):
| site | API | size | lifetime |
|---|---|---|---|
| module image | loader | 221,984 text + 178,628 RW (measured) | SceShell life |
| worker.c:1731-1732 | threads | 65,536 + 16,384 stacks | SceShell life |
| anki.c:708 | thread | 65,536 stack | per Anki job |
| main.cpp:25 | Paf `Plugin::LoadSync` of vitajpoverlay.rco | RCO + Paf plugin objects (not sized in source; XML 6.8 KiB) | SceShell life |
| worker.c:337 via memory.cpp:39 | `sce_paf_memalign` from SceShell's Paf heap, guarded by `GetFreeSize() - 1.5 MiB` (memory.cpp:10,33-38) | 65,536 (local pipeline) or 2x262,144 + 2 TLS slots (~17 KiB buffer + BearSSL context each) (network) | allocated at first job / game start / overlay open; freed when no game is active (worker.c:1697) or pipeline changes |
| anki.c:147 | same Paf path | 131,072 or 393,216 per save/send | per job |
| overlay/strip Paf pages | Paf PageOpen/TemplateOpen (overlay.cpp:468, strip.cpp:238) | Paf-internal, unsized | while open |
| `#else` worker.c:344 | sceKernelAllocMemBlock USER_RW "VjoShellMem" | compiled out (VJO_PAF_ALLOC=1, vita/CMakeLists.txt) | |
| statics (measured 178,628 B): overlay.cpp:129 s_ui_mem 98,304; worker.c:63 scratch 24,576; strip.cpp:35,38 8,194 + 8,480; overlay.cpp:95,99 s_rng 4,096 + s_cursor_text 4,096; worker.c:72 local_text 4,096; worker.c:89-90 + anki.c:54 3x VjoConfig 3,516; worker.c:623 VjoGameOcrResult 4,384; g_view ~8 KiB; strip ruby 2,560 | .bss | | SceShell life |
Game process (the `*ALL` worker):
| site | API | size | lifetime |
|---|---|---|---|
| module image | loader | 35,872 text + 4,480 RW in EVERY process | process life |
| game_ocr/main.c:345 | thread stack | 32,768 in every candidate title (also disabled ones) | process life |
| :161 | sceKernelAllocMemBlock "VjoGameVocrWeights" USER_RW | 659,456 (FL10) / 1,159,168 (H15) page-rounded, after a 4 MiB free-USER reserve check (:19, :160) | while this game stays the active one; dropped on refusal, failure, inactivity, stop |
| :274 | sceKernelAllocMemBlock "VjoGameVocr" USER_RW | vjo_vocr_job_bytes(region) page-rounded | per job, freed in cleanup() |
| :39 | static VjoGameOcrResult pending_result | 4,384 | process life |

## 6. Firmware assumptions
- Kernel uses only NIDs that PSVshell (3.60/3.65) and reVita/ds4vita use; no offsets; title IDs are read in user space because SceSysrootForKernel NIDs differ between 3.60 and 3.63+ (foreground.h:25-29). Shell: SceShell import NID 0xFB5E3E74 of SceAVConfig (as PSVshellPlus); weak imports + `VJO_IMPORT_READY` stub check for Paf/Net (vjo_import.h). CI checks the kernel has no FPU/NEON instructions and the syscall ABI (vita/CMakeLists.txt).

## 7. Configuration
- `ux0:data/VitaJPOverlay/config.ini` (<= 24 KiB-1, VJO_CONFIG_TEXT_MAX) read by the Shell control thread at worker start, game start, every overlay open and subtitles toggle (worker.c:1079, 1391, 1728); `games.ini` (<= 12 KiB) per game start; `region.ini`. Missing config: defaults written (files.c:85-92). Game worker reads no file itself; model bytes come through the kernel broker (32 KiB chunks).

## 8. Logging
- Kernel: 4 KiB ring (log.c) flushed by the worker to `ux0:data/VitaJPOverlay/kernel.txt` (open/append/close per tick with data, main.c:118-152; first flush does mkdir/remove/rename; 64 KiB cap). Shell: `status.txt` ALWAYS on, one open/append/close per `vjo_log` line, from the boot path (boot.c:43-44), the control/net/Anki threads and the Paf main thread (overlay.cpp:580 etc.); `log.txt` optional (256 KiB + rotation); UDP optional. Game worker: none (its numbers travel in the result struct).

## 9. Build artifacts
- vita/CMakeLists.txt: Kernel `-nostdlib -Wl,-q -mgeneral-regs-only`, exports a 22-function syscall library `VitaJPOverlayForUser` (kernel/exports.yml); Shell `-nostdlib -nostartfiles`, weak stubs for the kernel plugin, ScePaf, SceNet, SceNetCtl, SceRtc, SceAppMgr, BearSSL + core linked in, `VJO_PAF_ALLOC=1`; OCR worker `-nostdlib -nostartfiles --gc-sections`, links the vocr runtime; release zip + settings VPK; installer patches `*KERNEL`, `*main`, `*ALL`.

## 10. Patterns the overlay already shares with the reference set
- Same display/ctrl/touch/proc-event NIDs and same-process framebuffer copy rule as PSVshell/reVita/ds4vita (capture.c:86-96 comment).
- One-shot self-releasing boot hook (boot.c:29) as VitaGrafix's OSD hook and PSVshellPlus's bootstrap.
- Kernel module_start fails closed when sync objects or the worker cannot be created (main.c:471-476, 484-492); module_stop is complete and ordered thread -> hooks -> memory, as PSVshell.
- Weak imports with a stub-pattern check (vjo_import.h) — the user-space twin of taiHEN's own `TAI_ERROR_STUB_NOT_RESOLVED` test (taihen.c:117-121).

## 11. Overlay-specific
- Draws with ScePaf inside SceShell (overlay.cpp, strip.cpp) at draw priority 6 over the game, like PSVshellPlus; the screen is captured by the kernel in the game's display hook into a KERNEL_RW block that the Shell (JPEG/Lens) or the game worker (local OCR) reads back through syscalls. OCR weights and workspace are allocated in the GAME's USER_RW by the `*ALL` worker with a 4 MiB reserve; dictionary results, UI text and Anki buffers are allocated from SceShell's Paf heap with a 1.5 MiB reserve; the 400 KiB Shell image and 80 KiB of thread stacks are permanent SceShell USER memory.
