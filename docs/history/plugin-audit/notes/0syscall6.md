# 0syscall6 (SKGleba/0syscall6, commit 940257c, 2020-04-15)
Files read: 19 files, 566 lines (standalone/main.c 40, standalone/CMakeLists.txt 43, standalone/0syscall6.yml 8, for_renga/main.c 45, for_renga/CMakeLists.txt, for_renga/0syscall6.yml, for_hfw/main.c 118, for_hfw/CMakeLists.txt, for_hfw/0syscall6.yml, for_hfw/rvkp/{main.c 32, types.h, Makefile}, for_hfw/smlh/{main.c 13, types.h, Makefile}, sk_rvk_patch/{main.c 20, types.h, Makefile}, README.md). The `psp2renga` headers it includes (`nmprunner.h`, `renga-*.h`) are an external dependency not in this clone; `NMP*` behaviour is inferred from call names and the for_hfw re-implementation only.

## 1. Process context
- Three kernel-plugin variants of one idea (standalone, for_renga, for_hfw), each `*KERNEL` (README "Usage": `ur0:tai/0syscall6.skprx` under `*KERNEL`). Built with `vita-elf-create -e 0syscall6.yml` + `vita-make-fself -c` (standalone/CMakeLists.txt:38-41). The work is a MeP (F00D) payload (sk_rvk_patch/main.c:17-22 writes three halfwords at 0x00804d06..0a) run through an update_sm exploit; all memory is kernel + the venezia SPRAM window at paddr 0x1f850000 (standalone/main.c:24-26).

## 2. Module start / stop
- standalone module_start (main.c:20-35): `NMPcache_ussm("os0:sm/update_service_sm.self", 1)`; set SPRAM corridor paddr/size; `NMPrun_default(&payload)` (synchronous exploit, result IGNORED); `ksceKernelRegisterSysEventHandler("zss_sysevent", handler)` so the patch is re-applied on every resume (crypto reset) (main.c:14-18, 31). Always START_SUCCESS. module_stop (main.c:37-40): nothing (the sysevent handler is NOT unregistered).
- for_renga module_start (main.c:28-38): run the patch, register a renga reset entry; module_stop removes the reset entry (main.c:40-44), the only variant with a symmetric stop.
- for_hfw module_start (main.c:98-114): `NMPcache_ussm`, sysevent handler, `cache_kprxauthsm("os0:zss_ka.elf")` which ALLOCATES a 0xA000 memblock (type 0x1020D006) and reads the file into it (main.c:19-32: alloc and `GetMemBlockBase` results unchecked), then one `taiHookFunctionExportForKernel(0x10005, ..., "SceSblSsSmComm", 0xCD3C89B6, 0x039C73B1, load_sm_data_hook)` whose uid is DISCARDED (main.c:111). module_stop: nothing (main.c:116-118): hook, memblock and handler all leak.

## 3. Hooks
| variant | target | API | installed | released |
|---|---|---|---|---|
| for_hfw | SceSblSsSmComm 0xCD3C89B6::0x039C73B1 load_sm_data | ExportForKernel (pid 0x10005 = KERNEL_PID) | main.c:111 | never |
| all | sysevent handler "zss_sysevent" (resume hook) | ksceKernelRegisterSysEventHandler | standalone main.c:31, for_hfw main.c:105 | never |
- Hook bodies: the for_hfw load_sm hook runs the whole F00D exploit (`patch_rvkc`, main.c:35-77: SPRAM copies, `NMPf00d_jump`, SM stop) inside the kernel's SM-load path after a resume; the sysevent handler in standalone runs `NMPrun_default` directly in the resume event.

## 4. Threads
- None.

## 5. Allocations
| site | API | size | pool | lifetime |
|---|---|---|---|---|
| for_hfw main.c:25 | ksceKernelAllocMemBlock "cached_kprxsm" type 0x1020D006 | 0xA000 = 40 KiB | kernel (type constant hard-coded) | module life; never freed |
| for_hfw main.c:48 | stack `char bkp[0x80]` | 128 B | kernel stack | call |
| all | SPRAM corridor at paddr 0x1f850000, 0x10000 | 64 KiB of venezia SPRAM (not RAM) | hardware | during the exploit |
- Nothing in SceShell or games; kernel resident 40 KiB in the hfw variant only.

## 6. Firmware assumptions
- lv0 MeP code addresses 0x00804d06.. (sk_rvk_patch/main.c:17-21) "lv0 offsets are the same for 3.60-3.65" (for_hfw main.c:85); `NMPexploit_init(fw)` takes a firmware argument; README: use _hfw on hybrid firmware "to prevent wakeup crashes". No runtime firmware check in the standalone variant.

## 7. Configuration
- None (optional `ur0:tai/kprx_auth_sm.bin` / `os0:zss_ka.elf` custom SM).

## 8. Logging
- None.

## 9. Build artifacts
- `0syscall6.skprx` per variant; links SceSblSmCommForKernel, Sysclib, Sysmem, SysrootForKernel, Iofilemgr, KernelSuspendForDriver (+ taihen for renga/hfw); `-nostdlib -D__VITA_KERNEL__ -O3`.

## 10. Patterns worth copying / anti-patterns
- (+) Re-applying a volatile patch on every resume through a sysevent handler (standalone main.c:14-18, 31): suspend/resume resets hardware state, and a plugin that depends on it must expect that. (The overlay's kernel tracks suspend through the proc-event hook, foreground.c:27-35.)
- (-) Exploit payload run synchronously in module_start with its result ignored; a failure (wrong firmware) is invisible and the boot continues with the plugin believing it patched lv0.
- (-) for_hfw leaks a hook uid, a 40 KiB memblock and the sysevent handler; module_stop is empty (main.c:116-118).

## 11. Overlay-specific
- n/a.
