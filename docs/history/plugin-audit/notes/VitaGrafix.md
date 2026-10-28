# VitaGrafix (Electry/VitaGrafix, commit 65b6ee3, 2019-12-29, v5.0.2)
Files read: 31 files, 6873 lines (CMakeLists.txt 59, VitaGrafix.yml 8, src/main.c 312, main.h 80, osd.c 207, osd.h 34, osd_font.h 2294, osd_logo.h 49, io.c 195, io.h 44, log.c 87, log.h 12, config.c 318, config.h 96, patch.c 298, patch.h 21, patch_hook.c 86, patch_hook.h 8, interpreter/{interpreter.c 59, interpreter.h 74, parser.c 798, parser.h 140, legacy.c 236, legacy.h 5, op.h 80, op_datatype.c 48, op_encode.c 277, op_math.c 183, op_vitagrafix.c 130}, tests/main.c 51, tests/test_interpreter.c 584; README.md)

## 1. Process context
- One user module `VitaGrafix.suprx` (`vita_create_self(VitaGrafix.suprx ... CONFIG VitaGrafix.yml)`, CMakeLists.txt:57-59), user APIs only (`sceAppMgrAppParamGetString`, `taiHookFunctionImport`, `taiInjectData`, `sceIoOpen`). It patches the GAME's main module (`TAI_MAIN_MODULE`, main.c:280-285, patch.c:49) so it is installed under `*ALL` (the README in this commit has no install line; the project's wiki/convention is `*ALL` and the code exits early for VitaShell, main.c:218-221, which only makes sense for *ALL loading).
- All memory is the game process': static .bss of the module image and the game's thread stack during module_start / the display hook. No kernel module.

## 2. Module start / stop
module_start (main.c:213-299):
1. `sceAppMgrAppParamGetString(0, 12, titleid)` (main.c:216); exit (START_SUCCESS, module stays resident but inert) if VITASHELL (main.c:219-221).
2. `taiGetModuleInfo(TAI_MAIN_MODULE)` + `sceKernelGetModuleInfo` (main.c:224-227), unchecked.
3. `sceIoMkdir("ux0:data/VitaGrafix/")` (main.c:230) — file I/O at module start, inside the game, on every game launch.
4. `vg_config_parse()` reads `ux0:data/VitaGrafix/config.txt` (creating it if missing, config.c:291; io.c:93 `SCE_O_CREAT`), 1 KiB chunks on the stack (io.c:102).
5. On config error: force OSD on and log (main.c:241-247); if `ENABLED=0` -> EXIT (main.c:253-254).
6. `vg_patch_parse_and_apply()` reads `patch/<TITLEID>.txt` else `patchlist.txt` (created if missing, patch.c:265-270), applying `taiInjectData` (patch.c:49) and up to 4 `taiHookFunctionImport` (patch_hook.c:32) AS IT PARSES. On a parse error it stops with the patches applied so far left in place (patch.c:130-134 return; no rollback), forces OSD on and logs (main.c:262-269).
7. If the game is unsupported/self-shell -> EXIT with START_SUCCESS (main.c:273-275).
8. OSD hook `taiHookFunctionImport(TAI_MAIN_MODULE, TAI_ANY_LIBRARY, 0x7A410B64 sceDisplaySetFrameBuf)` (main.c:281-285); result unchecked (a negative uid is stored; module_stop guards `>= 0`, main.c:302).
9. `vg_log_flush()` (main.c:297) then `return SCE_KERNEL_START_SUCCESS` (main.c:298) — ALWAYS success; never NO_RESIDENT.

module_stop (main.c:301-320): release OSD hook if `>= 0`; release injects in reverse order guarded `>= 0`; release hooks in reverse order guarded. No threads, no memblocks. The OSD hook also releases ITSELF from inside the hook after 5 s (main.c:52-62: `taiHookRelease(g_main.osd_hook, ...)` inside `sceDisplaySetFrameBuf_patched`, then sets `osd_hook = -1`) — a self-releasing one-shot hook pattern; it stays installed indefinitely when there was an I/O error (so the error stays on screen).

## 3. Hooks
| # | target | API | installed | released |
|---|---|---|---|---|
| osd | game main module import 0x7A410B64 sceDisplaySetFrameBuf | taiHookFunctionImport | module_start main.c:281 | self-release after 5 s main.c:56-58, or module_stop main.c:302-304 |
| hook[0..3] | game imports 0x7A410B64 (SetFrameBuf withWait), 0x67E7AB83 / 0xC4226A3E (ReadBufferPositive -> Peek) chosen by patchlist `>` lines | taiHookFunctionImport | during patchlist parse patch_hook.c:32 | module_stop main.c:312-316 |
| inject[0..1023] | game main module data/code at seg:offset from patchlist | taiInjectData | patch.c:49 | module_stop main.c:306-310 |
- Partial failure: hook/inject failure returns an IO_ERROR status (patch_hook.c:33-35, patch.c:50-54), which aborts the parse; earlier injects stay applied (released only at module_stop). `g_main` is a zeroed static, so unused slots hold 0 and are never iterated (counts bound the loops).
- Hook bodies: the OSD hook draws text directly into the game's framebuffer (main.c:64-...; osd.c), with `osd_draw_stringf` using a 1 KiB stack buffer (osd.c:177) and `snprintf`; no allocation, no mutex, no I/O in the hook.

