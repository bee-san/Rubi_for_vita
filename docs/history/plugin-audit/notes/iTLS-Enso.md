# iTLS-Enso (SKGleba/iTLS-Enso, commit 52de568, 2023-09-22, v3.2)
Files read: 12 files, 880 lines (kernel/main.c 68, kernel/CMakeLists.txt 46, kernel/itls.yml 8, old_dns_fix/main.c 36, old_dns_fix/CMakeLists.txt, old_dns_fix/exports.yml, main.c 280 (installer app), graphics.c 181, graphics.h, font.c 146, Makefile, README.md; misc/*.suprx are prebuilt Sony modules, not read)

## 1. Process context
- `itls.skprx`: kernel plugin, `*KERNEL` (the installer app writes `# ITLS\n*KERNEL\nur0:tai/itls.skprx` into `ur0:tai/config.txt`, main.c:94-101, 114-117). Kernel memory only.
- `old_dns_fix.suprx`: a user plugin for SceShell that is NOT listed in config.txt: the kernel plugin loads it into SceShell itself with `ksceKernelLoadStartModuleForPid(pid, ...)` when SceShell loads libssl (kernel/main.c:41-44, `pid == ksceKernelSysrootGetShellPid()`). Its memory is SceShell's (module image + one hook).
- The installer (main.c) is a VPK app that copies modules to vs0:/ur0: and edits the tai config; not a plugin.

## 2. Module start / stop
- kernel module_start (kernel/main.c:48-58): `ksceDebugPrintf`; `add_replace_flags()` stats the five replacement module files on vs0 (kernel/main.c:11-21: five `ksceIoGetstat` on the boot path); if any is present: `ksceKernelSearchModuleByName("SceKernelModulemgr")` and ONE `taiHookFunctionOffsetForKernel(KERNEL_PID, ..., modid, 0, 0x21ec, 1, load_for_pid_patched)`; `if (modid < 0 || hk < 0) return SCE_KERNEL_START_FAILED` (kernel/main.c:52-55). With no replacement file present the plugin starts and does nothing (no hook).
- kernel module_stop (kernel/main.c:60-65): releases the hook if `hk >= 0`. Note `hk` is initialised to 0 (kernel/main.c:23), so when no hook was installed module_stop calls `taiHookReleaseForKernel(0, ...)`, which fails harmlessly.
- old_dns_fix module_start (old_dns_fix/main.c:23-29): `taiGetModuleInfo("SceShell")` then one `taiHookFunctionImport("SceShell", 0xE8F15CDE, 0xAEB3307E, httpConnPatched)`; always START_SUCCESS; `httpConnHookID` is an uninitialised static (0) if the hook was skipped. module_stop releases if `>= 0`.

## 3. Hooks
| module | target | API | installed | released |
|---|---|---|---|---|
| itls.skprx | SceKernelModulemgr seg0 + 0x21ec (load module for pid), thumb | taiHookFunctionOffsetForKernel | module_start kernel/main.c:52 | module_stop kernel/main.c:62-63 |
| old_dns_fix.suprx (in SceShell) | SceShell import SceHttp 0xE8F15CDE::0xAEB3307E sceHttpCreateConnection | taiHookFunctionImport | module_start old_dns_fix/main.c:26-27 | module_stop :32-34 |
- Hook body (kernel/main.c:27-46): string compares on the module path and pointer swaps to the vs0 replacement paths; after the original load, a `ksceKernelLoadStartModuleForPid` of old_dns_fix into SceShell once. No allocation, no lock.

## 4. Threads
- None.

## 5. Allocations
| site | API | size | pool | lifetime |
|---|---|---|---|---|
| kernel/main.c:9-10 | static `new_modules[5]` pointers + `replace[5]` | ~ 30 B | kernel .data/.bss | module life |
| kernel/main.c:12 | stack `SceIoStat stat` | ~ 88 B | kernel stack at boot | call |
| old_dns_fix | module image in SceShell + 2 statics | small (a 36-line module) | SceShell USER (module image) | SceShell life |
- No AllocMemBlock, no heap, no thread. Kernel resident: module image only. SceShell resident: the old_dns_fix image (tiny) and one taiHEN user slab page (4 KiB USER_RX + mirror, see taiHEN note section 5).

## 6. Firmware assumptions
- Hard-coded SceKernelModulemgr code offset 0x21ec (kernel/main.c:52) with no firmware/NID check (the project targets 3.60/3.65 Ensō). The "3.60 dns fix" branch is gated on `replace_comm2 == 2` (kernel/main.c:41), i.e. on which replacement files the installer put on vs0, not on the firmware version.

## 7. Configuration
- None at runtime; presence of files under `vs0:/data/external/itls/` selects behaviour (kernel/main.c:11-21).

## 8. Logging
- One `ksceDebugPrintf` at start (kernel/main.c:50). Nothing else.

## 9. Build artifacts
- `itls.skprx` via `vita-elf-create -e itls.yml` + `vita-make-fself -c`; links Threadmgr, Sysclib, Cpu, taihenModuleUtils, taihenForKernel, Sysmem, Iofilemgr, Debug, Modulemgr, Sysroot ForDriver stubs; `-nostdlib -D__VITA_KERNEL__ -O3`. `old_dns_fix.suprx` via `vita_create_self`. The VPK installer builds with vita2d-less raw framebuffer graphics (graphics.c, font.c).

## 10. Patterns worth copying / anti-patterns
- (+) module_start returns START_FAILED when its one hook cannot be installed (kernel/main.c:52-55): nothing half-installed stays behind.
- (+) Feature flags from file presence instead of a parser (kernel/main.c:11-21); zero runtime allocation.
- (+) A kernel plugin that needs code inside SceShell loads that module itself at the right moment (`ksceKernelLoadStartModuleForPid` once SceShell has libssl, kernel/main.c:41-44) instead of a `*main` entry that must wait for its dependencies, which is the problem the overlay's Shell boot hook solves by waiting for `sceAVConfigRegisterCallback` (shell/boot.c:62).
- (-) Hook uid default 0 (not -1) leads to a spurious release call (kernel/main.c:23, 62); old_dns_fix has the same uninitialised-uid pattern.
- (-) Five file stats on the boot path on vs0 (fast, but a blocking I/O dependency at kernel plugin start).
- (-) Hard-coded code offset without a module-NID check.

## 11. Overlay-specific
- n/a.
