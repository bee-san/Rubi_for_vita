# ref00d (dots-tb/ref00d, commit f3271bc, 2023-05-06 16:56:36 +0900 "improve the code", author Princess-of-Sleeping)
Files read: 15 files, 5456 lines (every file in the clone except LICENSE, 339 lines of GPL text):
CMakeLists.txt 61, README.md 41, build.sh 9, exports.yml 29 (wc -l says 28: last line unterminated), src/main.c 173, src/ref00d_kprx_auth.c 640,
src/ref00d_kprx_auth.h 288, src/ref00d_rsa_engine.c 162, src/ref00d_rsa_engine.h 15, src/ref00d_types.h 17,
src/ref00d_utils.c 82, src/ref00d_utils.h 16, src/ref00d_aes_cbc.S 60, src/self.h 102, src/elf.h 3762
(stock glibc elf.h, FSF 1995-2016, src/elf.h:1-2; included at src/main.c:28 and src/ref00d_kprx_auth.c:16 but
no Elf*/PT_/SHT_/EM_ symbol from it is referenced by any .c/.S file - grep-verified; read head/tail + symbol scan only).
Repo total is 5795 lines (wc). Source-only audit; README cited only for the install line / supported-FW claim.

## 1. Process context
- One module, `reF00D.skprx`, a kernel plugin: built with `-D__PSP2_KERNEL__` (CMakeLists.txt:46), linked against
  `*ForDriver_stub`/`*ForKernel_stub` + `taihenForKernel_stub` (CMakeLists.txt:27-41), uses only `ksce*` APIs and
  `taiHookFunctionImportForKernel` (src/main.c:34, :166-170), `.skprx` suffix (CMakeLists.txt:49). README install line:
  "Add reF00D.skprx path to your taihen config.txt" (README.md:14) and "must be used in taihen config.txt", i.e. not
  via a plugin loader (README.md:15). The README never names the section; from the evidence above it is `*KERNEL`.
- Memory: kernel only. All allocations go to kernel heap UID 0x1000B (src/ref00d_kprx_auth.c:127, :159); one kernel
  thread (src/ref00d_rsa_engine.c:132). Nothing is mapped into SceShell, a game, or any user process.
- Hook bodies execute in kernel mode on whatever thread is inside SceKernelModulemgr's loader when it calls the five
  `sceSblAuthMgr*ForKernel` imports (src/main.c:166-170): the kernel boot thread for boot-time modules, or the
  syscall context of the SceShell/game thread that requested a module load. Which thread that is is not
  determined by this repo.

## 2. Module start / stop
module_start (src/main.c:148-173; `_start` is a weak alias, src/main.c:147). Steps in order:
1. `ref00d_rsa_engine_initialization()` (src/main.c:152): creates mutex "Ref00dRsaMutex" (src/ref00d_rsa_engine.c:114),
   semaphore "Ref00dRsaSema" init 1 / max 1 (:120), event flag "Ref00dRsaComm" (:126), thread "Ref00dRsaEngine"
   (:132) and starts it (:138). Internal unwinding is correct reverse order via gotos: start failure deletes the
   thread (:145-147), then the event flag (:149-151), the semaphore (:153-155), the mutex (:157-159), and the error is
   returned (:142-143, :161). Slip: the `del_sema` branch resets `ref00d_rsa_comm_evfid` instead of
   `ref00d_rsa_semaid` (src/ref00d_rsa_engine.c:155). On any error module_start calls `SCE_KERNEL_PANIC()`
   (src/main.c:153-155) - a kernel panic, not SCE_KERNEL_START_FAILED.
2. `semaid = ksceKernelCreateSema("Ref00dSema", 0, 1, 1, NULL)` (src/main.c:157); failure -> `SCE_KERNEL_PANIC()`
   (src/main.c:158-160). Nothing from step 1 is undone before the panic (moot, the device panics).
