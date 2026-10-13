# nonpdrm (TheOfficialFloW/nonpdrm, commit 8372225, 2021-02-18)
Files read: 4 files, 512 lines (main.c 441, CMakeLists.txt 32, exports.yml 8, readme.md)

## 1. Process context
- One kernel module `nonpdrm.skprx` (`vita_create_self(nonpdrm.skprx ... CONFIG exports.yml UNSAFE)`, CMakeLists.txt:29), `*KERNEL` (readme.md:32-36). All `ksce*` APIs. Kernel memory only.

## 2. Module start / stop
- module_start (main.c:395-415): five `taiHookFunctionExportForKernel` on SceProcessmgr (launch app; 3.60 lib 0x7A69DE86::0x71CF71FD, fallback 0xEB1F8EF7::0x68068618 for newer firmware) and SceNpDrm (0xDB406EAE GetRifInfo, 0x723322B5 GetRifVitaKey, 0xDF62F3B8 GetRifName, 0x17573133 GetRifNameForInstall). Results stored in `hooks[5]`, never checked; always START_SUCCESS (main.c:414). No allocation, no thread, no I/O at start.
- module_stop (main.c:417-431): releases the five hooks in reverse order, each guarded `>= 0`. Complete (nothing else to free).

## 3. Hooks
| # | target | API | installed | released |
|---|---|---|---|---|
| 0 | SceProcessmgr ksceKernelLaunchApp (two NID variants) | ExportForKernel | main.c:396-400 | main.c:427-428 |
| 1-4 | SceNpDrm 0xD84DC44A:: GetRifInfo / GetRifVitaKey / GetRifName / GetRifNameForInstall | ExportForKernel | main.c:403-410 | main.c:418-425 |
- Partial failure: none handled (a failed install leaves -1 in the array, which release skips).
- Hook bodies: the launch hook runs `FindLicenses` = recursive directory walk + file reads + fake-license WRITES on ux0/gro0/grw0 (main.c:150-183, 72-146) INSIDE the kernel's app-launch path, on a 16 KiB stack obtained with `ksceKernelRunWithStack(0x4000, ...)` (main.c:300-308) because the syscall stack is too small for the 512-byte path buffers (main.c:66-68, 157, 282-283). The NpDrm hooks only rewrite output fields.

## 4. Threads
- None. All work on the launching thread with a temporary 16 KiB stack.

## 5. Allocations
| site | API | size | context | pool | lifetime |
|---|---|---|---|---|---|
| main.c:308 | ksceKernelRunWithStack | 0x4000 = 16 KiB temporary kernel stack | kernel, per app launch | kernel | call |
| main.c:66-68, 157, 282-283 | stack buffers path[512], rif_name[48], klicensee[16], new_path[512], license_path[512], SceNpDrmLicense (0x200 = 512 B) x2, SceIoDirent | ~ 2.5 KiB per recursion level | kernel | the 16 KiB RunWithStack | call |
| main.c:30-37 | statics: 5 refs + hooks[5] + n_hooks | 44 B | .bss | module | module life |
- Nothing allocated with AllocMemBlock/heap; zero resident bytes beyond the module image.

## 6. Firmware assumptions
- Two NID variants for the SceProcessmgr launch export (3.60 vs 3.61+; main.c:396-400); readme: 3.60-3.68. SceNpDrm NIDs stable.

## 7. Configuration
- None (fake licenses are generated into `ux0:nonpdrm/<path>/<rif>` from the real work.bin).

## 8. Logging
- None.

## 9. Build artifacts
- `nonpdrm.skprx`; links `SceIofilemgrForDriver SceNpDrmForDriver SceSysclibForDriver SceThreadmgrForDriver taihenForKernel`; `-nostdlib -O3`.

## 10. Patterns worth copying / anti-patterns
- (+) `ksceKernelRunWithStack(0x4000, ...)` to run deep, buffer-heavy code from a hook whose native (syscall/launch-path) stack is small (main.c:300-308; taiHEN itself does the same for libsubstitute, patches.c:263). The overlay keeps its 4 KiB result structs static for the same reason (kernel/game_ocr.c:14).
- (+) Try-the-new-NID-then-the-old-one at install (main.c:396-400) instead of a firmware table.
- (+) Reverse-order guarded release in module_stop (main.c:417-431).
- (-) Heavy filesystem work (recursive scans, writes) inside the app-launch hook delays every app launch; errors are silently ignored; install results never checked.

## 11. Overlay-specific
- n/a.

