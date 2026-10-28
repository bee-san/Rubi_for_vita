# PSVshell (Electry/PSVshell, commit 9a47b0e, 2019-12-21)
Files read: 15 files, 5326 lines (CMakeLists.txt 51, PSVshell.yml 8, README.md, src/main.c 409, src/main.h 51, src/gui.c 743, src/gui.h 99, src/gui_font_ter-u14b.h 524, src/gui_font_ter-u18b.h 1292, src/gui_font_ter-u24b.h 1548, src/oc.c 181, src/oc.h 63, src/perf.c 176, src/perf.h 55, src/profile.c 116, src/profile.h 10)

## 1. Process context
- One module, `PSVshell.skprx`, kernel only: `vita_create_self(${PROJECT_NAME}.skprx ... UNSAFE CONFIG PSVshell.yml)` (CMakeLists.txt:45-48); all APIs are `ksce*`/`taiHookFunctionExportForKernel`. README: "Runs in kernelland (=> visible in LiveArea)". Installed under `*KERNEL`.
- Every byte it owns is kernel memory. It never allocates in SceShell or in a game. It draws INTO the game's framebuffer from the game's own `sceDisplaySetFrameBuf` call (kernel code executing in the game's thread context) with `ksceKernelMemcpyKernelToUser` (gui.c:720-739), so it needs no user-side module at all.

## 2. Module start / stop
module_start (main.c:267-385), in order:
1. `ksceSblAimgrIsGenuineDolce()` (main.c:270).
2. `psvs_gui_init()` allocates the 308x344x4 off-screen GUI buffer, KERNEL_RW (gui.c:703-713). Return value IGNORED by module_start (main.c:273): if the alloc fails, `g_gui_buffer` stays NULL and the draw thread would dereference it.
3. `psvs_profile_init()` makes `ur0:data/PSVshell/profiles/` (profile.c:15-19). File I/O on the boot path, on ur0 (always mounted early).
4. `taiGetModuleInfoForKernel(KERNEL_PID, "ScePower")` + three `module_get_offset` into ScePower data (main.c:275-283) and eight `module_get_export_func` (main.c:285-302); results unchecked.
5. Two mutexes (main.c:304-305), unchecked.
6. `psvs_oc_init()` (main.c:307).
7. 18 hooks `g_hooks[0..17]` (main.c:309-358). Each result is stored but NOT checked; no unwind if one fails. `g_hooks[]` is a zero-initialised static array (main.c:19) so an untried slot would read 0 (>= 0) in module_stop, but every slot is assigned before stop can run.
8. Firmware-dependent exports resolved by trying the 3.60 NID and falling back to the 3.65 NID (main.c:360-379).
9. `taiInjectAbsForKernel` of a 2-byte NOP into ScePervasive (main.c:386-388) — computed from an unchecked function pointer `ScePervasiveForDriver_0xE9D95643 + 0x1D`.
10. Load "main" profile (main.c:391-392): file I/O at boot.
11. Create + start thread `psvs_thread` (main.c:394-395); results unchecked.
12. Returns SCE_KERNEL_START_SUCCESS unconditionally (main.c:397).

module_stop (main.c:400-422): stop flag, `ksceKernelWaitThreadEnd` + delete thread FIRST (main.c:401-405); then release all 18 hooks guarded by `>= 0` (main.c:407-410); release inject (main.c:412-413); delete mutexes guarded (main.c:415-418); `psvs_gui_deinit()` frees the KERNEL_RW block (gui.c:715-718). Order: thread -> hooks -> inject -> mutexes -> memory. Returns STOP_SUCCESS.

## 3. Hooks
| # | target | API | installed | released |
|---|---|---|---|---|
| 0 | SceDisplay 0x9FED47AC::0x16466675 sceDisplaySetFrameBufInternal | ExportForKernel | module_start main.c:309 | module_stop main.c:407-410 |
| 1-8 | SceCtrl 0xD197E3C7:: Peek/Read Buffer Negative/Positive(2) (0x104ED1A7, 0x81A89660, 0xA9C3CED6, 0x15F81E8C, 0x15F96FB0, 0x27A0C5FB, 0x67E7AB83, 0xC4226A3E) | ExportForKernel | main.c:312-327 | same |
| 9-12 | ScePower 0x1590166F:: kscePowerSet{Arm,Bus,GpuEs4,GpuXbar}ClockFrequency | ExportForKernel | main.c:329-336 | same |
| 13 | SceProcessmgr import 0x887F19D0::0x414CC813 ksceKernelInvokeProcEventHandler | ImportForKernel | main.c:338-339 | same |
| 14-17 | ScePower 0x1082DA7F (user lib):: scePowerGet{Arm,Bus,Gpu,GpuXbar}ClockFrequency | ExportForKernel | main.c:341-348 | same |
| inj | ScePervasiveForDriver_0xE9D95643+0x1D, 2 bytes NOP | taiInjectAbsForKernel | main.c:386-388 | main.c:412-413 |
- Partial failure: none handled; module_start never fails. Release is guarded on `>= 0` only.
- Hook bodies: display hook (main.c:60-91) runs in the GAME's thread: calls `psvs_perf_calc_fps`, memcpy of framebuf params, `psvs_perf_poll_memory` (only in FULL mode; reads sysroot address-space data), `psvs_gui_dd_fps` and `psvs_gui_cpy` = per-pixel/per-line `ksceKernelMemcpyKernelToUser` into the game framebuffer. No allocation, no mutex, no I/O in the display hook. Ctrl hooks (main.h:8-14) zero the game's buttons in FULL mode via `ksceKernelMemcpyKernelToUser` (main.c:50-58). Power setter hook takes `g_mutex_cpufreq_uid` (main.c:107-125). Proc-event hook takes `g_mutex_procevent_uid` and does FILE I/O (`psvs_profile_load` -> `ksceIoOpen/Read`) inside the process-event handler (main.c:155-213).

## 4. Threads
| name | entry | prio | stack | affinity | created | stopped | waits on | period |
|---|---|---|---|---|---|---|---|---|
| psvs_thread | psvs_thread main.c:215-265 | 0x3C | 0x3000 (12 KiB) | 0x10000 (CPU... mask arg 0x10000 as in overlay) | module_start main.c:394 | module_stop WaitThreadEnd+Delete main.c:401-405 | `ksceKernelDelayThread(50*1000)` polling, 200 ms while PspEmu | 50 ms |
- Polls the pad (`ksceCtrlPeekBufferPositive` port 0 then 1, main.c:224-228) and draws the whole GUI into the kernel buffer every 50 ms regardless of mode (only mode-dependent sections). No event flag.

## 5. Allocations
| site | API | size | context | pool | lifetime | failure |
|---|---|---|---|---|---|---|
| gui.c:703-706 | ksceKernelAllocMemBlock "psvs_gui" SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW | (308*344*4 + 0xfff) & ~0xfff = 425,984 B (416 KiB; raw 423,808) | module_start, kernel | kernel KERNEL_RW (kernel's own address space) | whole plugin life; freed gui.c:715-718 | returns uid<0 but caller ignores it (main.c:273) |
| main.c:394 | ksceKernelCreateThread stack | 0x3000 = 12,288 B | kernel | kernel thread stack | deleted main.c:405 | unchecked |
| gui_font_ter-u*.h | static const font bitmaps (rodata) | three fonts, ~ (256*14*1)+(256*18*2)+(256*24*2) ~ 3.5+9+12 KiB | kernel module image | module .rodata | module life | n/a |
| main.c:19-21 | static arrays g_hookrefs[18], g_hooks[18], g_injects | ~ 18*4*2 + 4 = 148 B | .bss | module .bss | module life | n/a |
| perf.c/oc.c/profile.c | small statics (< 300 B total) | | .bss | | | |
| gui.c:263 psvs_gui_printf | `char buffer[256]` on stack per printf; gui.c:203 `char buf[4]` | stack | psvs_thread / game thread (display hook) | thread stack | call | n/a |
| profile.c:28 `char path[128]`, 44 `psvs_oc_profile_t oc` | stack | proc-event hook (game or SceShell thread via kernel) | caller's kernel stack | call | n/a |
- No sce_paf, no USER_RW, no CDRAM, no heap. Resident kernel bytes at boot: ~416 KiB KERNEL_RW + 12 KiB stack + module image. Peak during use: the same (nothing grows). Nothing in SceShell, nothing in the game's USER memory.

## 6. Firmware assumptions
- README: 3.60 and 3.65 supported. Export lookups try 3.60 lib NID first then 3.65 (`SceKernelModulemgr` 0xC445FA63 vs 0x92C9FFC2; `SceSysmem` 0x63A519E5 vs 0x02451F0F) (main.c:360-379). Hard-coded ScePower data offsets 0x41C8/0x41CC (main.c:277-279), `sysroot_cas + 328/332/316` (perf.c:111-139), process-local-storage offsets 40..52 (oc.c:122-128), ScePervasive +0x1D inject. Any of these being wrong on another firmware silently corrupts state; none is version-gated except the two NID pairs.

## 7. Configuration
- Binary profiles at `ur0:data/PSVshell/profiles/<titleid>` and `.../global`, `psvs_oc_profile_t` fixed size, version string check `PSVS0100` (profile.c:21-61). Read in the proc-event hook (kernel, inside the game's startup event) and at module_start. Missing file = defaults.

## 8. Logging
- None. No log file, no printf to file. Overlay text itself is the only output.

## 9. Build artifacts
- Single target `PSVshell` -> `PSVshell.skprx`; yml exports only module start/stop (no syscalls). Links `gcc taihenForKernel_stub taihenModuleUtils_stub SceThreadmgrForDriver SceSysclibForDriver SceSysmemForDriver SceProcessmgrForDriver SceSysrootForKernel SceAppMgrForDriver ScePowerForDriver SceCtrlForDriver SceIofilemgrForDriver SceSblAIMgrForDriver` (CMakeLists.txt:30-43); `-nostdlib`, `-Wl,-q`, `-O3 -std=gnu99`. Uses floats in kernel code (gui.c ratio math, perf.c) — no `-mgeneral-regs-only`.

## 10. Patterns worth copying / anti-patterns
- (+) Everything the overlay needs lives in one kernel block allocated once at module_start (gui.c:703-713): no per-frame allocation, no user-side module, nothing charged to SceShell or the game.
- (+) Draws into the game's framebuffer only from inside that game's own `sceDisplaySetFrameBuf` call (main.c:60-91), copying with `ksceKernelMemcpyKernelToUser`: the same-process rule the overlay also follows.
- (+) Skips drawing when `index && ksceAppMgrIsExclusiveProcessRunning()` (main.c:67-68) so it never draws over SceShell's own overlay; skips PspEmu entirely (main.c:64-65, 217-221).
- (+) module_stop order: thread first, then hooks, then memory (main.c:400-422).
- (-) Not one return value in module_start is checked (gui alloc, mutexes, 18 hooks, inject, thread); a NULL `g_gui_buffer` would be dereferenced by the thread.
- (-) File I/O (`ksceIoOpen/Read`) inside the process-event hook (main.c:200-204 -> profile.c:21-61), under a mutex.
- (-) Hard-coded data offsets and a code patch into ScePervasive with no firmware gate.
- (-) 50 ms polling thread that redraws text continuously (CPU cost on whatever core it lands on; unmeasured).

## 11. Overlay-specific
- Draws by writing pixels directly into the game's current framebuffer (`pParam->base`) from the display hook (gui.c:720-739 psvs_gui_cpy; gui.c:177-206 dd_fps). The source is a kernel-resident off-screen buffer of 308x344 RGBA = 416 KiB KERNEL_RW allocated once at module_start (gui.c:703-713), freed at module_stop. The game's memory is never touched except the framebuffer it already owns; the game can use 100 % of its USER memory and PSVshell still works. SceShell is not involved at all.