## 4. Threads
- None created. All work in module_start (game main thread) and in the game's display call.

## 5. Allocations
| site | API | size | context | pool | lifetime | failure |
|---|---|---|---|---|---|---|
| main.c:13 `g_main` | static | sizeof(vg_main_t): inject[1024]*4 = 4096 + hook/ref 32 + config (~ib[16]*4 + ...) ~ 4.4 KiB | game | module .bss (game process) | module life | n/a |
| main.c:16 `g_osd_buffer` | static | 1024 B | game | .bss | module life | n/a |
| log.c:11 `g_log_buffer` | static | 1024 B | game | .bss | module life | flushed to file |
| osd_font.h / osd_logo.h | static const | font 95 chars x 20 rows x 2 B = ~3.8 KiB; logo 60x38 = 2280 B | game | .rodata | module life | n/a |
| osd.c:177 `buffer[1024]` | stack | 1 KiB | game display thread | game thread stack | call | n/a |
| io.c:102 `chunk[1024]`, patch.c:243 `path[128]`, parser value_t (32 B raw + 32 bool) | stack | ~1.2 KiB | game main thread at module_start | game thread stack | call | n/a |
| none | sceKernelAllocMemBlock / malloc / sce_paf | 0 | | | | |
- Resident in the game: ~ 8 KiB .bss + ~6 KiB .rodata + code. Nothing dynamic. Nothing in SceShell. Nothing in kernel.

## 6. Firmware assumptions
- None in code: works by NID import hooks on the game module and user-supplied offsets, with the game's module NID checked against the patchlist entry (`vg_main_is_game`, main.c:187-206: wrong NID -> GAME_WRONG_VERSION -> OSD error message).

## 7. Configuration
- `ux0:data/VitaGrafix/config.txt` ([MAIN] and per-title sections, config.c), `patch/<TITLEID>.txt` or `patchlist.txt` (patch.c:261-270), 1 KiB chunked reader with 128-char line limit (io.h:5-6), `!USE(name)` redirection (patch.c:189-211). Read in the game process at module_start. Missing config/patchlist are CREATED empty (io.c:93 with create=true); missing game-specific patch file falls back to patchlist.

## 8. Logging
- `ux0:data/VitaGrafix/log.txt`, 1 KiB RAM buffer (log.c:11) flushed when full and at the end of module_start (main.c:297); truncated at each start when LOG=1 (log.c:21-23 prepare). Runs on the game's startup path. Also read back for display on the OSD after an error (main.c:288-290, log.c:66-86).

## 9. Build artifacts
- `VitaGrafix.suprx`; yml exports only start/stop. Links `taihen_stub SceAppMgr_stub SceDisplay_stub SceCtrl_stub SceKernelModulemgr_stub SceLibKernel_stub SceIofilemgr_stub SceLibc_stub gcc`, `-nostdlib`, `-O3 -std=gnu99` (CMakeLists.txt:40-55). Uses SceLibc (snprintf/strtoul) from the game's libc — a dependency on the host game having SceLibc loaded. Host unit tests for the interpreter under tests/.

## 10. Patterns worth copying / anti-patterns
- (+) Self-releasing one-shot display hook (main.c:52-62): the OSD costs nothing after 5 s and leaves no hook in the game. The overlay's Shell boot hook uses the same idea (shell/boot.c one-shot).
- (+) Zero dynamic allocation; every buffer is a small static or stack array (sections 5).
- (+) Explicit caps on patch/hook counts with error codes (`MAX_INJECT_NUM`, `MAX_HOOK_NUM`, patch.c:38-40, patch_hook.c:25-27) and `>= 0` guards in module_stop.
- (+) Exits early and stays inert in VitaShell (main.c:219-221) and for unsupported titles; a `*ALL` plugin that does nothing in other apps.
- (-) File I/O (mkdir, config read and CREATE, patchlist read and CREATE, log truncate/append) on every game's startup path inside module_start; a slow or failing ux0 delays or breaks every launch. The README-era hint "Do you have ioPlus installed?" (main.h:11) shows ux0 writes from a game can fail with the standard sandbox.
- (-) Patches are applied while parsing; a mid-file syntax error leaves a half-patched game (patch.c:130-134) until module_stop.
- (-) module_start always returns SUCCESS even when it did nothing useful; the module stays loaded in every app (small, but it is the `*ALL` cost).

## 11. Overlay-specific
- Draws by writing pixels into the game's own framebuffer from within the game's `sceDisplaySetFrameBuf` import (user mode, same process: main.c:64-183, osd.c). No off-screen buffer at all: text and rectangles are rasterised straight into the frame (osd.c:97-113, 136-168) with alpha blending in place. Memory from the game: only the stack and ~8 KiB of static .bss of the module. Nothing allocated; a game that has used all of its memory is unaffected. No GPU, no paf, no kernel.
