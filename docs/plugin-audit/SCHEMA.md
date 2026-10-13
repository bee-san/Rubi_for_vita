# Per-project audit note schema (binding for every helper)

Write one file per project at `/tmp/vita-plugin-audit/notes/<project>.md`.
Read the SOURCE end to end (every .c/.h/.cpp/.S/CMakeLists.txt/exports.yml/Makefile in scope),
not READMEs or summaries. Every factual claim must carry a `path:line` citation into the clone
under `/tmp/vita-plugin-audit/<project>/` (relative to the project root). Quote at most one line
of source per fact. Do not guess; write "not found in source" when something is absent.
Do not commit anything anywhere; do not touch any git repository. Write only under
`/tmp/vita-plugin-audit/notes/`.

```
# <project> (<owner/repo>, commit <git rev-parse --short HEAD>, <date of commit>)
Files read: <count> files, <total lines> (list the paths)

## 1. Process context
- taiHEN section the plugin is meant for: *KERNEL / *main / *TITLEID / *ALL / user app
  (evidence: README install line, exports.yml / CMake `vita_create_self ... CONFIG`, module
  attributes, use of ksce* vs sce* APIs). If several modules, one line each.
- Which process' memory each module lives in (kernel, SceShell, game, own app).

## 2. Module start / stop
- module_start steps in order (with file:line); what it returns on each failure
  (SCE_KERNEL_START_SUCCESS / FAILED / NO_RESIDENT) and what has been undone before returning.
- module_stop steps in order; is every hook released, every thread joined, every memblock freed,
  every sync object deleted? Ordering (stop threads before releasing hooks? etc.)

## 3. Hooks
Table: | # | target (module, library NID, function NID or import, or offset) | API used
(taiHookFunctionExportForKernel / ImportForKernel / OffsetForKernel / taiHookFunctionImport /
taiInjectData / ...) | installed where (module_start / lazily / in another hook) | released where |
- Partial-failure handling: if hook k fails, what happens to hooks 1..k-1? Is the uid array
  initialised to -1? Does release guard on uid >= 0? Is the result of each install checked?
- Does any hook body allocate, take a mutex, do I/O, or call something that can block?

## 4. Threads
Table: | name | entry | priority | stack bytes | cpu affinity | created where | stopped/joined
where | what it waits on (event flag, delay, semaphore) | period |
- Note any busy-wait, polling period, or work done per frame.

## 5. Allocations (every one)
Table: | site (file:line) | API (ksceKernelAllocMemBlock + type / sceKernelAllocMemBlock + type /
ksceKernelAllocHeapMemory / sce_paf_malloc / malloc / static array / stack) | size (exact number
or formula) | process context in which the call runs (kernel, SceShell, game, own app) | pool it
is charged to (kernel heap, KERNEL_RW, USER_RW of SceShell, USER_RW of the game, PAF heap of
SceShell, CDRAM, PHYCONT, static .bss) | lifetime (freed where) | failure handling |
- Include static .bss/.data arrays >= 1 KiB and every thread stack.
- Sum: resident bytes per process at boot; peak per process during use.

## 6. Firmware assumptions
- Firmware checks (ksceKernelGetSystemSwVersion etc.), NID/offset tables per firmware,
  3.60 vs 3.65 vs 3.68+ handling, anything that would break on an unexpected firmware.

## 7. Configuration
- File path(s), parser, size limits, when read and in which process, what happens if missing.

## 8. Logging
- Mechanism (file / printf / ring / none), process context, whether it runs on the boot path,
  size caps, rotation.

## 9. Build artifacts
- Targets, module names, exports.yml contents (syscall libs?), stub libs, link flags
  (-nostdlib, -nostartfiles, gc-sections), SELF flags, where the artifact is installed.

## 10. Patterns worth copying / anti-patterns
- 3-10 bullets, each with a citation.

## 11. Overlay-specific (only if the plugin draws or captures inside games)
- How it draws (own GPU setup / framebuffer write in display hook / paf / vita2d), where the
  frame or draw memory comes from (which memblock type, which process), when it is allocated
  and freed, how it handles a game that uses all of its memory.
```

End your run by printing exactly one line: `AUDITHELP_DONE /tmp/vita-plugin-audit/notes/<label>-manifest.txt`
where the manifest lists the note files you wrote. If you could not finish, print
`AUDITHELP_BLOCKED <reason>` instead. Write progress to
`/local/home/skerraut/herd-synth3/<your label>.status` (one line, overwritten) at each project.
