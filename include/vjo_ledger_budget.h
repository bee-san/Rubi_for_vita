/* The memory ledger's static budget: every pool our three plugins take memory
 * from, every item they hold there, its worst case in bytes, and the limit
 * each pool's worst case must stay under. tools/check_memory_ledger.py reads
 * this file (plain `#define NAME expression` lines, integer arithmetic only)
 * and fails the build when a worst case exceeds its limit or a packaged
 * module has more pages than declared; the C code uses the same constants,
 * so the table and the binaries cannot drift apart. docs/memory-ledger.md
 * explains the ledger; docs/vita-memory.md is the reference for the pools.
 *
 * Tags: [measured] read on the console or from the packaged binaries (where
 * is in docs/memory-ledger.md), [SDK] the Sony documentation, [expert] the
 * plugin developer's guidance (AGENTS.md), [estimate] neither. */
#ifndef VJO_LEDGER_BUDGET_H
#define VJO_LEDGER_BUDGET_H

/* The module pages differ by build (docs/release-variants.md): `#if VJO_DEBUG` picks the debug
 * build's, the test builds' own, or the normal build's, which compiles the debug-only logging, the
 * self-test and the traces out; tools/check_memory_ledger.py --variant reads the same branch. */
#include "vjo_build.h"

/* Every thread costs its stack plus at least 4 KiB of TLS from its process's
 * budget [SDK: Kernel-Reference, sceKernelCreateThread]. */
#define VJO_LEDGER_TLS 4096u
#define VJO_LEDGER_PAGE 4096u

/* ---- SceShell's partition (ScePhyMemPartShell, 50 MiB; pool "shell pages") ----
 * Module pages are permanent; they may only shrink (AGENTS.md rule 1). The
 * values are the packaged SELF segments rounded to pages [measured, PR #60]. */
