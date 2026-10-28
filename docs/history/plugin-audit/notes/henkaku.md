# henkaku (henkaku/henkaku, commit 784e483, 2018-04-04), plugin/ only: henkaku.skprx + henkaku.suprx
Files read: 8 files, 1386 lines (plugin/kernel.c 257, plugin/user.c 534, plugin/henkaku.h 42, plugin/language.h 426, plugin/kernel.yml 13, plugin/user.yml 8, plugin/CMakeLists.txt 106, plugin/taihen.json; the two settings XMLs are Paf layouts linked as binary resources). payload/, bootstrap/, krop/, urop/, webkit/ are the exploit chain, not taiHEN plugins, and were not read.

## 1. Process context
- `henkaku.skprx`: kernel plugin (`vita_create_self(henkaku.skprx kernel UNSAFE CONFIG kernel.yml)`, CMakeLists.txt:88-91), first `*KERNEL` entry of the standard config (taiHEN README:49-50). Exports one syscall `henkaku_reload_config` (kernel.yml:9-13).
- `henkaku.suprx`: user plugin for SceShell, `*main` (taiHEN README:63-65), also listed under `*NPXS10015` SceSettings (README:56-57) where its settings hooks apply. Memory: the host process's (module image with two XML resources and the language tables; `g_hooks[15]`; a 24 B config struct). No allocation of its own.

