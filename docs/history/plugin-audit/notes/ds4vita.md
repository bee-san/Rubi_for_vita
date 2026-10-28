# ds4vita (xerpi/ds4vita, commit aad83c9, 2017-07-30)
Files read: 6 files, 929 lines (main.c 783, log.c 50, log.h 31, CMakeLists.txt 57, ds4vita.yml 8, README.md)

## 1. Process context
- One kernel module `ds4vita.skprx` (`vita-elf-create -e ds4vita.yml ... vita-make-fself -c`, CMakeLists.txt:47-50), installed under `*KERNEL` (README: `*KERNEL` / `ux0:tai/ds4vita.skprx`). All `ksce*`, `taiHook*ForKernel`.
- All memory is kernel memory: a private kernel heap (`ksceKernelCreateHeap`, main.c:698), one kernel thread stack, module .bss. Nothing in SceShell or in any game.

## 2. Module start / stop
module_start (main.c:640-716):
1. `log_reset()`: debug builds truncate `ux0:dump/ds4vita_log.txt` (log.c:11-21) — file I/O on the boot path, compiled out with `-DRELEASE` (CMakeLists.txt:20-22).
2. `taiGetModuleInfoForKernel(KERNEL_PID, "SceBt")`; on failure `return SCE_KERNEL_START_FAILED` (main.c:647-651, 713-714) — the only checked step.
3. One offset hook in SceBt + 11 export hooks (SceCtrl x6, SceTouch x4, SceMotion x1) via macros that store `<name>_hook_uid` (main.c:654-693); results unchecked, no unwind.
4. `ksceKernelCreateHeap("ds4vita_mempool", 0x100, &opt)` with `opt.field_8 = 0x10000` (main.c:689-699) — result only logged.
5. `ksceKernelCreateThread("ds4vita_bt_thread", prio 0x3C, stack 0x1000, affinity 0x10000)` + start (main.c:701-704); results only logged.
6. `return SCE_KERNEL_START_SUCCESS`.
module_stop (main.c:726-756): stop flag + `ksceKernelWaitThreadEnd` (timeout 0xFFFFFFFF) + delete thread; delete heap; release each hook guarded by `uid > 0` (UNBIND_FUNC_HOOK, main.c:718-724); `log_flush()`. Order: thread -> heap -> hooks (heap deleted before hooks released; the hooks do not use the heap, only the BT callback does, which the thread unregisters before exiting, main.c:629-631).

## 3. Hooks
| # | target | API | installed | released |
|---|---|---|---|---|
| 1 | SceBt seg0 offset 0x22999C8-0x2280000 (hard-coded 3.60 address) | taiHookFunctionOffsetForKernel thumb | module_start main.c:654-655 | module_stop main.c:740 |
| 2 | SceCtrl ANY::0xF11D0D30 ksceCtrlGetControllerPortInfo | ExportForKernel | main.c:658 | main.c:741 |
| 3 | SceCtrl ANY::0x8F9B1CE5 sceCtrlGetBatteryInfo | ExportForKernel | main.c:662 | main.c:742 |
| 4-7 | SceCtrl ANY:: 0x15F81E8C, 0xC4226A3E, 0x860BF292, 0xA7178860 (Peek/Read Positive2 / Ext2) | ExportForKernel | main.c:666-676 | main.c:743-746 |
| 8-11 | SceTouch ANY:: 0xBAD1960B, 0x9B3F7207, 0x70C8AACE, 0x9A91F624 (ksceTouchPeek/PeekRegion/Read/ReadRegion) | ExportForKernel | main.c:679-689 | main.c:747-750 |
| 12 | SceMotion ANY::0xBDB32767 sceMotionGetState | ExportForKernel | main.c:692-693 | main.c:751 |
- Partial failure: uids initialised to -1 by the DECL macro (main.c:104-107), release guarded `> 0`; install results unchecked, start never fails for a hook.
- Hook bodies: pure data patching with `ksceKernelMemcpyUserToKernel/KernelToUser` of the caller's buffers (main.c:276-299, 388-416); `ksceKernelPowerTick` in touch patch (main.c:401); no allocation, no lock, no I/O. The SceBt offset hook pokes device flags (main.c:460-477).