/* VitaJPOverlay_Shell.suprx: 102,400 text + 73,728 data/bss. The fixed Kiku note (#90) took 3,488 B
 * off data/bss (the Anki field mapping's 1,152 B per VjoConfig copy); with #86's region selector and
 * #84 (its --gc-sections link; 28 B of new VjoConfig fields) the module is 214,488 text + 77,876
 * data/bss; with #87, #88, #92, #85 and #89 it is 215,216 text + 77,980 data/bss [measured, bcd3cf0],
 * and with the finder refinement 215,256 + 77,984 [measured, fec85af]: 217,088 + 81,920 B of pages;
 * API 21's model sizes (#108) made it 216,448 + 77,624, 294,912 B of pages [measured, 8f1ec8f], and
 * API 22 (#93) 216,856 + 77,648, the same pages [measured, ba5f69a].
 * Lowered from 299,008 B by the removal of jpdb.io and jiten.moe, the online dictionaries: their
 * clients and the subtitle strip's furigana (only their lookups gave readings) took 6,672 B off the
 * text segment (.text -5,544 B, .rodata -1,040 B, the import stubs -48 B, alignment) and 6,444 B off
 * .bss (shell/worker.c -3,340 B: VjoView's strip readings and two VjoConfig copies' API keys;
 * shell/strip.cpp -2,584 B: its copy of the readings and their widgets; shell/anki.c -512 B: a third
 * VjoConfig's keys), so the module is 210,184 text + 71,204 data/bss [measured: the build log's
 * module rows, main ba5f69a and this change], one text page and one data page fewer than main's
 * 294,912 B.
 * Lowered from 286,720 B by the removal of the Hachidori relay client (core/hachidori.c, its
 * dictionary branches, hachidori_host and its validation, the relay's error text, the JSON helper
 * only it used): 3,992 B off the text segment (.text -3,344 B, .rodata -648 B) and 184 B off .bss
 * (the 64 B hachidori_host of three VjoConfig copies and shell/worker.c's 4 B cached dictionary
 * ID, 196 B, less 12 B of alignment), so the module is 206,192 text + 71,020 data/bss [measured:
 * the build log's module rows, main a5d79f2 and this change], one text page fewer: 208,896 +
 * 73,728 B of pages. Lowered again by the removal of AnkiConnect (the Shell's sender and its
 * requests, the screenshot branch, the Kiku note, the audio URL; the Overlay app exports the cards):
 * 18,192 B of text and 972 B of data/bss, so the module is 188,472 text + 70,104 data/bss [measured:
 * the build log's module rows, main 5e68ecd (#116, #111: 206,664 + 71,076, the same 282,624 B of
 * pages) and this change], four text pages fewer: 192,512 + 73,728 B of pages. (On main c4a6ade,
 * before #111's 424 B of text, it was 188,056 B of text, five pages fewer.) #123's key filter makes it
 * 189,104 text, the same pages [measured: the build log's module row, main c641bdf]. #122's
 * removal of the default config.ini template makes it 186,416 + 70,112, one text page fewer:
 * 262,144 B packaged (docs/memory-ledger.md). #127's frequency ranks make it 187,552 + 70,128, the
 * same pages (docs/memory-ledger.md). #124's identification of Adrenaline's PSP games (shell/game.c:
 * AppMgr's app name and one description read per launch; core/games.c's shared PSP row) makes it
 * 188,000 + 70,132, the same 262,144 B [measured: the build log's module rows, main 9f92bc6 and
 * #124].
 * Lowered from 266,240 B by the removal of Google Lens (the overlay is offline): its client and
 * protobuf codec (core/lens.c, core/pb.c), the worker's capture -> JPEG -> Lens job and its
 * connection warm-up, the Shell's capture and JPEG encoder (shell/capture.c, core/jpegsw.c), the
 * worker's network pipeline (two 256 KiB arenas, a connection pool, the whole-sentence lookup only
 * its jobs made) and the engine choice (ocr_backend). Lens was the last of the Shell's TLS
 * connections once AnkiConnect was removed, so the link no longer keeps BearSSL's TLS, the roots
 * or the HTTP client either (only SHA-256, the queue's IDs): 87,208 B off the text segment (.text
 * -66,728 B, .rodata -20,424 B) and 72 B off data/bss, so the module is 100,792 text + 70,060
 * data/bss [measured: the build log's module rows and arm-vita-eabi-size, main 6020c0b and this
 * change], 21 text pages fewer than main's packaged 262,144 B: 102,400 + 73,728 B of pages. #140's
 * posting hints make it 101,024 text, the same pages, and #132's VjoNet stack paint and check
 * 101,488 + 70,076 [measured: the build log's module rows, main 32bfc6a and #132; the same on
 * 3aa08a6, #141]. */
#if VJO_DEBUG
#define VJO_BUDGET_SHELL_MODULE_PAGES 176128u
#else
/* The normal build (docs/release-variants.md): the self-test (shell/selftest.c), the lines of the
 * game worker's step log, of the recognized text and of every job and lookup, and the timings and
 * traces compiled out: 90,768 text + 69,988 data/bss against the debug build's 100,792 + 70,060,
 * 94,208 + 73,728 B of pages, two text pages fewer [measured: the build log's module rows, both builds
 * of this tree on main 3805a13 (#130, #142)]. #140's posting hints make it 91,016 + 69,988, and #132's
 * VjoNet stack paint and check 91,464 + 70,004, the same pages [measured: the build log's module
 * rows, main 3aa08a6 and #132]. Its check is exact (--exact): this must equal its pages. */