3. `ref00d_kprx_auth_initialization()` (src/main.c:162): `pKprxKey = NULL` (src/ref00d_kprx_auth.c:151),
   `kprxAuthKeysSetup()` adds the two built-in keys (:141-142; return values of `ref00d_kprx_add_key` ignored, the
   function always returns 0, :144), then allocates the 0x103F-byte private header buffer (:159) and aligns it to
   64 bytes (:165). Allocation failure -> `SCE_KERNEL_PRINTF_LEVEL(1, ...)` + return -1 (:160-162) ->
   `SCE_KERNEL_PANIC()` (src/main.c:163). Key nodes allocated in :141-142 are not freed before the panic.
4. Five `HookImport(...)` calls (src/main.c:166-170). The macro expands to `taiHookFunctionImportForKernel(0x10005,
   &<fn>_ref, module, libnid, funcnid, <fn>_patched)` (src/main.c:34); the returned hook UID is discarded in every
   call (no assignment, grep-verified: no SceUID/uid variable exists in src/main.c). Results are not checked.
5. `return SCE_KERNEL_START_SUCCESS` (src/main.c:172) - the only return statement; FAILED / NO_RESIDENT never returned.
- Unwinding summary: there is none. Steps 1-3 panic on failure; step 4 cannot fail visibly; if hook k of 5 fails,
  hooks 1..k-1 stay installed with their UIDs lost.
- module_stop: not found in source (grep over src/ and exports.yml). exports.yml declares only `main: start:
  module_start` (exports.yml:6-7) and `attributes: 7` (exports.yml:2; bits 0x1|0x2|0x4 - in the SCE module-attribute
  encoding used by the toolchain these are CANT_STOP|EXCLUSIVE_LOAD|EXCLUSIVE_START; the definitions are not in this
  repo). Consequently no hook is ever released (`HookRelease` macro at src/main.c:37-39 has zero call sites), the
  RSA thread is never stopped/joined, the two heap blocks are never freed (no `ksceKernelFreeHeapMemory` anywhere,
  grep-verified), and the four sync objects persist until reboot.

## 3. Hooks
All five are import hooks on module "SceKernelModulemgr", library NID 0x7ABF5135 (SceSblAuthMgrForKernel),
installed with `taiHookFunctionImportForKernel(KERNEL_PID=0x10005, ...)` (src/main.c:34).

| # | target (module, lib NID, func NID -> import) | API | installed where | released where |
|---|---|---|---|---|
| 1 | SceKernelModulemgr, 0x7ABF5135, 0xA9CD2A09 sceSblAuthMgrOpenForKernel | taiHookFunctionImportForKernel | module_start src/main.c:166 | never (UID discarded) |
| 2 | SceKernelModulemgr, 0x7ABF5135, 0x026ACBAD sceSblAuthMgrCloseForKernel | taiHookFunctionImportForKernel | module_start src/main.c:167 | never |
| 3 | SceKernelModulemgr, 0x7ABF5135, 0xF3411881 sceSblAuthMgrAuthHeaderForKernel | taiHookFunctionImportForKernel | module_start src/main.c:168 | never |
| 4 | SceKernelModulemgr, 0x7ABF5135, 0xBC422443 sceSblAuthMgrLoadBlockForKernel | taiHookFunctionImportForKernel | module_start src/main.c:169 | never |
| 5 | SceKernelModulemgr, 0x7ABF5135, 0x89CCDA2C sceSblAuthMgrLoadSegmentForKernel | taiHookFunctionImportForKernel | module_start src/main.c:170 | never |

- Partial-failure handling: none. No uid array exists, so nothing is initialised to -1; the unused `HookRelease`
  macro would guard on `hook_uid > 0` (src/main.c:38). Install results are unchecked (src/main.c:166-170). If e.g.
  hook 3 fails but 4-5 succeed, nothing breaks (`ref00d_kprx_auth_state()` stays failing, src/ref00d_kprx_auth.c:634-639,
  so 4-5 always `TAI_CONTINUE`, src/main.c:125-126, :138-139). If 3 succeeds but 4 or 5 fails, ref00d can claim a
  header (`is_auth_success = 1`, src/ref00d_kprx_auth.c:525) while the unhooked original decrypts the blocks ->
  that module load fails; no crash path in ref00d itself.