---

# NoPsmDrm (frangarcj/NoPsmDrm, commit ce80bee, 2023-07-31)
Files read: 4 files, 529 lines (src/main.c 440, src/CMakeLists.txt 33, src/exports.yml 8, README.md)

## 1. Process context
- One kernel module `nopsmdrm.skprx` (`vita_create_self(nopsmdrm.skprx ... UNSAFE)`, src/CMakeLists.txt:30), `*KERNEL` (README.md:29-33). Kernel memory only.

## 2. Module start / stop
- module_start (main.c:400-416): five `taiHookFunctionExportForKernel` (SceProcessmgr launch app with the same two-NID fallback as nonpdrm, main.c:403-406; SceNpDrm PSM library 0x9F4924F2:: GetActInfo 0xB09003A7, GetRifInfo 0x984F9017, GetRifKey 0x8C8CFD01; library 0x3F2B0888:: GetRifNameForInstall 0x0D6470DA). Results unchecked; always START_SUCCESS. BUG: `static SceUID hooks[4]` (main.c:40) but five hooks are stored (`n_hooks` reaches 5): a one-element overflow into the adjacent static (`n_hooks` itself or padding).
- module_stop (main.c:418-432): five guarded reverse releases.

## 3. Hooks
| # | target | API | installed | released |
|---|---|---|---|---|
| 0 | SceProcessmgr ksceKernelLaunchApp (0x71CF71FD or 0x68068618) | ExportForKernel | main.c:403-406 | main.c:429-430 |
| 1-3 | SceNpDrm 0x9F4924F2:: PsmDrm GetActInfo / GetRifInfo / GetRifKey | ExportForKernel | main.c:408-410 | main.c:420-427 |
| 4 | SceNpDrm 0x3F2B0888:: PsmDrmGetRifNameForInstall | ExportForKernel | main.c:412 | main.c:419 |
- Hook bodies: launch hook does registry write + directory scan + file copy on a 16 KiB `ksceKernelRunWithStack` (main.c:224-249); GetRifKey hook WRITES a fake license file to `ux0:/data/<content_id>.rif` inside the DRM call, also on a RunWithStack (main.c:275-349); GetActInfo / GetRifInfo read the registry (`ksceRegMgrGetKeyBin` into a 2 KiB static buffer, main.c:74-93).

## 4. Threads
- None.

## 5. Allocations
| site | API | size | context | pool | lifetime |
|---|---|---|---|---|---|
| main.c:72 | static `char reg_buffer[2048]` | 2 KiB | kernel | .bss | module life |
| main.c:245, 345 | ksceKernelRunWithStack | 0x4000 each (two call sites, nested never) | kernel | temp stack | call |
| main.c:100-102, 226, 262-264 | stack: rif_name[48], new_path[256], ScePsmDrmLicense (0x400 = 1 KiB) x2, license_path[256], path[256], msg[128] per debugPrintf | ~ 3 KiB | kernel temp stack | | call |
| main.c:40-41 | statics hooks[4] + n_hooks + 5 refs | ~ 40 B | .bss | | module life |
- No AllocMemBlock / heap.

## 6. Firmware assumptions
- README: 3.60 only; launch-hook NID fallback for newer firmware (main.c:403-406). PSM NIDs hard-coded.

## 7. Configuration
- None; relies on `FAKE.rif` placed in the PSM license folder and the registry key `/CONFIG/NP/account_id`; writes registry `/CONFIG/PSM/revocation_check_req = 0` at each PSM launch (main.c:239).

## 8. Logging
- `#ifdef DEBUG` only: append to `ux0:data/nopsmdrm.txt` with open/write/close per line (main.c:57-67), 128-byte stack message buffer (main.c:70).

## 9. Build artifacts
- `nopsmdrm.skprx`; links Iofilemgr, NpDrm, Sysclib, Sysmem, Threadmgr, RegMgr ForDriver + taihenForKernel; `-nostdlib -O3`.

## 10. Patterns worth copying / anti-patterns
- (+) Same RunWithStack and two-NID patterns as nonpdrm (main.c:245, 403-406).
- (+) Debug logging compiled out by default (main.c:57-67).
- (-) `hooks[4]` holding five uids (main.c:40 vs 403-412): static-array overflow — the kind of bookkeeping bug a single uid-array-plus-count macro (ds4vita) avoids.
- (-) File writes inside DRM kernel calls; unchecked installs; always START_SUCCESS.

## 11. Overlay-specific
- n/a.