## 4. Threads
| name | entry | prio | stack | affinity | created | stopped | waits on | period |
|---|---|---|---|---|---|---|---|---|
| ds4vita_bt_thread | main.c:607-636 | 0x3C | 0x1000 (4 KiB) | 0x10000 | module_start main.c:701 | module_stop main.c:728-732 | `ksceKernelDelayThreadCB(200 ms)` so the BT callback runs on it | 200 ms |
- All BT work happens in the callback `bt_cb_func` (main.c:480-605) delivered on this thread; callback created/registered inside the thread and unregistered/deleted by it before exit (main.c:609-612, 629-631).

## 5. Allocations
| site | API | size | context | pool | lifetime | failure |
|---|---|---|---|---|---|---|
| main.c:698 | ksceKernelCreateHeap "ds4vita_mempool" size 0x100, opt.field_8 0x10000 | 256 B heap (grows per opt) | kernel | kernel heap | module life; deleted main.c:734-736 | logged only |
| main.c:139-158 | ksceKernelAllocHeapMemory x2 (SceBtHidRequest ~0x20 B + len+1 B report) | ~ 45 B | BT callback thread | ds4vita_mempool | per report, freed main.c:157-158 (leak of req when buf alloc fails main.c:145-148) | returns -1 |
| main.c:701 | thread stack | 0x1000 = 4096 B | kernel | kernel | deleted at stop | logged |
| main.c:482-483 | static SceBtHidRequest hid_request, unsigned char recv_buff[0x100] | ~ 288 B | .bss | module | module life | |
| main.c:112 `ds4_input` | static struct (packed, aligned 32) | ~ 80 B | .bss | module | | |
| log.c:8 `log_buf` | static char[16*1024] | 16 KiB (debug builds only, `#ifndef RELEASE`) | .bss | module | module life | |
| log.h:16 LOG macro | `char buffer[256]` on stack | 256 B per LOG call, incl. inside the BT callback | caller stack | | | |
- Resident at boot: 4 KiB stack + 256 B heap + ~0.4 KiB .bss (+16 KiB log in debug). Peak: + ~45 B per HID report. Nothing in SceShell, nothing in games.

## 6. Firmware assumptions
- Hard-coded SceBt code offset `0x22999C8 - 0x2280000` (main.c:655) with no firmware check: a 3.60-era address. Export hooks use `TAI_ANY_LIBRARY` so they are NID-stable across firmwares.

## 7. Configuration
- None.

## 8. Logging
- Debug builds only: 16 KiB RAM buffer written to `ux0:dump/ds4vita_log.txt`, truncated at start (log.c:11-21), appended at stop (log.c:36-49); LOG is `(void)0` when RELEASE (log.h:21-24). Release builds do no I/O at all.

## 9. Build artifacts
- `ds4vita.skprx` via `vita-elf-create -e ds4vita.yml` + `vita-make-fself -c`; links `gcc taihenForKernel_stub SceSysclibForDriver SceSysmemForDriver SceSysmemForKernel SceThreadmgrForDriver SceIofilemgrForDriver SceBtForDriver SceKernelSuspendForDriver SceCtrlForDriver`; `-nostdlib`, `-D__VITA_KERNEL__`, `-O3 -std=gnu99` (CMakeLists.txt:9, 28-45). `RELEASE` option compiles out all logging.

## 10. Patterns worth copying / anti-patterns
- (+) Hook uids default to -1 through one macro and are released through one matching macro guarded `> 0` (main.c:104-107, 718-724): no bookkeeping errors possible.
- (+) A tiny private kernel heap for transient request buffers instead of static or KERNEL_RW blocks (main.c:689-699).
- (+) The BT callback's working buffers are static (main.c:482-483), so the callback never allocates.
- (+) Logging compiled out for release; release module_start does no I/O.
- (+) `module_start` returns START_FAILED when its one hard dependency (SceBt module) is missing (main.c:647-651).
- (-) Twelve hook installs, heap and thread creation are not checked; a failed SceBt offset hook (wrong firmware) would be silently ignored and the plugin would run with DS4 connections refused.
- (-) `ds4_send_report` leaks `req` when the second heap allocation fails (main.c:145-148).
- (-) 4 KiB kernel stack with `char buffer[256]` LOG frames inside a callback that also builds `SceBtEvent` and `vid_pid[2]`: tight but works because LOG is compiled out in release.

## 11. Overlay-specific
- Not an overlay (input emulation only). Relevant as the reference for the overlay's SceTouch/SceCtrl hooks (same NIDs: 0xBAD1960B, 0x9B3F7207, 0x70C8AACE, 0x9A91F624, 0x15F81E8C, 0xC4226A3E, 0x860BF292, 0xA7178860) and for patching the caller's buffer in place with user<->kernel memcpy.