#define VJO_BUDGET_SHELL_MODULE_PAGES 167936u
#endif
/* VitaJPOverlay_OCR.suprx: 65,536 text + 8,192 data/bss [measured, API 20 build]. Raised from 45,056 B
 * (36,864 + 8,192): the in-game capture, its memory ladder, probe and step log run in the worker,
 * in the game's own USER_RW (91,226,112 B free measured in CLANNAD, docs/vita-memory.md), never in SceShell.
 * Raised again from 53,248 B (45,056 + 8,192) by one text page for API 20's job policy: the
 * stable-text wait and the blank-line check (core/vocr_ocr.c), and by one more for the text
 * normalisation (core/textnorm.c, -Os), which runs here so that SceShell's pages stay as they are.
 * Raised again from 61,440 B by two text pages for the beam + LM decoder: vita-vn-ocr's
 * runtime/vocr_lm.c (the decoder and the LM scorer) and vocr.c's top-K extras, with their
 * planning and per-line confidence in core/vocr_ocr.c, and the confidence cascade
 * (59,692 B text measured at bcd3cf0, core/vocr_ocr.c built for size); and by one more for the finder
 * refinement (vita-vn-ocr #83's runtime/vocr_lines_refine.c, -O2 as the runtime is built; 65,084 B
 * text measured at fec85af).
 * The game's partition has the room (90,177,536 B free USER the least #81 saw). */
/* Raised again from 73,728 B for API 22 (parallel lines, the line cache, the clock) and the runtime
 * pin, vita-vn-ocr #85's faster W8I kernels: one text page and two data/bss pages for API 22 (text
 * 62,012 -> 67,452 B: the scheduler, the helper threads, the clock, the cache; data/bss 5,840 ->
 * 14,200 B: the 6,160 B VjoVocrCache, eight lines each with its shown and greedy text, and the
 * 2,080 B line queue and slots, both on vita-vn-ocr a540027), and one text page for #85 (the build's
 * vocr.c object 15,288 -> 17,952 B of code; the worker's text 67,452 -> 70,124 B) [measured: main
 * 8f1ec8f against the API 22 build], in the same game partition. */
/* Both builds (docs/release-variants.md): the normal build compiles the self-test's capture probe
 * out, 1,348 B of text, in the same pages since #124 (the PSP bubbles' ID: +260 B): 69,724 + 14,208
 * against the debug build's 71,072 + 14,208 [measured: the build log's module rows, both builds on
 * main 6020c0b; on 9f92bc6 the normal worker was 69,464 B, a text page fewer]. The rest of the
 * worker's debug-only code is the step log, whose tier checks and clock step the normal Shell's
 * short log reads. */
#define VJO_BUDGET_WORKER_MODULE_PAGES 90112u
/* The installer registers the worker per game (*TITLEID), never under *ALL,
 * so taiHEN does not load it into SceShell. Were it loaded there (an older
 * install's *ALL line, a hand edit), its module_start returns
 * SCE_KERNEL_START_NO_RESIDENT outside a candidate game, so the pages are
 * given back at once [SDK: Kernel-Overview, module_start]. */
#define VJO_BUDGET_WORKER_IN_SHELL_PAGES 0u
/* VjoNet: a job's OCR exchange with the game worker and the local dictionary's lookup. Lowered from
 * 65,536 B ("BearSSL needs it for TLS") once Google Lens, the last of its jobs that made a TLS
 * connection, was removed: nothing on the thread reaches BearSSL, the HTTP client or a TCP socket
 * any more (the UDP log's send is the one network call left, PR-B's), and its deepest chain of the
 * Shell's own code is 5,624 B: net_main 144, a job's vocr_run_one 5,000 (its log batch), then
 * vjo_log down to the file write 480 [measured: arm-vita-eabi-gcc -fstack-usage -fcallgraph-info=su
 * of this build, on main 32bfc6a; the SDK's own code and the indirect-call sites are not
 * counted]. The thread paints its
 * unused stack and logs, at each game exit, the most it has used (shell/worker.c net_stack_check),
 * so a console run measures it. */
#define VJO_STACK_SHELL_NET 16384u
#define VJO_STACK_SHELL_CTL 16384u                 /* VjoControl */
#define VJO_STACK_SHELL_ANKI 65536u                /* VjoAnki, while an Anki job runs: with anki_mining on, a count
                                                    * of the queue on every overlay open, a save (Enter) */
