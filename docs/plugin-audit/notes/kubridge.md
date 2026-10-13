# kubridge (TheOfficialFloW/kubridge, commit 40bffc3, 2023-04-10)
Files read: 4 files, 222 lines (CMakeLists.txt 46, exports.yml 17, kubridge.h 62, main.c 97). The repository contains no README, no other sources, no .S files. `ENTER_SYSCALL`/`EXIT_SYSCALL`, `SceKernelAllocMemBlockKernelOpt` (kernel-side definition) and `module_get_export_func` come from vitasdk headers / taihenModuleUtils, which are outside this clone (main.c:1-7); main.c does NOT include the repo's own kubridge.h.

## 1. Process context
- One module, `kubridge.skprx`, kernel: `vita_create_self(kubridge.skprx kubridge CONFIG exports.yml UNSAFE)` (CMakeLists.txt:29); links only kernel stubs `taihenForKernel_stub taihenModuleUtils_stub SceSysclibForDriver_stub SceSysmemForDriver_stub SceThreadmgrForDriver_stub` (CMakeLists.txt:21-27); all APIs are `ksce*` (main.c:32-44) and `module_get_export_func(KERNEL_PID, ...)` (main.c:79-90). Install section is `*KERNEL` by construction (no README in repo to cite; "not found in source").
- Exports a syscall library `kubridge` (`syscall: true`, exports.yml:10-11) with five functions (exports.yml:12-17), so its code runs in kernel on behalf of whatever user process calls it (any game/app linking `libkubridge_stub.a`, CMakeLists.txt:31-36). The module's own memory is kernel memory; the only memory it hands out is created by `kuKernelAllocMemBlock` on behalf of the calling process (main.c:24-49).

## 2. Module start / stop
- module_start (main.c:76-93), in order:
  1. Resolve `ksceKernelCpuDcacheWritebackInvalidateRange` from `SceSysmem` with `TAI_ANY_LIBRARY`: try NID 0x6BA2E51C, on failure try 0x4F442396 (main.c:79-81). Result of the second attempt is not checked.
  2. Same for `ksceKernelCpuIcacheInvalidateRange`: 0xF4C7F578 then 0x2E637B1D (main.c:82-84).
  3. Same for `ksceKernelCpuIcacheAndL2WritebackInvalidateRange`: 0x19F17BD0 then 0x73E895EA (main.c:85-87). This pointer is never used (its only call site is commented out, main.c:20).
  4. `kscePowerGetSysClockFrequency` NID 0xC63DACD5 and `kscePowerSetSysClockFrequency` NID 0x0E333BEC from `ScePower`, results unchecked, no fallback NID (main.c:89-90).
  5. `return SCE_KERNEL_START_SUCCESS` unconditionally (main.c:92).
  Nothing is allocated, hooked, or created at module_start; there is nothing to unwind. If any lookup fails the corresponding function pointer stays NULL (zero-initialised globals, main.c:9-13) and the module still reports success; the first user call to that export dereferences NULL in kernel mode.
- module_stop (main.c:95-97): returns `SCE_KERNEL_STOP_SUCCESS` (main.c:96) and does nothing else. There are no hooks, threads, memblocks or sync objects to release. Memblocks allocated for user processes via `kuKernelAllocMemBlock` are not tracked and are not freed on stop (main.c:24-49 keeps no list).

