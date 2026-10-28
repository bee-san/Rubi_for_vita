# yamt-vita (SKGleba/yamt-vita, commit 2eb966b, 2020-08-09, YAMT-V 4)
Files read: 17 files, ~2200 lines (plugin/kernel/{main.c 182, tai_compat.c 52, tai_compat.h 57, defs.h 82, yamt.yml 15, CMakeLists.txt}, plugin/kernel_lite/{main.c 109, tai_compat.c 43, tai_compat.h, defs.h 31, yamt.yml}, plugin/helper/{main.c 199, logging.h 39, hyamt.yml}, plugin/user/{user.c 443, FatFormatProxy.h 53, user.yml}, plugin/user_lite/user.c 227, README.md; src/ (the installer VPK) skimmed only).

## 1. Process context
- `yamt.skprx`: kernel module loaded by Enso's `boot_config.txt`, i.e. BEFORE taiHEN (README:34), which is why it carries its own `module_get_offset` re-implementation and finds `getModuleInfo` by decoding bytes of `ksceKernelSearchModuleByName` (tai_compat.c:12-27). Memory: kernel statics only.
- `yamt_helper.skprx`: kernel plugin under `*KERNEL` (README:36), exports one syscall (hyamt.yml:9-13).
- `yamt.suprx`: user plugin for SceSettings, `*NPXS10015` (README:35), the same lazy settings-core hook pattern as henkaku's user plugin (user.c:381-412, 418-441). Nothing in SceShell or games.

## 2. Module start / stop
- yamt.skprx module_start (main.c:146-180): zero statics; read `ur0:tai/yamt.cfg` (binary, 140 B, main.c:149-155) with defaults on failure; `tai_init()` (firmware sniff, -1 = unknown); `if (cfg.ion == 0 || cfg.vec > 16 || newfw < 0) return SCE_KERNEL_START_SUCCESS;` (main.c:160-161, fail-inert); a raw 2-byte INJECT into SceSysmem (main.c:165-167); `patch_redirect()` rewrites SceIofilemgr mount-point device structs and injects device-name strings (main.c:76-104); starts a 4 KiB thread `x` that remounts the redirected partitions and later loads a patched umass driver (main.c:33-56, 176); always START_SUCCESS. module_stop (main.c
:182-185): returns STOP_SUCCESS and undoes nothing (raw memory patches have no saved originals; the module is a boot-time storage redirector and is never meant to unload).
- yamt_helper module_start (helper/main.c:101-199): checks that yamt loaded (`ksceKernelSearchModuleByName`, :104-108), resolves its exports, waits on a GPIO flag with 50 ms delays (:122-126), then mounts/loads the USB driver; returns SUCCESS either way. kernel_lite is the same with a 2-entry fixed table (kernel_lite/main.c).
- yamt.suprx module_start (user.c:418-432): two import hooks on SceSettings' module load/unload, like henkaku; the four settings-core hooks are installed in the load hook and released in the unload hook (user.c:381-412); module_stop releases all six guarded `>= 0` (user.c:434-441).

## 3. Hooks
- yamt.skprx uses NO taiHEN hooks (it loads before taiHEN): raw code/data writes with the DACR disabled (`INJECT`, `INJECT_NOGET`, `NOP_MOD_RANGE`, tai_compat.h:7-56) into SceSysmem (+0x21610), SceIofilemgr (device-name table at 0x1D340/0x1D498 - 0xA0 by firmware, main.c:77), SceUsbServ (+0x22ec) and the umass driver (+0x1546) (main.c:21-26, 101, 165-167). Originals are never saved; nothing can be released.
- yamt.suprx: 6 `taiHookFunctionImport` on SceSettings / SceSystemSettingsCore (user.c:381-399, 421-431), released guarded.
- Partial failure: `INJECT` silently does nothing when the module is not found (`if (modid >= 0)`, tai_compat.h:44-47) and `module_get_offset`'s result is ignored (a NULL `addr` would be written through).

## 4. Threads
| name | prio | stack | created | ends |
|---|---|---|---|---|
| "x" (yamt.skprx) | 0x00 (highest) | 0x1000 | module_start main.c:176, create+start in one expression, results unchecked | `ksceKernelExitDeleteThread` after remounts + 500 ms delay + optional driver load (main.c:33-56) |
- The thread exists only to take the slow mount calls off the boot path ("ksceIoMount threaded to save time", main.c:175).

## 5. Allocations
| site | kind | size |
|---|---|---|
| main.c:9-15 | statics: cfg 140 B, mids[16], convc[16][64] 1,024 B, custom[16] SceIoDevice 320 B, gpio 16 B | ~1.6 KiB .bss |
| main.c:176 | thread stack | 4 KiB, freed when the thread exits |
| helper/logging.h | log buffer only if LOGGING_ENABLED (0) | compiled out |
- No AllocMemBlock, heap or paf in any module; the user plugin has `g_hooks[6]` and a 24 B config.

## 6. Firmware assumptions
- 3.60 vs 3.65 decided by sniffing the bytes of a kernel function and checking a `push` opcode at two candidate offsets (tai_compat.c:12-27); unknown -> `-1` and the plugin becomes inert (main.c:160). Every offset (SceSysmem 0x21610, SceIofilemgr tables, SceUsbServ 0x22ec, umass 0x1546, sysroot +0x6c/+0xD8, sdif +0x2430/+0x24) is hard-coded for those two firmwares (main.c:21-26, 60-66, 77-78, 165).

## 7. Configuration
- `ur0:tai/yamt.cfg`, 140-byte binary with version 4 (defs.h:1-17), read at kernel start; written by the Settings-side plugin (user.c:160-170).

## 8. Logging
- helper: `LOG` to `ur0:temp/yamt_helper.log` only when `LOGGING_ENABLED` (0 in the release source, helper/main.c:6-7). Kernel module: none.

## 9. Build artifacts
- `yamt.skprx` (exports `kyamt` kernel library, yamt.yml:9-15), `yamt_lite.skprx`, `yamt_helper.skprx` (syscall `yamtUserCmdHandler`), `yamt.suprx`, `yamt_lite.suprx`; installer VPK under src/. All `-nostdlib`.

## 10. Patterns worth copying / anti-patterns
- (+) Fail-inert: with an unknown firmware or a disabled config the kernel module returns SUCCESS having touched nothing (main.c:160-161).
- (+) Slow I/O (mount/unmount, driver load with a 500 ms wait) moved off the boot path onto a short-lived thread that deletes itself (main.c:33-56, 176).
- (+) The settings UI is a `*NPXS10015` plugin that exists only inside SceSettings, not a `*main` plugin.
- (-) Raw DACR-off memory writes with no saved originals and no verification of the target bytes; `module_get_offset` results ignored (tai_compat.h:44-56).
- (-) Thread created and started in one unchecked expression (main.c:176); a highest-priority (0x00) kernel thread doing filesystem mounts.

## 11. Overlay-specific
- n/a.