#define VJO_BUDGET_SHELL_NET_STACK (VJO_STACK_SHELL_NET + VJO_LEDGER_TLS)
#define VJO_BUDGET_SHELL_CTL_STACK (VJO_STACK_SHELL_CTL + VJO_LEDGER_TLS)
#define VJO_BUDGET_SHELL_ANKI_STACK (VJO_STACK_SHELL_ANKI + VJO_LEDGER_TLS)
/* The Vita build takes result memory from the PAF heap (VJO_PAF_ALLOC); the
 * USER_RW path exists for the host tests only. */
#define VJO_BUDGET_SHELL_USERRW_RESULTS 0u
#define VJO_WORST_SHELL_PAGES_PERMANENT (VJO_BUDGET_SHELL_MODULE_PAGES + VJO_BUDGET_WORKER_IN_SHELL_PAGES + \
                                         VJO_BUDGET_SHELL_NET_STACK + VJO_BUDGET_SHELL_CTL_STACK)
#define VJO_WORST_SHELL_PAGES (VJO_WORST_SHELL_PAGES_PERMANENT + VJO_BUDGET_SHELL_ANKI_STACK + VJO_BUDGET_SHELL_USERRW_RESULTS)
/* A whole Shell plugin has "genuinely under 2 MiB", and SceShell wants that
 * headroom itself [expert]; the limit keeps half of it. The partition had
 * 28,672 and 98,304 B free beside a game [measured], so this is a ceiling
 * on our footprint, not a promise that it fits: only shrinking is safe. */
#define VJO_LIMIT_SHELL_PAGES (2u * 1024u * 1024u / 2u)

/* ---- SceShell's PAF global heap (5,242,880 B, mapped once; pool "shell paf") ----
 * Allocations here take no new pages; they take room firmware's PAF users
 * need. The runtime reserve guard (shell/memory.cpp) refuses anything that
 * would leave less than VJO_PAF_RESERVE free. */
#define VJO_PAF_HEAP 5242880u                      /* [measured, docs/meiki-ocr.md on experiment/meiki-ocr] */
#define VJO_PAF_FREE_MEASURED 2557952u             /* 2,498 KiB free beside CLANNAD, 2026-10-10 [measured, same] */
#define VJO_PAF_RESERVE (1536u * 1024u)            /* shell/memory.cpp RESERVE */
#define VJO_BUDGET_PAF_PLUGIN 8192u                /* the RCO (6,368 B file; 5,632 B before the region cursor) and the Paf Plugin object [estimate] */
/* One result arena, the local dictionary's lookup (worker.c LOCAL_RESULT_ARENA_SIZE): the only result
 * memory since the network paths, Google Lens and the Hachidori relay, were removed (they took
 * 2 x 262,144 B arenas and a connection pool: 571,888 B with two slots, 548,096 B with one). */
#define VJO_BUDGET_PAF_RESULTS_LOCAL 65536u
/* An Anki job (anki.c ANKI_TEXT_MEM): saving a text-only card to the queue. The Shell takes no
 * screenshot: that branch (the kernel's 2,088,960 B full-frame capture, which #74's console refused,
 * and a 393,216 B arena here) was removed with AnkiConnect. A save with 8 dictionaries' entries
 * peaks at 102,680 B [measured, host, test_shell_anki.c, main e53d9fb]. anki.c checks its arena against this at
 * compile time. */
#define VJO_BUDGET_PAF_ANKI 131072u
#define VJO_BUDGET_PAF_UI 20480u                   /* the overlay's text arena while its page is open: 17,952 B needed (overlay.cpp) */
#define VJO_WORST_SHELL_PAF_PERMANENT VJO_BUDGET_PAF_PLUGIN
/* The only configuration (the overlay is offline): one 64 KiB arena, the
 * overlay open, an Anki card being saved. The opt-in network row went with
 * Google Lens and the Hachidori relay (707,840 B with one connection slot). */