## 3. Hooks
None. No `taiHook*`/`taiInject*` call exists in main.c; the only taiHEN dependency is `module_get_export_func` from taihenModuleUtils (main.c:7, 79-90). N/A for partial-failure handling.
- Syscall bodies (run in kernel, on the calling user thread's kernel stack):
  - `kuKernelFlushCaches` (main.c:15-22): aligns `ptr`/`len` to 32 bytes (main.c:17-18) and calls Dcache writeback+invalidate then Icache invalidate on the USER virtual address range as given (main.c:19, 21). No `ENTER_SYSCALL`, no range/ownership validation of `ptr`, no NULL check of the function pointers.
  - `kuKernelAllocMemBlock` (main.c:24-49): `ENTER_SYSCALL` (main.c:30); copies the name (≤ 32 B) and the full `SceKernelAllocMemBlockKernelOpt` (0x58 B per kubridge.h:24-47) from user memory (main.c:32-38) — `opt == NULL` therefore fails the copy and the call returns an error; calls `ksceKernelAllocMemBlock(k_name, type, size, &k_opt)` with user-controlled type, size and ALL kernel opt fields (attr, paddr, alignment, mirror_blockid, pid, paddr_list...) (main.c:40); then `ksceKernelCreateUserUid(ksceKernelGetProcessId(), res)` (main.c:44). If `CreateUserUid` fails the kernel memblock is leaked (no free on that path, main.c:44-48). Blocking: yes (allocation).
  - `kuKernelCpuUnrestrictedMemcpy` (main.c:51-65): reads DACR, writes DACR = 0x15450FC3 (main.c:54-55), byte loop using `ldrbt`/`strbt` (unprivileged loads/stores) (main.c:57-61), restores DACR (main.c:63). No `ENTER_SYSCALL`, no length or address validation, interrupts not disabled around the DACR change; `int i` compared with `SceSize len` (main.c:57). Always returns 0 (main.c:64).
  - `kuPowerGetSysClockFrequency` / `kuPowerSetSysClockFrequency` (main.c:67-69, 71-73): direct pass-through to the resolved ScePower function pointers, no validation, no NULL check.

## 4. Threads
None created (no `ksceKernelCreateThread` in main.c). No polling, no periodic work. All code executes on caller threads via syscall.

## 5. Allocations (every one)
| site (file:line) | API | size | process context | pool | lifetime | failure handling |
|---|---|---|---|---|---|---|
| main.c:40 | `ksceKernelAllocMemBlock(k_name, type, size, &k_opt)` — type, size and opt supplied by the user caller | caller-chosen `size` | kernel, inside syscall `kuKernelAllocMemBlock` issued by a user process (game/app) | whatever `type`/`k_opt` select (user-controlled: can be KERNEL_RW, USER_RW, CDRAM, PHYCONT, any pid via `opt.pid`) | returned to the caller as a user uid (main.c:44); never freed by kubridge, not tracked, not released in module_stop | `res < 0 → goto error` (main.c:41-42); leak of the kernel block if `ksceKernelCreateUserUid` fails (main.c:44-48) |
| main.c:25 | stack `char k_name[32]` | 32 B | kernel, caller's kernel stack | kernel thread stack | call | n/a |
| main.c:26 | stack `SceKernelAllocMemBlockKernelOpt k_opt` | 0x58 = 88 B (22 × 4, layout kubridge.h:24-47) | kernel, caller's kernel stack | kernel thread stack | call | n/a |
| main.c:9-13 | static .bss: 5 function pointers | 20 B | kernel module image | module .bss | module life | n/a |
- No `ksceKernelAllocHeapMemory`, no `sce_paf_*`, no `malloc`, no static array ≥ 1 KiB, no thread stack.
- Allocated at module_start: nothing. Resident kernel bytes at boot: module image + 20 B .bss. Peak during use: + ≤ ~124 B transient stack per `kuKernelAllocMemBlock` call; plus whatever user callers allocate through it (unbounded, charged to the pools the caller selects, attributed to the caller's uid space).

## 6. Firmware assumptions
- No `ksceKernelGetSystemSwVersion`. Firmware differences are handled solely by trying two NIDs per SceSysmem cache function with `TAI_ANY_LIBRARY` (main.c:79-87): Dcache WBI 0x6BA2E51C → 0x4F442396; Icache Inv 0xF4C7F578 → 0x2E637B1D; Icache+L2 WBI 0x19F17BD0 → 0x73E895EA. Which pair member belongs to 3.60 vs 3.65+ is not stated in source (no comments).
- ScePower NIDs 0xC63DACD5 / 0x0E333BEC are looked up once with no alternative (main.c:89-90).
- Any lookup failure is silent (second attempt and ScePower unchecked, main.c:81, 84, 87, 89-90); module_start still returns SUCCESS (main.c:92), leaving NULL function pointers that crash the kernel on first use from user space. There is no 3.60/3.65/3.68+ branching beyond the NID pairs; the DACR constant 0x15450FC3 (main.c:55) and the 32-byte cache-line alignment (main.c:17-18) are hard-coded.

## 7. Configuration
None (no file paths or parser in source).

## 8. Logging
None (no `ksceDebugPrintf`/`printf`/file I/O in main.c).

## 9. Build artifacts
- Target `kubridge` from `main.c` only (CMakeLists.txt:17-19); SELF `kubridge.skprx` with `CONFIG exports.yml UNSAFE` (CMakeLists.txt:29).
- exports.yml: module `kubridge`, `attributes: 0`, version 1.0, `main: start module_start / stop module_stop` (exports.yml:1-8); one exported library `kubridge` with `syscall: true` (exports.yml:9-11) and functions `kuKernelAllocMemBlock 0x2EF7C290`, `kuKernelFlushCaches 0x38B70744`, `kuKernelCpuUnrestrictedMemcpy 0x91D9CABC`, `kuPowerGetSysClockFrequency 0x13371337`, `kuPowerSetSysClockFrequency 0x13371338` (exports.yml:12-17). Library NID not specified in the yml (auto-derived by the toolchain).
- Stubs: `vita_create_stubs(stubs kubridge exports.yml KERNEL)` (CMakeLists.txt:31) → `libkubridge_stub.a` and `libkubridge_stub_weak.a` built by target `kubridge_all` (CMakeLists.txt:33-37); `install(DIRECTORY stubs/ DESTINATION lib ... *.a)` and `install(FILES kubridge.h DESTINATION include)` (CMakeLists.txt:39-46) — i.e. the project ships an SDK (header + stubs) for user apps.
- Link flags: `-Wl,-q -Wall -O3 -nostdlib` (CMakeLists.txt:14); `-fno-rtti -fno-exceptions` for C++ though no C++ sources exist (CMakeLists.txt:15). No `-nostartfiles`, no gc-sections. `_start` weak alias of `module_start` (main.c:75).
- Linked libs: `taihenForKernel_stub taihenModuleUtils_stub SceSysclibForDriver_stub SceSysmemForDriver_stub SceThreadmgrForDriver_stub` (CMakeLists.txt:21-27). SceThreadmgrForDriver is linked but no threadmgr function is called in main.c (header included, main.c:4).
- Installation path of the .skprx on device: not found in source (no README).

## 10. Patterns worth copying / anti-patterns
- (+) Two-NID fallback per firmware-sensitive kernel export via `module_get_export_func(..., TAI_ANY_LIBRARY, ...)`, avoiding a per-firmware table (main.c:79-87).
- (+) `ENTER_SYSCALL`/`EXIT_SYSCALL` bracket and `ksceKernelStrncpyUserToKernel` / `ksceKernelMemcpyUserToKernel` copies of all user arguments before use in `kuKernelAllocMemBlock` (main.c:30-38, 47).
- (+) Mapping a kernel-created memblock into the caller's uid namespace with `ksceKernelCreateUserUid(ksceKernelGetProcessId(), res)` (main.c:44).
- (+) Minimal, hook-free, thread-free kernel bridge: nothing to unwind, module_stop trivially correct (main.c:95-97).
- (−) Unchecked lookups + unconditional START_SUCCESS leave NULL kernel function pointers reachable from user space (main.c:81, 84, 87, 89-92) — should return `SCE_KERNEL_START_FAILED` when a required export is missing.
- (−) `kuKernelCpuUnrestrictedMemcpy` and `kuKernelFlushCaches` run with no `ENTER_SYSCALL`, no address/length validation, and the former rewrites DACR (0x15450FC3) without disabling interrupts, with a signed `int i` loop over an unsigned `SceSize len` (main.c:15-22, 51-65). By design this gives any user process arbitrary memory copy; must only be installed on a trusted (homebrew) system.
- (−) `kuKernelAllocMemBlock` forwards the entire kernel opt struct from user space (pid, paddr, attr, mirror uid, paddr_list pointer) (main.c:36-40) — user controls kernel allocation attributes; leaks the block if `ksceKernelCreateUserUid` fails (main.c:44-48); requires a non-NULL `opt` (NULL makes the copy fail, main.c:36-38) which the header does not document (kubridge.h:49).
- (−) Dead code: third cache pointer resolved but never called (main.c:11, 20, 85-87); SceThreadmgr stub linked but unused (CMakeLists.txt:26).

## 11. Overlay-specific
Not applicable — kubridge draws nothing and touches no framebuffer.