## 2. Module start / stop
- kernel module_start (kernel.c:163-233): `load_config_kernel()` reads `ur0:tai/henkaku_config.bin` (fallback ux0 path) into a 24 B struct, defaults on any error (kernel.c:117-160); then 4 kernel hooks, plus 2 more when version spoofing is on (kernel.c:166-221); every result only LOGged, never checked; if started with a 4-byte argument (the exploit passes SceShell's pid) it calls `taiLoadPluginsForTitleForKernel(shell_pid, "main", 0)` because the exploit runs after SceShell is up (kernel.c:222-230). Always START_SUCCESS.
- kernel module_stop (kernel.c:235-245): releases the 4 (or 6) hooks UNGUARDED (a failed install's negative uid is passed to release).
- user module_start (user.c:430-528): `load_config_user()` (file read; migrates the old config with a write+remove, user.c:95-107); 3 import hooks on SceSettings (user.c:433-449); `g_hooks[3..5] = -1` (user.c:450); `taiGetModuleInfo("SceShell")`; a `switch (info.module_nid)` with 3 known SceShell NIDs installs 3 offset hooks, `default:` logs and skips (user.c:453-514); one import hook on SceShell (user.c:517-521). Always START_SUCCESS. Eight further hooks are installed LAZILY inside the SceSettings load hook when `system_settings_core.suprx` loads (user.c:347-396) and released inside the unload hook (user.c:399-412).
- user module_stop (user.c:530-540): releases hooks 0-6 guarded `>= 0`; hooks 7-14 are not released here (they live with the settings-core module). `g_hooks[0..2]` default 0 if an install was skipped, but all three are always attempted.

## 3. Hooks
| # | module | target | API | installed | released |
|---|---|---|---|---|---|
| K0 | skprx | SceKernelModulemgr import SceSblAuthMgrForKernel 0x7ABF5135::0xF3411881 parse_headers | ImportForKernel | kernel.c:166 | kernel.c:236 |
| K1-2 | skprx | SceSysmem exports 0x3691DA45::0xF8769E86, 0x63A519E5::0xD514BB56 (dynarec checks) | ExportForKernel | kernel.c:174-186 | kernel.c:237-238 |
| K3 | skprx | ScePower import SceSblACMgrForDriver 0x9AD8E213::0x8612B243 | ImportForKernel | kernel.c:189-195 | kernel.c:239 |
| K4-5 | skprx | SceKernelModulemgr 0xD4A60A52::0x5182E212 GetSystemSwVersion; SceSblUpdateMgr 0x31406C49::0x8E3EC2E1 | ExportForKernel, only if spoofing | kernel.c:199-215 | kernel.c:240-243 |
| U0-2 | suprx | SceSettings imports SceLibKernel 0xCAE9ACE6::0x2DCC4AFA / 0x2415F8A4, SceModulemgr 0xEAED1616::0x5182E212 | taiHookFunctionImport | user.c:433-449 | user.c:533-535 |
| U3-5 | suprx | SceShell seg0 code offsets per NID (retail 3.60 0x363de8/0x37beda/0x325230; PTEL; PDEL) | taiHookFunctionOffset thumb | user.c:460-510 | user.c:536-538 |
| U6 | suprx | SceShell import SceVshBridge 0x35C5ACD4::0x333875AB (id.dat check) | taiHookFunctionImport | user.c:517-521 | user.c:539 |
| U7-14 | suprx | SceSettings/SceSystemSettingsCore imports (ScePafToplevel, ScePafMisc, SceRegMgr x5) | taiHookFunctionImport, lazily when system_settings_core loads | user.c:347-396 | in the unload hook user.c:399-412 |
- Partial failure: kernel side unchecked and released unguarded; user side guarded `>= 0`, with `-1` pre-set for the firmware-dependent slots (user.c:450) and a `default:` that skips unknown SceShell NIDs (user.c:508-510).
- Hook bodies: K0 patches the auth args of homebrew SELFs (kernel.c:49-77); U-side registry hooks read and WRITE the config file from inside SceSettings' registry calls (user.c:140-194, `save_config_user`) and call the kernel syscall which spawns a 4 KiB kernel thread per reload (kernel.c:163-176).

## 4. Threads
| name | prio | stack | created | joined |
|---|---|---|---|---|
| configwrite (kernel) | 64 | 0x1000 | per `henkaku_reload_config` syscall, kernel.c:170 | immediately, kernel.c:173-174 (a worker thread used only to get a bigger stack for file I/O) |
- No permanent threads in either module.

## 5. Allocations
| site | API | size | pool | lifetime |
|---|---|---|---|---|
| kernel.c:45, user.c:22 | static `henkaku_config_t config` | 24 B each | .bss | module life |
| kernel.c:47, user.c:24 | static `g_hooks[6]` / `g_hooks[15]` | 24 / 60 B | .bss | module life |
| user.c resources | two Paf XML layouts + language tables (language.h) | tens of KiB of .rodata in SceShell (not measured) | module image | SceShell life |
| kernel.c:170 | thread stack | 4 KiB | kernel | per reload call |
- No AllocMemBlock, heap or paf allocations in either module.

## 6. Firmware assumptions
- Kernel NIDs are 3.60's (SceModulemgrForDriver 0xD4A60A52 etc.); HENkaku ships per-firmware builds. User side: SceShell code offsets keyed on module NID for retail/PTEL/PDEL 3.60 with a logged skip for anything else (user.c:453-510); XML offsets 82/79 into the layout buffers (user.c:300-305) assume Sony's layout files.

## 7. Configuration
- `ur0:tai/henkaku_config.bin`, 24-byte binary with magic and version (henkaku.h:26-36), read by both modules at start and rewritten by the Shell side on settings changes (user.c:71-85); the kernel re-reads on the syscall.

## 8. Logging
- `LOG` -> kernel `printf` / `sceClibPrintf`, compiled in only when not RELEASE (CMakeLists.txt:24-26, henkaku.h:16-24). No files.

## 9. Build artifacts
- `henkaku.skprx` (+ `henkaku-stubs`) and `henkaku.suprx`; user links taihen, HENkaku, SceLibKernel, SceIofilemgr, SceRegistryMgr, ScePower, SceVshBridge stubs; `-nostdlib`, `-fshort-wchar`, `-O3`.

## 10. Patterns worth copying / anti-patterns
- (+) Hooks that depend on a module which loads later are installed inside the hook of that module's load call and released inside its unload call (user.c:347-412): the clean answer to `TAI_ERROR_STUB_NOT_RESOLVED`. The overlay's one-shot Paf bootstrap hook (shell/boot.c) is the same idea for Paf.
- (+) Firmware-dependent slots pre-set to -1 and a `default:` that skips unknown builds (user.c:450, 508-510); unknown = do nothing, never patch blindly.
- (+) Zero allocation in SceShell beyond the image; config is 24 bytes.
- (-) Kernel module_stop releases unguarded; kernel installs unchecked (kernel.c:166-221, 235-245).
- (-) Config file written from inside SceSettings' registry hooks (user.c:150-194).

## 11. Overlay-specific
- n/a.