#define VJO_WORST_SHELL_PAF (VJO_WORST_SHELL_PAF_PERMANENT + VJO_BUDGET_PAF_RESULTS_LOCAL + VJO_BUDGET_PAF_ANKI + VJO_BUDGET_PAF_UI)
/* What the measured free heap leaves us once the reserve is kept: the
 * reserve is the headroom. Later sessions saw 355-441 KiB and 190 KiB free
 * [measured]; under those every allocation is refused, which the code
 * survives (tested), so this limit is the best case, not a guarantee. */
#define VJO_LIMIT_SHELL_PAF (VJO_PAF_FREE_MEASURED - VJO_PAF_RESERVE)

/* ---- the kernel's partition (ScePhyMemPartKD, 36 MiB shared by every driver; pool "kernel") ---- */
/* VitaJPOverlay_Kernel.skprx: 36,864 text + 77,824 data/bss [measured; API 22's frame-pacing step
 * (kernel/game_ocr.c game_ocr_frame, VJO_GSTEP_PACE) takes 36,336 -> 36,608 B of text and
 * 77,720 -> 77,768 B of data/bss, the same pages: main 8f1ec8f against the API 22 build] */
#if VJO_DEBUG
#define VJO_BUDGET_KERNEL_MODULE_PAGES 114688u
#else
/* The normal build: the self-test's memory routes (kernel/meminfo.c), the frame pacing, and the
 * per-screen, per-job and input traces compiled out: 32,332 + 76,080 against 36,640 + 77,768, one
 * text page fewer [measured: the build log's module rows, both builds]. */
#define VJO_BUDGET_KERNEL_MODULE_PAGES 110592u
#endif
#define VJO_STACK_KERNEL 16384u                    /* VjoWorker */
#define VJO_BUDGET_KERNEL_STACK VJO_STACK_KERNEL
/* The kernel's full-frame capture (vjoRequestCapture), which no Shell code takes any more: Google
 * Lens's JPEG and the Anki screenshot were its users, both removed. At a full frame, KERNEL_RW,
 * per request. Local OCR takes no kernel block since API 19 (#74: on the owner's
 * 3.65 this 2,088,960 B request failed with 0x80024302 at every attempt). */
#define VJO_BUDGET_KERNEL_CAPTURE (960u * 544u * 4u)
/* The OCR ladder's last rung (kernel/buffers.c buffers_kstrip_alloc): one strip of
 * grey rows for one vjoOcrCaptureRows call, only after the display hook could not
 * write into the game's own block; never held together with the JPEG capture
 * (an OCR job and a capture exclude each other), so it adds nothing to the worst case. */
#define VJO_BUDGET_KERNEL_OCR_STRIP (64u * 1024u)
#define VJO_WORST_KERNEL_PERMANENT (VJO_BUDGET_KERNEL_MODULE_PAGES + VJO_BUDGET_KERNEL_STACK)
#define VJO_WORST_KERNEL (VJO_WORST_KERNEL_PERMANENT + VJO_BUDGET_KERNEL_CAPTURE)
/* No cited safe size exists for this pool: its free space on a retail console
 * is unmeasured, and a 2,088,960 B capture has failed there once (#14) and
 * succeeded since. Until the self-test measures the partition, the limit is
 * today's worst case: a ratchet, so nothing grows unnoticed. */
#define VJO_LIMIT_KERNEL VJO_WORST_KERNEL