- Bodies (what each does, and what can block):
  - #1 Open (src/main.c:86-95): `TAI_CONTINUE` first (:90), then `ref00d_auth_open(&ref00d_ctx)` (:92) whose result
    is ignored; that call sets `is_auth_success = 0` (src/ref00d_kprx_auth.c:476) and does an untimed
    `ksceKernelWaitSema(semaid, 1, NULL)` (:465) - BLOCKS until the previous Open/Close pair finishes. It is called
    even when the original Open failed (`ret` is not inspected, src/main.c:90-92), so correct operation depends on
    SceKernelModulemgr always pairing Open with Close; otherwise the semaphore is held forever and the next module
    load deadlocks inside this hook.
  - #2 Close (src/main.c:97-107): `TAI_CONTINUE` (:101), then `ref00d_auth_close(ref00d_ctx)` (:103) which signals
    the semaphore only if `ctx == 1` (src/ref00d_kprx_auth.c:497-502), then `ref00d_ctx = 0` (src/main.c:104).
  - #3 AuthHeader (src/main.c:109-119): tries `ref00d_auth_header` FIRST (:113) and only falls back to the
    original when it returns < 0 (:114-115). The body copies up to 0x1000 bytes (src/ref00d_kprx_auth.c:307-310),
    parses control info (:336-353), looks keys up (:358-374), then for a matching key runs AES-CBC
    (:235, :244), SHA-256 (:274) and an RSA-2048 verify handed to the RSA thread: `ref00dRsaEngineRequest` locks
    "Ref00dRsaMutex" and waits "Ref00dRsaSema" (src/ref00d_rsa_engine.c:65, :69), `ref00dRsaEngineWaitWork`
    locks the mutex again and does an untimed `ksceKernelWaitEventFlag` for REQ_DONE (:90, :94) - BLOCKS on the
    RSA thread. Also calls `ksceSblAimgrIsDEX/IsTest/IsTool/IsCEX` and `kscePmMgrGetProductMode`
    (src/ref00d_kprx_auth.c:413-439). No allocation, no file I/O. Note the compared `ctx` (:513) is the one
    supplied by modulemgr, i.e. the real auth manager's context id, which the code requires to equal 1; ref00d's
    own `ref00d_ctx` is only used on the Close path (src/main.c:103).
  - #4 LoadBlock (src/main.c:121-132): if `ref00d_kprx_auth_state() < 0` -> original (:125-126), else
    `ref00d_load_block` (:128) which decrypts in place: software AES-128-CTR via `ksceAesEncrypt1` per 16-byte
    block when `len < 0x300` (src/ref00d_kprx_auth.c:550-553; src/ref00d_utils.c:53-81), else hardware
    `ksceSblDmac5AesCtrDec` (:555). Non-AES-CTR segments are passed through untouched with return 0 (:562-566).
  - #5 LoadSegment (src/main.c:134-145): same gate (:138-139), else `ref00d_setup_segment` (:141) which
    selects the segment entry (src/ref00d_kprx_auth.c:576-577), sets `currentKey` (:581), initialises the AES
    context (:584), byte-swaps the IV for DMAC (:587) and returns the segment's compression flag (:589).
- No hook body takes a mutex other than the RSA handshake above; none does I/O; none allocates.

## 4. Threads
| name | entry | priority | stack bytes | cpu affinity | created where | stopped/joined where | waits on | period |
|---|---|---|---|---|---|---|---|---|
| Ref00dRsaEngine | `ref00dRsaEngine` (src/ref00d_rsa_engine.c:31) | 0x5E (94) | 0x2000 (8192) | 0xF (all cores); attr 0 | `ksceKernelCreateThread` src/ref00d_rsa_engine.c:132, started :138, from module_start via `ref00d_rsa_engine_initialization` (src/main.c:152) | never (no module_stop); deleted only if `ksceKernelStartThread` fails (:145-146) | event flag "Ref00dRsaComm" bit REQ (1<<0), WAITOR \| WAITCLEAR_PAT, no timeout (:37) | event-driven, none |

