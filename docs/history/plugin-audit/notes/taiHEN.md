# taiHEN (yifanlu/taiHEN, commit 309b380, 2018-02-20) — the framework itself
Files read: 23 files, 6717 lines (taihen.c 396, taihen-user.c 986, plugin.c 227, hen.c 537, patches.c 792, proc_map.c 265, slab.c 489, module.c 480, posix-compat.c 71, taihen.h 620, taihen_internal.h 112, plugin.h, patches.h, proc_map.h, slab.h, module.h, hen.h, error.h, exports.yml 57, CMakeLists.txt 92, README.md, USAGE.md; tests/* skimmed) plus taihen-parser/src/parser.c (section semantics) from the vita-memory-deep toolchain tree.

## 1. Process context
- Kernel module `taihen.skprx` loaded by the exploit (HENkaku/Ensō). It is the loader of every other plugin: `*KERNEL` entries are loaded with `ksceKernelLoadStartModuleForPid(KERNEL_PID, ...)` at taiHEN start (taihen.c:356-357, plugin.c:168-176); every other section is loaded into a USER process when that process starts (hen.c:367-388 `start_preloaded_modules_patched` -> `taiLoadPluginsForTitleForKernel(pid, titleid)`).
- Section semantics (taihen-parser parser.c:225-270 + README:44-80): a section name is compared with the started process' title ID; `main` is SceShell's title ID; `ALL` matches every non-KERNEL process ("ALL ... && section != KERNEL", parser.c:242); `KERNEL` only at taiHEN start. Hold L while starting = skip plugins for that process (hen.c:374-380), and hold L at taiHEN start = skip kernel plugins (taihen.c:348-352).
- A user plugin loaded into process P lives in P's memory: its module image (text/data/bss) and whatever it allocates with `sceKernelAllocMemBlock` are charged to P's USER budget; its `malloc`/`sce_paf_*` go to whatever heap it names.

## 2. Module start / stop
module_start (taihen.c:322-359): `proc_map_init`, `patches_init`, `plugin_init`, `hen_add_patches` — EACH checked, each failure returns SCE_KERNEL_START_FAILED (and `hen_add_patches` unwinds its own partial hooks). Then reads the pad; unless L is held, `plugin_load_config()` (fails -> START_FAILED) and `plugin_load_all(KERNEL_PID, "KERNEL")`.
module_stop (taihen.c:379-385): `hen_remove_patches`, `plugin_deinit`, `patches_deinit`, `proc_map_deinit` ("TODO: release everything").

## 3. Hooks (the framework's own)
11 kernel hooks in `hen_add_patches` (hen.c:395-492): SceKernelModulemgr imports (parse_headers 0xF3411881, setup_buffer 0x89CCDA2C, decrypt_buffer 0xBC422443, nid_poison 0xECF9435A), SceNpDrm exports (0x723322B5, 0xDACB71F4, 0xDB406EAE) and imports (0xD78B04A2, 0xF4B98F66), SceKernelModulemgr export start_preloaded_modules 0x432DCC7A, SceProcessmgr import unload_process 0x0E33258E.
- PARTIAL-FAILURE UNWIND, reference implementation: `memset(g_hooks, 0, ...)` first (hen.c:396), `if (g_hooks[k] < 0) goto fail;` after EVERY install, and `fail:` releases each `g_hooks[i] >= 0` (hen.c:494-528) and returns TAI_ERROR_SYSTEM, which module_start turns into START_FAILED. (Note the slight bug: memset to 0, so an untried slot reads 0 and would be "released"; taiHookReleaseForKernel on uid 0 just fails.)
- Hook mechanics (patches.c): first hook on an address patches the function with libsubstitute on a 16 KiB temporary stack (`ksceKernelRunWithStack(0x4000, ...)`, patches.c:263), saving 16 bytes (`FUNC_SAVE_SIZE`); later hooks on the same address chain in a linked list in undefined order (patches.c:364-397). `TAI_CONTINUE` walks the chain. Releasing the head re-patches to the next (patches.c:412-429).
- `taiHookFunctionImportForKernel` returns TAI_ERROR_STUB_NOT_RESOLVED when the import stub still reads `E24FC008 E12FFF1E` (taihen.c:117-121): hooking an import of a library that is not loaded yet fails cleanly. (The overlay's `vjo_import.h` checks the same stub pattern in user space.)

## 4. Threads
- None of its own. Everything runs in the caller's syscall context or the process-start path.

## 5. Allocations
| site | API | size | pool | lifetime |
|---|---|---|---|---|
| patches.c:98 | ksceKernelCreateHeap "tai_patches" | 0x10000 (64 KiB) | kernel heap | taiHEN life; holds inject saved-data (patches.c:502) and libsubstitute's malloc (posix-compat.c:25) |
| proc_map.c:43 | ksceKernelCreateHeap "tai_maps" | MAP_POOL_SIZE | kernel heap | tai_proc_t per hooked process |
| plugin.c:105 | ksceKernelAllocMemBlock "tai_config" KERNEL_RW | config.txt size page-rounded | kernel | until reload |
| slab.c:60-116 | ksceKernelAllocMemBlock "taislab" KERNEL_RX / SHARED_RX / USER_RX + "taimirror" RW mirror | >= 0x1000 per slab per hooked process | kernel for KERNEL_PID; the TARGET PROCESS' USER_RX for user hooks | per process; freed by tai_try_cleanup_process at process exit (hen.c:340-352, patches.c:617-640) |
| patches.c:446-461 | ksceKernelCreateUidObj taiHENClass (sizeof tai_patch_t) | ~ 60 B per patch | kernel UID pool | until release |
- So: every USER-side hook costs the hooked process one 4 KiB executable slab page (plus mirror) the first time, and a tai_hook_t slot thereafter. Kernel hooks cost kernel memory only.

## 6. Firmware assumptions
- `DEFAULT_FW_VERSION 0x3600000` fallback (taihen_internal.h:29); `OFFSET_PATCH_ARG 168` "should be same on all current firmware" (hen.c:21); NIDs of SceKernelModulemgr library 0xC445FA63 are 3.60's (hen.c:466, 487) — HENkaku/Ensō ships per-firmware builds.

## 7. Configuration
- `ux0:tai/config.txt`, fallback `ur0:tai/config.txt` (`TAIHEN_CONFIG_FILE`, `TAIHEN_RECOVERY_CONFIG_FILE`, plugin.c:70-81), read whole into a KERNEL_RW block, validated (`taihen_config_validate`, plugin.c:140) before use; a plugin may ask for a delayed reload (plugin.c:206-227).

## 8. Logging
- `LOG` compiled out unless ENABLE_LOGGING (taihen_internal.h:17-21), then `printf` to the kernel debug printf.

## 9. Build artifacts
- `taihen.skprx` + user/kernel stubs from exports.yml (syscall library `taihen` for user, `taihenForKernel`, `taihenModuleUtils`). Links libsubstitute as a submodule.

## 10. Patterns worth copying / anti-patterns
- (+) `hen_add_patches` is the canonical install-all-or-release-all pattern (hen.c:395-528): zeroed uid array, check after every install, `fail:` label that releases every `>= 0` uid, failure propagated to START_FAILED.
- (+) module_start checks every init step and returns START_FAILED on any of them (taihen.c:322-346); nothing is left half-initialised.
- (+) Process-exit cleanup of user hooks is best-effort only (patches.c:606-640 "It is the caller's responsibility to clean up before it terminates!"): a user plugin that does not release its hooks in module_stop relies on this dirty free, which does NOT restore the patched bytes.
- (+) The hold-L skip is implemented per process start (hen.c:374-380), not just at boot: holding L while launching a GAME skips its `*TITLEID`/`*ALL` plugins too.
- (-) Hook chain order is undefined (patches.c:22-26): two plugins hooking the same NID (e.g. PSVshell and the overlay both on SceCtrl 0xD197E3C7 exports and SceDisplay 0x16466675) run in arbitrary order; each must tolerate the other's modifications.

## 11. Overlay-specific
- n/a (framework). Relevant fact for the overlay: `start_preloaded_modules_patched` passes the started process' title ID (`main` for SceShell, `NPXS…` for system apps) as the section (hen.c:382-386), and the parser records `*ALL` entries for ANY section other than KERNEL (parser.c:242-245). So an `*ALL` plugin is loaded into SceShell, every NPXS system app, VitaShell and every game: an `*ALL` module image is resident in every user process on the console. That is why the overlay's game worker needs the title-ID guard in its module_start (game_ocr/main.c) — and why `*TITLEID` sections avoid the cost entirely.