/* ---- the game's partition (ScePhyMemPartGame, 256 MiB minus the game's own use; pool "game") ---- */
#define VJO_BUDGET_GAME_MODULE_PAGES VJO_BUDGET_WORKER_MODULE_PAGES
#define VJO_STACK_GAME 32768u                      /* VjoGameVocr */
#define VJO_BUDGET_GAME_STACK (VJO_STACK_GAME + VJO_LEDGER_TLS)
#define VJO_BUDGET_GAME_WEIGHTS 1560576u           /* F20, the largest accepted model, page-rounded (core/vocr_ocr.c) */
/* The largest job's workspace at a 960x544 region, page-rounded (host/tests/test_ledger.c
 * computes it): the model's own (vjo_vocr_job_bytes; F20 245,808 B, H15 186,704 B), and with
 * the beam + LM decoder (API 20) its extras (vjo_vocr_lm_job_bytes: the top-K buffers, the
 * decoder's structs and its workspace, bounded by VJO_VOCR_LM_WS_BYTES: 56,928 B on the host
 * build) and the greedy text kept for the log (4,096 B), and for a model smaller than the
 * confidence cascade's the cascade's extras (vjo_vocr_cascade_job_bytes: VOCR4-W20's arena,
 * structs and decoder, 296,192 B), and the finder's boxes refined (vjo_vocr_refine_bytes:
 * 72,400 B at 960x544): 617,312 B with H15. API 22 (parallel lines) ends it with the line slots
 * of the most helpers a job takes (vjo_vocr_helpers_bytes, VJO_VOCR_HELPERS_MAX = 2: the caller's
 * slot, a line's pixels, greedy text and top-K, 68,160 B, and per helper its recognizer arena and
 * context and a slot, 253,104 B for H15 on the host build): 1,191,680 B with H15. The worker takes
 * helpers only when its memory check passes with them, one fewer at a time down to none.
 * API 19 holds no grey copy of the region (it was 770,048 B with one at 960x544). */
#define VJO_BUDGET_GAME_WORKSPACE 1191936u
/* API 22: one thread per line a helper recognizes, created for the line and deleted once it is
 * placed (game_ocr/main.c pool_run): at most VJO_VOCR_HELPERS_MAX at once, none between jobs.
 * Its deepest chain: vocr_run 448 + run_head 1,328 + mm_i16 616 = 2,392 B with the Vita compiler
 * (-fstack-usage, vita-vn-ocr #85's runtime), about a seventh of it. */
#define VJO_STACK_GAME_HELPER 16384u
#define VJO_BUDGET_GAME_HELPER_STACK (VJO_STACK_GAME_HELPER + VJO_LEDGER_TLS)
#define VJO_BUDGET_GAME_HELPERS (2u * VJO_BUDGET_GAME_HELPER_STACK)
/* The capture block: one strip of grey rows and, per region row, its checksum,
 * its recent states and two bits, at most 65,536 B at any region (vjo_vocr_capture_bytes,
 * VJO_VOCR_CAPTURE_LIMIT; 65,480 B at 960x544, 61,388 B at 917x125). Its fallback rungs take 256 KiB of CDRAM or 1 MiB of
 * phycont instead, other partitions, only when USER_RW refused it. */
#define VJO_BUDGET_GAME_CAPTURE 65536u
/* A line taller than a strip, kept whole (core/vocr_ocr.h VJO_VOCR_LINE_MAX_BYTES): line
 * width x line height, so the recognizer gets exactly API 18's input for any line the finder
 * reports (host/tests/test_capture_parity.c; 78,884 B at most on the linefind dev regions).
 * Only when such a line occurs, freed with the job. Sized when the job ceiling was 2,392,064 B:
 * 2,392,064 - 1,560,576 (F20's weights) - 249,856 (the serial workspace) - 65,536 (the capture
 * block) = 516,096 B; a larger band is left out and counted. A tall line of at most
 * VJO_VOCR_STRIP_BYTES may still run on a helper (API 22): it is copied out of this block into
 * the helper's line slot first, so no helper reads it; a larger one runs on the worker's thread. */
#define VJO_BUDGET_GAME_LINE 516096u
/* API 20: the beam + LM decoder's LM, resident like the weights while the game is the
 * active one, USER_RW only: the largest tier, 3,781,072 B (include/vjo_vocr_sizes.h),
 * page-rounded. The worker's memory check takes the largest tier the request allows
 * that fits beside the reserve, a smaller one, or none. */