- The thread loops `while(1)` (src/ref00d_rsa_engine.c:36); a failed wait does `continue` (:38-39) with no delay, so
  it would spin at priority 0x5E if the event flag were ever invalid (it is never deleted, so this is latent only).
  Per request it runs `ksceSblRSA2048VerifySignature` over a 0x100-byte signature and 0x20-byte hash with the key's
  modulus (:42-46), stores the result (:49-52, non-OK mapped to 0x800F0024), and sets REQ_DONE (:55).
- No polling, no per-frame work. The caller side (module-loader thread) blocks on REQ_DONE (:94). Hand-off of the
  request pointers happens through globals `rsa_m/rsa_k/rsa_n/rsa_hash` (:17-20, :71-74) guarded by the mutex +
  binary semaphore (:65-69, :98).

## 5. Allocations (every one)
| site | API | size | process context | pool | lifetime | failure handling |
|---|---|---|---|---|---|---|
| src/ref00d_kprx_auth.c:127 | `ksceKernelAllocHeapMemory(0x1000B, sizeof(SceKprxAuthKey))` | sizeof(SceKprxAuthKey): the header comment says 0x180 (src/ref00d_kprx_auth.h:234) but with 8-aligned `uint64_t minver` (:238) the natural layout is 0x188 = 392 B (padding after `iv`, tail pad) - compile-time checked; x2 at boot (:141-142) = 784 B; +392 B for every later `ref00d_kprx_add_key` call by another kernel module (exported, exports.yml:25-29) - no dedupe, no cap | kernel (module_start boot thread; later: the exporter's caller) | kernel heap 0x1000B | never freed | NULL -> return -1 (:128-129), ignored by `kprxAuthKeysSetup` (:141-144) |
| src/ref00d_kprx_auth.c:159 | `ksceKernelAllocHeapMemory(0x1000B, 0x103F)` | 4159 B (0x1000 usable after 64-byte align-up at :165) | kernel, module_start | kernel heap 0x1000B | never freed | NULL -> printf level 1 + return -1 (:160-162) -> `SCE_KERNEL_PANIC()` (src/main.c:163) |
| src/ref00d_rsa_engine.c:132 | thread stack via `ksceKernelCreateThread(..., 0x2000, ...)` | 8192 B | kernel, module_start | kernel thread stack (threadmgr) | never freed (thread never deleted after a successful start) | < 0 -> goto del_evf (:133-134) |
| src/ref00d_kprx_auth.c:40 | static `SceAesContext ref00d_kprx_aes_ctx` | sizeof(SceAesContext) - vitasdk type, size not defined in this repo | kernel | static .bss | module lifetime | n/a |
| src/ref00d_kprx_auth.c:49-83, :85-119 | `const SceKprxAuthKey ref00d_kprx_user_rev0 / ref00d_kprx_kernel_rev0` | 2 x 392 B = 784 B | kernel | .rodata (module image) | module lifetime | n/a |
| src/ref00d_kprx_auth.c:33-47, src/ref00d_rsa_engine.c:16-21, src/main.c:53-59 | scalars/pointers (`ref00d_private_iv[0x10]` :34, ids, flags, 5 `tai_hook_ref_t`) | ~0x70 B total | kernel | static .bss | module lifetime | n/a |
| hook-body stack, caller's kernel stack | `SceSelfAuthInfo self_auth_info` src/ref00d_kprx_auth.c:301 (0x90 per comment src/ref00d_kprx_auth.h:209); `uint32_t kprx_auth_rsa_buffer_e[0x40]` :266 (256 B); `header_hash[0x20]` :272; `rw_iv`/`ref00d_private_header_iv` :228-229 (32 B); `klicensee_dec`/`iv[0x20]` :192-193 (48 B); `SceAesContext ctx` src/ref00d_utils.c:41 (size as above); `iv_enc[0x10]` :55 | ~0x200 B + sizeof(SceAesContext) + callee frames (ksceSha256Digest, ksceAes*) | kernel, on the module-loader thread | that thread's kernel stack | per call | n/a |

- No `ksceKernelAllocMemBlock`, no `sceKernelAllocMemBlock`, no `sce_paf_malloc`, no `malloc` anywhere (grep-verified).
  No static array >= 1 KiB (the largest is the 16-byte IV; `SceAesContext` size unknown here).
- Sum, resident at boot (kernel only): 784 + 4159 + 8192 = 13135 B (~12.8 KiB) of heap + thread stack, plus the module
  image (.text/.rodata incl. 784 B of keys, .bss incl. one SceAesContext) and five kernel objects (thread, mutex,
  2 semaphores, event flag). Nothing charged to SceShell, games, PAF, CDRAM or PHYCONT.
- Peak during use: identical heap footprint (hooks never allocate); plus ~0x200 B + sizeof(SceAesContext) transient
  stack on the loader thread while hook #3 runs, plus 392 B per externally added key. Growth is unbounded only via
  the `ref00d_kprx_add_key` export.

## 6. Firmware assumptions
- No firmware check of any kind: `ksceKernelGetSystemSwVersion`/`SwVersion` not found in source (grep over src/).
  No per-firmware NID or offset tables; no 3.60/3.65/3.68 branches.
- The only firmware-sensitive surface is the fixed import-hook identity: module name "SceKernelModulemgr", library
  NID 0x7ABF5135 and the five function NIDs (src/main.c:166-170), plus the kernel exports resolved through the stub
  libraries at link time (CMakeLists.txt:28-41): SceSysmem (`ksceKernelAllocHeapMemory`), SceThreadmgr (sema/
  mutex/evf/thread), SceKernelUtils (`ksceSha256Digest`, `ksceAesInit1/Encrypt1/Decrypt1`), SceSblSsMgr
  (`ksceSblDmac5AesCtrDec`, src/ref00d_kprx_auth.c:555), SceSblPostSsMgr (`ksceSblRSA2048VerifySignature`,
  src/ref00d_rsa_engine.c:42), SceSblAIMgr (`ksceSblAimgrIs*`, src/ref00d_kprx_auth.c:413-433), ScePmMgr
  (`kscePmMgrGetProductMode`, declared by hand at :24, called :439), SceDebug. On a firmware where a hooked NID does
  not exist the install fails silently (section 3). `SceCpuForDriver_stub` and `SceSysrootForKernel_stub` are
  linked (CMakeLists.txt:28, :36) but no call into them was found in source.
- The "version" the code reasons about is the TARGET module's: `sysver` = control-info type 4 `min_required_fw`
  (src/ref00d_kprx_auth.c:339-341; field at src/ref00d_kprx_auth.h:65) or, if absent, `appinfo->version` (:355-356),
  matched against each key's `minver`/`maxver` (:179). Both built-in keys accept any version: minver 0, maxver
  0xFFFFFFFFFFF (:57-58, :93-94).
- Key coverage at this commit: only two keysets are compiled in, `self_type` REF00D_USER 0x1000D (:79) and
  REF00D_KERNEL 0x10007 (:115), key_type METADATA, key_rev 0 (:77-80, :113-116). Keys for Sony self types
  KERNEL 0x07 / APP 0x08 / USER 0x0D and any NPDRM key are not found in source; `ref00d_kprx_gen_key_file`,
  `ref00d_kprx_set_key`, `ref00d_kprx_gen_keys` are declared (src/ref00d_kprx_auth.h:284-286) but have no definition
  (grep-verified). So, as built from this tree, every ordinary SELF fails the lookup (`return -1`, :361, :374) and is
  handed back to the real SceSblAuthMgr; decrypting other-firmware modules requires another kernel module to call
  `ref00d_kprx_add_key` (exports.yml:25-29). README's "supported 3.60 to 3.73" (README.md:27) is a README claim with
  no corresponding check in source.
- Header format assumptions enforced: magic 0x454353, version 3, platform bit 0x40, 16-byte-aligned ext header
  (:315-316), sig_type 5 / RSA-2048 only (:252-254), header_len <= 0x1000 (:307).

## 7. Configuration
- None. No file I/O (`ksceIo*` not found in source), no config path, no parser. Behaviour is fixed at compile time
  (`REF00D_DEBUG 0`, src/ref00d_types.h:9; `RELEASE 1`, src/main.c:43). The only runtime input is the kernel-only
  export `ref00d_kprx_add_key` (src/ref00d_kprx_auth.c:123-137), callable by other kernel modules via the installed
  `libReAuthMgrForKernel_stub.a` (CMakeLists.txt:61). Nothing to go "missing".

## 8. Logging
- `printf` is `ksceDebugPrintf` only when `REF00D_DEBUG == 1`, otherwise a no-op macro (src/ref00d_types.h:11-14);
  REF00D_DEBUG is 0 (:9), so every `printf(...)` in src/ref00d_kprx_auth.c (e.g. :181, :370-373, :414, :563, :593)
  and the timing blocks (:518-521, :527-530, :542-545, :558-561) compile to nothing. `hex_dump` exists only when
  `RELEASE == 0` (src/main.c:63-84); RELEASE is 1 (:43).
- Always compiled in: `SCE_KERNEL_PRINTF_LEVEL(1, ...)` on the two init failures (src/ref00d_kprx_auth.c:155, :161) -
  boot path, immediately followed by a panic - and `ksceKernelPrintf` when the RSA request/wait fails inside hook #3
  (:278, :284). Kernel debug console only; no file, no ring buffer, no size cap, no rotation.

## 9. Build artifacts
- Target `reF00D` from src/main.c, src/ref00d_utils.c, src/ref00d_kprx_auth.c, src/ref00d_aes_cbc.S (Thumb-2 + NEON,
  src/ref00d_aes_cbc.S:7-9), src/ref00d_rsa_engine.c (CMakeLists.txt:19-25); `enable_language(C ASM)` (:17).
- C flags `-Wl,-q -Wall -O2 -fno-inline` (CMakeLists.txt:14); LINK_FLAGS `-nostdlib` (:45); no `-nostartfiles`, no
  `-Wl,--gc-sections` (not found). COMPILE_FLAGS add `-D__PSP2_KERNEL__` plus per-file `__PSP2FILE__` /
  `__PSP2FILEHASH__` computed by a shell `shasum` at build time (:46) - neither macro is referenced by any source
  file (grep-verified).
- SELF: `vita_create_self(reF00D.skprx reF00D CONFIG exports.yml UNSAFE STRIPPED REL_OPTIMIZE)` (CMakeLists.txt:49-54).
- exports.yml: module `reF00D`, `attributes: 7`, version 1.1, `main: start: module_start` (exports.yml:1-7); three
  libraries, all `syscall: false` (kernel-only, no user-visible syscalls): `ReAuthMgr` {ref00d_auth_open,
  ref00d_auth_close, ref00d_auth_header, ref00d_load_block, ref00d_setup_segment} (:9-17), `ReAuthMgrForDebugger`
  {ref00d_get_internal_header, ref00d_segment_num, ref00d_segment_info} (:18-24), `ReAuthMgrForKernel`
  {ref00d_kprx_add_key} (:25-29).
- Stubs: `vita_create_stubs(stubs reF00D exports.yml KERNEL)` (CMakeLists.txt:56); `make install` copies
  src/ref00d_kprx_auth.h and the three `libReAuthMgr*_stub.a` into $VITASDK (CMakeLists.txt:58-61; build.sh:1-9
  runs cmake + `make install`). On-device location: wherever the user's taiHEN config.txt points (README.md:14).

## 10. Patterns worth copying / anti-patterns
- (+) Augment-don't-replace: the AuthHeader hook tries ref00d first and `TAI_CONTINUE`s only on failure
  (src/main.c:113-116); LoadBlock/LoadSegment are gated on the recorded auth state (src/main.c:125-129, :138-142),
  so every non-matching module takes the original path untouched.
- (+) The caller's header is copied into a private kernel buffer before parsing, with the size bounded first
  (src/ref00d_kprx_auth.c:307-310), and offsets are range-checked against that size (:22, :318-327, :347, :401).
- (+) Heavy crypto (RSA-2048 verify) runs on a dedicated 8 KiB-stack thread with a mutex + binary-semaphore + event-flag
  handshake (src/ref00d_rsa_engine.c:61-108, :132) instead of on the loader's stack; software AES is chosen below a
  measured 0x300-byte threshold because DMAC5 setup costs ~80 us (src/ref00d_kprx_auth.c:550).
- (+) `ref00d_rsa_engine_initialization` unwinds in exact reverse order via labelled gotos (src/ref00d_rsa_engine.c:145-161);
  one copy/paste slip resets the wrong variable (:155).
- (-) Hook UIDs are thrown away (src/main.c:166-170), `HookRelease` is dead code (:37-39), there is no module_stop and
  `attributes: 7` (exports.yml:2): the plugin can never be unloaded, and a failed hook install is invisible.
- (-) Init failures call `SCE_KERNEL_PANIC()` (src/main.c:154, :159, :163) rather than returning
  SCE_KERNEL_START_FAILED - a heap or thread shortage at boot bricks the boot (cf. README.md:25-29).
- (-) Untimed waits inside hooks: `ksceKernelWaitSema(semaid, 1, NULL)` after an unconditional Open (src/main.c:90-92,
  src/ref00d_kprx_auth.c:465) and `ksceKernelWaitEventFlag(..., NULL)` for the RSA result (src/ref00d_rsa_engine.c:94);
  the module loader deadlocks if Open is ever not paired with Close or the RSA thread stops responding.
- (-) Parser robustness on a crafted/corrupt header: the control-info and meta-info walks make no progress check, so an
  entry with `size == 0` and `next == 1` loops forever in kernel mode (src/ref00d_kprx_auth.c:336-353, :390-407);
  `section_num` is never bounded (:288-289, :576); `DecryptSize` can underflow when `m_header_length` is smaller than
  the fixed prefix (:242) and the `-1` from `ref00dAesCbcDecrypt` for a non-multiple-of-16 length is ignored (:244 vs
  src/ref00d_utils.c:44-45) - the assembly loop only stops on exactly zero (src/ref00d_aes_cbc.S:52-53).
- (-) Ignored return values: `ref00d_auth_open` (src/main.c:92), both `ref00d_kprx_add_key` calls
  (src/ref00d_kprx_auth.c:141-142), all four `ref00dAesCbcDecrypt` calls (:200, :203, :235, :244).
- (-) Dead weight shipped in a kernel module: a 3762-line glibc elf.h with zero symbols used (src/elf.h), unused
  `HookExport/HookOffset/GetExport` macros and `module_get_offset/export_func` prototypes (src/main.c:33, :35, :41,
  :46-47), unused `SceNpDrmRsaKey` (src/ref00d_kprx_auth.c:26-29), `setup_ac` (:208-216), `cf_header_v2/v3/cf_header_t`
  (src/self.h:57-101), and three declared-but-undefined key functions (src/ref00d_kprx_auth.h:284-286).
- (-) Struct comments disagree with the ABI layout: `SceKprxAuthKey` is documented as 0x180 with `rsa_n` at 0x40 and
  `sce_type` at 0x140 (src/ref00d_kprx_auth.h:234, :241, :244) but the 8-aligned `uint64_t minver` (:238) makes it
  0x188 / 0x48 / 0x148. Harmless here because allocation uses `sizeof` (src/ref00d_kprx_auth.c:127), but any
  external module building the struct from the comments would hand over a mis-laid-out key.

## 11. Overlay-specific
- Not applicable: kernel-only auth-manager shim; it draws nothing and captures nothing.
