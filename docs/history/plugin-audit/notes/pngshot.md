# pngshot (xyzz/pngshot, commit 113d471, 2019-04-06)
Files read: 5 files, 303 lines (main.c 244, CMakeLists.txt 32, exports.yml 8, libpng/VITABUILD 19, README.md)

## 1. Process context
- One user module `pngshot.suprx`, installed under `*main` = SceShell (README: "add `ur0:tai/pngshot.suprx` below `*main`"). It hooks SceShell's own screenshot encoder by code OFFSET inside SceShell's main module (`taiGetModuleInfo(TAI_MAIN_MODULE)`, main.c:176-178) and replaces the JPEG encoder with libpng.
- All memory is SceShell's: libpng's `malloc`/`free` are routed to `sce_paf_private_malloc`/`sce_paf_private_free` (main.c:14-20), i.e. the ScePaf heap of SceShell; one `wh.width*4` row buffer is also taken from that heap per screenshot (main.c:149). `ScePaf_stub_weak` (CMakeLists.txt:27) so the module loads before paf is bound.

## 2. Module start / stop
module_start (main.c:174-241): `taiGetModuleInfo(TAI_MAIN_MODULE)`; dispatch on SceShell `module_nid` (3.60 retail 0x0552F692, 3.60 testkit 0xEAB89D5C, 3.65/3.67/3.68 retail 0x5549BF1F/0x34B4D82E/0x12DAC0F3); for a known NID: three `taiHookFunctionOffset` and two `taiInjectData` (main.c:182-197 etc.). Return values of every hook/inject are DISCARDED (the uids are never stored: only `tai_hook_ref_t` refs are kept, main.c:23). Unknown firmware: does nothing and still `return 0` (= SCE_KERNEL_START_SUCCESS; main.c:240). No file I/O, no threads, no allocation at start.
module_stop (main.c:243-245): `return 0` — releases NOTHING (hook uids were never kept). The plugin cannot be unloaded cleanly; it relies on living as long as SceShell.

## 3. Hooks
| # | target | API | installed | released |
|---|---|---|---|---|
| 1 | SceShell seg0 + 0x247e00 (3.60) etc. "place watermark" | taiHookFunctionOffset (thumb) | module_start main.c:182 | never |
| 2 | SceShell seg0 + 0x365f46 "encode screenshot" (forces format 2) | taiHookFunctionOffset | main.c:185 | never |
| 3 | SceShell seg0 + 0x36bd22 "encode type 2" -> libpng encoder | taiHookFunctionOffset | main.c:188 | never |
| i1 | SceShell seg0 + 0x248840, 2 bytes 0x3b (enable screenshots in any app) | taiInjectData | main.c:192 | never |
| i2 | SceShell seg0 + 0x5148b8, path string "ur0:temp/screenshot/capture.png" | taiInjectData | main.c:196 | never |
| (+ one more ref `reg_hook`/`get_key_int` declared main.c:23-32 but never installed) |
- Partial failure: none checked; no unwind. Hook 3's body (`encode_type2`, main.c:111-171) runs inside SceShell's screenshot thread and allocates from the paf heap via libpng (zlib deflate state: ~256-400 KiB for default window/memlevel) plus one row buffer, all freed at the end (`png_destroy_write_struct`, `sce_paf_private_free`, main.c:162-167). `pixels` malloc result is not NULL-checked (main.c:149-157).

## 4. Threads
- None. Runs on SceShell's screenshot thread when the user takes a screenshot.

## 5. Allocations
| site | API | size | context | pool | lifetime | failure |
|---|---|---|---|---|---|---|
| main.c:149 | sce_paf_private_malloc | width*4 (3840 B at 960) | SceShell screenshot thread | SceShell ScePaf heap | per screenshot, freed main.c:166 | unchecked NULL |
| libpng via main.c:14 malloc | sce_paf_private_malloc | png_struct + info (~1 KiB) + zlib deflate (default: ~ 268 KiB for windowBits 15/memLevel 8) | SceShell screenshot thread | SceShell ScePaf heap | per screenshot | libpng returns NULL -> goto cleanup |
| main.c:98 `g_png_size` | static | 4 B | | .bss | | |
| libpng + zlib static code | module image | ~ 100-150 KiB .text in SceShell | SceShell | module image (USER) | module life | |
- Nothing resident on the paf heap at boot; a transient several-hundred-KiB paf burst per screenshot. This is what "SceShell wants headroom for screenshots" means in practice: the stock encoder (and this replacement) takes its working memory from the SceShell paf heap at screenshot time.

## 6. Firmware assumptions
- Hard-coded SceShell code/data offsets keyed on SceShell module NID for 3.60 retail, 3.60 testkit, 3.65/3.67/3.68 retail (main.c:180, 199, 216-218). Other firmware: inert. README: "If ... you have some weird modifications done to your SceShell, this plugin will probably crash your Vita."

## 7. Configuration
- None.

## 8. Logging
- None.

## 9. Build artifacts
- `pngshot.suprx`, `vita_create_self(... CONFIG exports.yml UNSAFE)`; libpng built with `-DPNG_NO_SETJMP=1 -DPNG_NO_STDIO=1` etc. (libpng/VITABUILD:10); links `png16 z k m gcc taihen_stub ScePaf_stub_weak SceSysmem_stub`; `-nostdlib -O3`; libk provides string functions; `abort()` is `__builtin_trap()` (main.c:10-12) — a libpng error would crash SceShell.

## 10. Patterns worth copying / anti-patterns
- (+) Zero resident footprint: nothing allocated at boot, no thread, no log; memory used only during the user's action and freed right after (main.c:162-167).
- (+) Routing a library's malloc to `sce_paf_private_malloc` (main.c:14-20) is the minimal way to use SceShell's heap from a *main plugin, and `ScePaf_stub_weak` avoids a load-order dependency.
- (-) Hook uids discarded; module_stop releases nothing (main.c:243-245).
- (-) Offsets per SceShell NID with no fallback diagnostics; `abort()` traps SceShell.
- (-) Unchecked `sce_paf_private_malloc` (main.c:149).

## 11. Overlay-specific
- Not an in-game overlay; it is a *main plugin that borrows SceShell's existing screenshot pipeline. Relevant to the overlay because it shows the stock screenshot path allocating its encoder from the SceShell paf heap.