#define VJO_BUDGET_GAME_LM 3784704u
/* API 20: the confidence cascade's larger model (VOCR4-W20, F20's 1,558,016 B,
 * include/vjo_vocr_sizes.h), resident like the weights, USER_RW only, page-rounded;
 * taken only when the memory check passes with it. API 21: the cascade's model is
 * the next tier up of the job's (Small's is Normal's VOCR3-H15, Normal's is Large's
 * VOCR4-W20, Large has none), so this is still its largest; a job's own weights
 * and its cascade's are at most Normal's and Large's, under the two budgets. */
#define VJO_BUDGET_GAME_CASCADE 1560576u
/* The free USER memory a job leaves to the game beside what it holds, by the
 * largest model tier the job holds (include/vjo_vocr_sizes.h; the job's own model
 * or the cascade's): vita-vn-ocr docs/model-tiers.md §1.3 and §1.5 proposes
 * 4 MiB for Small, 8 MiB for Normal and 16 MiB for Large, because a larger
 * resident block held while the game is in front is a larger worst-case bite out
 * of the game's own later allocations (an estimate, untuned; tools/model_tiers.json
 * "reserve_bytes"). The reserves are what the job leaves free, not memory it
 * takes: a larger one only makes a tier fall back sooner. A request by model name
 * (host tools; the Shell asks by tier since API 21) keeps the flat 4 MiB. */
#define VJO_GAME_RESERVE_SMALL (4u * 1024u * 1024u)
#define VJO_GAME_RESERVE_NORMAL (8u * 1024u * 1024u)
#define VJO_GAME_RESERVE_LARGE (16u * 1024u * 1024u)
#define VJO_GAME_RESERVE VJO_GAME_RESERVE_SMALL    /* kept free in the game before either block (game_ocr/main.c) */
#define VJO_WORST_GAME_PERMANENT (VJO_BUDGET_GAME_MODULE_PAGES + VJO_BUDGET_GAME_STACK)
#define VJO_WORST_GAME_JOB (VJO_BUDGET_GAME_WEIGHTS + VJO_BUDGET_GAME_WORKSPACE + VJO_BUDGET_GAME_CAPTURE + \
                            VJO_BUDGET_GAME_LINE + VJO_BUDGET_GAME_LM + VJO_BUDGET_GAME_CASCADE + \
                            VJO_BUDGET_GAME_HELPERS)
/* The recognizer ceiling tier: weights <= 1,601,536 B and a full-screen
 * workspace <= 790,528 B [vita-vn-ocr docs/vita-memory-budget.md; since API 22 the
 * workspace limit is VJO_BUDGET_GAME_WORKSPACE by construction], and (API 20)
 * the 4 MB LM and the cascade's larger model: a job's peak 8,105,984 B, which the
 * preflight (free USER minus the job's reserve, 4-16 MiB by tier) has to cover. Raised from
 * 2,392,064 B for the LM and the cascade: 9 % of the 90,177,536 B free USER, the
 * least any job saw in CLANNAD [measured, #81's v0.8.0-test.1 log]; a game with
 * less runs a smaller LM, no cascade, or neither (the memory check), then a smaller tier.
 * Raised again for API 22's parallel lines (the helpers' line slots in the workspace,
 * 573,440 B more at 960x544 with H15, and two 20,480 B thread stacks): a job's peak
 * 8,720,384 B, 9.7 % of that free USER; a game with less runs fewer helpers, or none,
 * before anything else gives way, and never a smaller tier for them (game_ocr/main.c). */
#define VJO_LIMIT_GAME_WEIGHTS 1601536u
#define VJO_LIMIT_GAME_WORKSPACE 1191936u
#define VJO_LIMIT_GAME_JOB 8720384u

#endif
