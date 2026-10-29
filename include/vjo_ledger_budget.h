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

/* Standalone Rubi app, its own 50 MiB partition, never SceShell's plugin.
 * Four 1024-square, one-byte PVF atlases: drawing, measuring, and at most
 * one replacement of each. [source: settings/font_raster.c and main.c]
 * tools/check_settings_self.py includes this worst case in the app budget. */
#define VJO_APP_FONT_ATLAS_EDGE 1024u
#define VJO_BUDGET_APP_FONT_ATLASES (4u * VJO_APP_FONT_ATLAS_EDGE * VJO_APP_FONT_ATLAS_EDGE)

/* Standalone settings app only: private scratch for a slow appearance save. Its .bss is included
 * in the SELF segments, and check_settings_self.py counts its third 64 KiB worker stack. */
#define VJO_APP_APPEARANCE_SCRATCH (64u * 1024u)

/* ---- SceShell's partition (ScePhyMemPartShell, 50 MiB; pool "shell pages") ----
 * Module pages are permanent; they may only shrink (AGENTS.md rule 1). The
 * values are the packaged SELF segments rounded to pages [measured, PR #60]. */
/* VitaJPOverlay_Shell.suprx. The normal build (docs/release-variants.md) compiles out the self-test
 * (shell/selftest.c), the lines of the game worker's step log, of the recognized text and of every job and lookup,
 * and the timings and traces. Both builds' packaged segments, each rounded to pages: debug 98,148 + 69,632 B ->
 * 167,936 B (156 B of code room, 0 B of data room), normal 85,980 + 69,556 B -> 155,648 B (36 B, 76 B) [measured:
 * tools/check_memory_ledger.py's module rows, tools/fast_gate.sh's vitasdk image, main f3719fb2 (#537); 97,996 + 69,184
 * and 85,724 + 69,116 B at API 57's rear-tap scan]. The normal build's check is exact (--exact): its budget must equal
 * its pages. How each change moved them: its own
 * section of docs/memory-ledger-history.md, and before #418 that file's "Budget history". */
#if VJO_DEBUG
#define VJO_BUDGET_SHELL_MODULE_PAGES 167936u
#else
#define VJO_BUDGET_SHELL_MODULE_PAGES 155648u
#endif
/* VitaJPOverlay_OCR.suprx, in the game's partition, in every game that is ON (never SceShell's: AGENTS.md rules 1-2).
 * The normal build compiles out the self-test's capture probe and hooks.log's writer (game_ocr/hooks.c), and the
 * debug build has the read-only PSP probe (docs/native-hooks.md). Both builds' packaged segments, each rounded to
 * pages: debug 180,672 + 32,424 B -> 217,088 B (3,648 B of code room, 344 B of data room), normal 171,740 + 32,408 B
 * -> 204,800 B (292 B, 360 B) [measured: tools/check_memory_ledger.py's module rows, both builds, psandvita #1's
 * audit's follow-up on main 3f864a3a, tools/fast_gate.sh's vitasdk image]. The normal build's check is exact
 * (--exact): its budget must equal its pages.
 * How each change moved them: its own section of docs/memory-ledger-history.md, and before #418 that file's "Budget history".
 * Before current-game disk profiles, each verified hook row took 40 B of the data room (core/hook_place.c's verified[], VJO_DATA_TABLE): once it is
 * gone the next row takes a page in both builds, unless a table moves to read-only data first or the budget is raised
 * (audit-rubi-merge-394 N5). */
/* Current-game disk profiles remove the full catalogue/program data from the worker.
 * Measured with the pinned VitaSDK: debug 168,264 + 28,840 -> 204,800 B;
 * normal 159,156 + 28,824 -> 192,512 B, 12,288 B fewer pages in each build.
 * The current title's bounded profile is included in these module pages.
 * #528 (the game's whole-MiB free memory, mem-game): debug 168,808 + 29,544 B, normal 159,636 + 29,528 B, the same
 * pages (normal: 108 B of code room left; the ledger table, the resident descriptors and the blocks' and threads'
 * names moved to the data page) [measured: tools/check_memory_ledger.py, both builds, the gate's VitaSDK image, on
 * main e8b6abe3]. The Deltarune capture reader keeps these page budgets by
 * placing the existing 480 B model-descriptor table beside the LM descriptors
 * in the spare data-page space: normal 159,540 B code + 31,016 B data/BSS,
 * 192,512 B page-rounded [measured: local pinned gate VitaSDK build, 2026-10-29]. */
#if VJO_DEBUG
#define VJO_BUDGET_WORKER_MODULE_PAGES 204800u
#else
#define VJO_BUDGET_WORKER_MODULE_PAGES 192512u
#endif
/* The kernel loads the worker only into games ON in games.ini (#138, kernel/loader.c:
 * never SceShell, a system app, VitaShell or the Rubi app); a *TITLEID line an
 * installer writes for it (until the one-VPK install removes them) loads it into that game
 * only, and none writes *ALL, so taiHEN does not load it into SceShell. Were it loaded
 * there (an older install's *ALL line, a hand edit), its module_start returns
 * SCE_KERNEL_START_NO_RESIDENT outside a candidate game, so the pages are
 * given back at once [SDK: Kernel-Overview, module_start]. */
#define VJO_BUDGET_WORKER_IN_SHELL_PAGES 0u
/* VjoNet, the Shell's job thread: a job's OCR exchange with the game worker and the local dictionary's lookup. Its
 * deepest chain of the Shell's own code is 5,616 B: net_main 144, a job's vocr_run_one 5,000 (its log batch), then
 * vjo_log down to the file write 472 [measured: arm-vita-eabi-gcc -fstack-usage -fcallgraph-info=su, main 3aa08a6
 * with #132 and #145; the SDK's own code and the indirect-call sites are not counted]; the normal build's 4,720 B:
 * net_main 2,144 (the 2 KiB log buffer that holds a job's lines until its result is taken, shell/worker.c net_main),
 * then a lookup down to the dictionary's page cache [measured the same way, on main eece6c6 with t3-log-path]. The
 * thread paints its unused stack and logs, at each game exit, the most it has used (shell/worker.c net_stack_check),
 * so a console run measures it. The name is from when the thread also made network requests (docs/memory-ledger-history.md,
 * "Budget history"); the logs' "VjoNet stack" and tools/analyse_selftest.py read it. */
#define VJO_STACK_SHELL_NET 16384u
#define VJO_STACK_SHELL_CTL 16384u                 /* VjoControl */
/* VjoAnki, while an Anki job runs: with anki_mining on, a count of the queue on every overlay open, a save (Enter).
 * Its deepest chain of the Shell's own code is 1,728 B, a save's queue ID (job_main 24, do_save 672, vjo_queue_save
 * 416, vjo_queue_id 160, SHA-256 456), the file callbacks of its indirect calls included [measured: arm-vita-eabi-gcc
 * -fstack-usage -fcallgraph-info=su, #145's build; the SDK's own code is not counted]. 16 KiB is VjoControl's stack,
 * which runs the same kinds of SDK calls (sceIo, the log, Paf's heap) on every console. */
#define VJO_STACK_SHELL_ANKI 16384u
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
#define VJO_PAF_RESERVE (1536u * 1024u)            /* shell/paf_guard.h vjo_paf_guard_reserve */
#define VJO_BUDGET_PAF_PLUGIN 8192u                /* the RCO (6,368 B file; 5,632 B before the region cursor) and the Paf Plugin object [estimate] */
/* One result arena, the local dictionary's lookup (worker.c LOCAL_RESULT_ARENA_SIZE): the Shell's only result
 * memory. */
/* Includes the dictionary session, filter directory and bounded word locators
 * (at most 7 KiB, compile-time checked), ahead of the resettable lookup arena. */
#define VJO_BUDGET_PAF_RESULTS_LOCAL 65536u
/* An Anki job (anki.c ANKI_TEXT_MEM): saving a card to the queue. The Shell takes no screenshot itself: the game
 * worker makes the card's JPEG in the game's memory (VJO_BUDGET_GAME_SHOT); here only its request and result, 5,440 B
 * in this arena while the shot runs, after the record's dry runs are released. #528: sized by construction from its
 * inputs' bounds (anki.c ANKI_FULLEST, checked at compile time: the settings, the locators, a record's worth of card,
 * one more dictionary's entry at VJO_LOCAL_MAX_ENTRY and the record being encoded, VJO_QUEUE_TEXT_MAX: 87,013 B on the
 * host, 86,869 B on the Vita), so a card is the same as in a larger arena; 131,072 B until then, whose measured peak,
 * 102,624 B with 8 dictionaries' entries, was its record's buffer doubling into room it had [measured, host,
 * test_shell_anki.c]. */
#define VJO_BUDGET_PAF_ANKI 90112u
#define VJO_HEADER_RANGE_BYTES 2048u              /* 512 entry starts and lengths, uint16_t each, in the open page's UI block */
/* The overlay's text block: text/spans 18,016 B + ranges 2,048 B (source bounds, test_ledger.c). #528: taken once as
 * the Shell starts (shell/overlay_mem.h vjo_overlay_mem_boot, through the reserve guard: 3,725,976-3,744,264 B free at
 * all 90 logged starts [measured, #528's logs]) and kept, so the overlay asks for no memory of ours when it opens: a
 * permanent item. Taken per open before, it was refused 34 times beside another plugin's buffer (free 202,296-426,872 B). */
#define VJO_BUDGET_PAF_UI 20480u
/* The overlay page itself: Paf's own blocks for PageOpen's widgets while it is open, outside our ledger and not ours to
 * size: a median 94,800 B and at most 103,296 B in the 81 opens of #528's logs where Rubi allocated again before the
 * page closed (74 of them 92,368-103,296 B; of the 7 smaller ones, 16-88,624 B, three allocated before PageOpen and
 * two had a read give the result arena back between the samples; audit-mem-lead-1's pairing of the same opens:
 * 81,152-103,296 B, median 94,928 B) [measured, log; reports/mem findings Mmem-paf-3]. With the text block held from the start (#528) the reserve
 * guard no longer runs before PageOpen, so shell/paf_guard.h vjo_paf_page_fits opens the page only with the most of
 * those free and VJO_PAF_PAGE_MARGIN beside them for SceShell: Paf's use besides ours moved the free figure by at
 * most 17,712 B between two of our allocations within one open (12 pairs, the page's own layout between them
 * included), and SceShell's own, with Rubi at its 8 KiB and no page open, by a median 20,008 B over a game session
 * (286 sessions, #528's logs) [measured, log]; 64 KiB is about 3x either, and the page still opens at every
 * press the logs hold (the least, 202,296 B free with 0.10.1's 8,192 B held, leaves 181,816 B with the block) [the
 * owner scenario]. A guess at what SceShell needs at the very bottom, not a measurement of it [estimate]. */
#define VJO_PAF_PAGE_MAX 103296u
#define VJO_PAF_PAGE_MARGIN 65536u
/* The page's blocks, counted in the pool's worst case although Paf takes them, not our wrapper (the run-time ledger
 * cannot see them): they are held because our page is open, beside everything else an open page can hold. The
 * subtitle strip's page is never open with the overlay's (shell/overlay.cpp frame: the strip closes first); its own
 * blocks are unmeasured, assumed no larger (one text, its box and a busy indicator) [estimate]; the debug build samples
 * them at every strip open (shell/strip.cpp strip_page_open) so a console run measures them. */
#define VJO_BUDGET_PAF_PAGE VJO_PAF_PAGE_MAX
/* Line history (shell/worker.c history_open, docs/line-history.md): one visit's block, from the overlay's
 * first Up past its text's top row until the return to the current text (or the overlay's close): the
 * table (core/history_lines.h VjoHistory, 34 line pointers and two counts) and the worker's three words
 * beside it, the current text (at most 4,095 B and its NUL), and as many of this game's earlier lines as
 * fit, at most VJO_HISTORY_LINES (32). Typical dialogue lines (20-60 characters of 3 B) are 60-180 B,
 * so 32 take 2-6 KiB [estimate]. history.csv's last 24,512 B (and the byte before them) are read into the
 * control thread's static 24 KiB scratch, not here. Held beside an open overlay, its lookup and an Anki
 * save (a card from an earlier line). */
#define VJO_BUDGET_PAF_HISTORY 16384u
#define VJO_WORST_SHELL_PAF_PERMANENT (VJO_BUDGET_PAF_PLUGIN + VJO_BUDGET_PAF_UI)
/* The worst case: the 64 KiB result arena, the overlay open on an earlier line (its page's own blocks too), and an
 * Anki card being saved (the overlay's text block is in the permanent part since #528). */
#define VJO_WORST_SHELL_PAF (VJO_WORST_SHELL_PAF_PERMANENT + VJO_BUDGET_PAF_RESULTS_LOCAL + VJO_BUDGET_PAF_ANKI + \
                             VJO_BUDGET_PAF_HISTORY + VJO_BUDGET_PAF_PAGE)
/* What the measured free heap leaves us once the reserve is kept: the
 * reserve is the headroom. Later sessions saw 355-441 KiB and 190 KiB free
 * [measured]; under those every allocation is refused, which the code
 * survives (tested), so this limit is the best case, not a guarantee. */
#define VJO_LIMIT_SHELL_PAF (VJO_PAF_FREE_MEASURED - VJO_PAF_RESERVE)

/* ---- the kernel's partition (ScePhyMemPartKD, 36 MiB shared by every driver; pool "kernel") ---- */
/* VitaJPOverlay_Kernel.skprx. The normal build compiles out the self-test's memory routes (kernel/meminfo.c), the
 * frame pacing and the per-screen, per-job and input traces. Both builds' packaged segments, each rounded to pages:
 * debug 43,752 + 72,404 B -> 118,784 B (1,304 B of code room, 1,324 B of data room), normal 38,560 + 70,716 B ->
 * 114,688 B (2,400 B, 3,012 B) [measured: tools/check_memory_ledger.py's module rows, both builds, main 6b028b2b,
 * tools/fast_gate.sh's vitasdk image]. The normal build's check is exact (--exact): its budget must equal its pages.
 * How each change moved them: its own section of docs/memory-ledger-history.md, and before #418 that file's "Budget
 * history". */
#if VJO_DEBUG
#define VJO_BUDGET_KERNEL_MODULE_PAGES 118784u
#else
#define VJO_BUDGET_KERNEL_MODULE_PAGES 114688u
#endif
#define VJO_STACK_KERNEL 16384u                    /* VjoWorker */
#define VJO_BUDGET_KERNEL_STACK VJO_STACK_KERNEL
/* Deliberately add one 16 KiB kernel stack so slow disk I/O cannot stall
 * input polling. No new Shell memory, image buffer or heap. */
#define VJO_STACK_KERNEL_IO 16384u
#define VJO_BUDGET_KERNEL_IO_STACK VJO_STACK_KERNEL_IO
/* The capture ladder's last rung (kernel/buffers.c buffers_kstrip_alloc): one strip of a job's
 * rows for one vjoOcrCaptureRows call, only after the display hook could not write into the
 * game's own block; freed before the call returns, or, for a copy the call abandoned, by the
 * kernel worker once the copy ends (kernel/game_ocr.c). Counted in the worst case. */
#define VJO_BUDGET_KERNEL_OCR_STRIP (64u * 1024u)
/* #138: a starting game's games.ini read (kernel/loader.c policy_alloc, KERNEL_RW,
 * "VjoPolicy"): the parsed table (sizeof(VjoGames), 8,200 B) and the file's text
 * (VJO_GAMES_TEXT_MAX, 12,288 B), page-rounded. The kernel worker takes it for one
 * read of a game the Shell may enable and frees it before the game's start goes
 * on; loader.c checks at compile time that both fit. It is not refused while a
 * kernel strip is held, so the worst case counts both. */
#define VJO_BUDGET_KERNEL_POLICY 24576u
#define VJO_WORST_KERNEL_PERMANENT (VJO_BUDGET_KERNEL_MODULE_PAGES + VJO_BUDGET_KERNEL_STACK + VJO_BUDGET_KERNEL_IO_STACK)
#define VJO_WORST_KERNEL (VJO_WORST_KERNEL_PERMANENT + VJO_BUDGET_KERNEL_OCR_STRIP + VJO_BUDGET_KERNEL_POLICY)
/* No cited safe size exists for this pool: its free space on a retail console
 * is unmeasured, and a 2,088,960 B capture failed there (#14, #74) and
 * succeeded in other sessions before API 24 removed it. Until the self-test
 * measures the partition, the limit is today's worst case: a ratchet, so
 * nothing grows unnoticed. */
#define VJO_LIMIT_KERNEL VJO_WORST_KERNEL

/* ---- the game's partition (ScePhyMemPartGame, 256 MiB minus the game's own use; pool "game") ---- */
#define VJO_BUDGET_GAME_MODULE_PAGES VJO_BUDGET_WORKER_MODULE_PAGES
#define VJO_STACK_GAME 32768u                      /* VjoGameVocr */
#define VJO_BUDGET_GAME_STACK (VJO_STACK_GAME + VJO_LEDGER_TLS)
/* The largest accepted model, page-rounded: the Large tier's (include/vjo_vocr_sizes.h VJO_VOCR_MAX_BYTES; game_ocr/main.c
 * refuses a larger weights block). VOCR6-L's 2,475,680 B since VOCR6 (vita-vn-ocr #125, v0.4.0-vocr6.1 and the follow-up
 * set v0.4.1-vocr6.2, the same size: the plain blob's 2,473,696 B with its head rows in frequency order and its
 * 1,984 B two-level trailer, no gate section), in the same 605 pages as VOCR5-L's 2,477,472 B (API 42: v0.3.0's
 * 2,473,696 B with the gate's 1,792 B as well), so no change. Raised from 1,560,576 B, F20's shape, the largest before
 * VOCR5, then from 2,473,984 B (v0.3.0's VOCR5-L). */
#define VJO_BUDGET_GAME_WEIGHTS 2478080u
/* The largest job's workspace at a 960x544 region, page-rounded (host/tests/test_ledger.c
 * computes it): the model's own (vjo_vocr_job_bytes; VOCR6-L 267,456 B, VOCR6-N 189,424 B and VOCR6-S 129,808 B on the
 * host build at vita-vn-ocr f8a1856, since v0.4.1-vocr6.2, whose VOCR6-N has a head gate again (+48 B: v0.4.0-vocr6.1's
 * had 189,376 B); VOCR5-L 267,440 B, VOCR5-N 189,360 B and H15 187,856 B at API 42's pin), and with the
 * beam + LM decoder (API 20) its extras (vjo_vocr_lm_job_bytes:
 * the top-K buffers, the decoder's structs and its workspace, bounded by VJO_VOCR_LM_WS_BYTES:
 * 56,928 B on the host build) and the greedy text kept for the log (4,096 B), and with the
 * confidence cascade the cascade's extras (vjo_vocr_cascade_job_bytes: the larger model's arena,
 * structs and decoder; by tiers the next tier up's, VOCR6-L's 317,840 B beside VOCR6-N; by name
 * VOCR4-W20's 296,352 B beside a smaller model), and the finder's boxes refined
 * (vjo_vocr_refine_bytes: 72,400 B at 960x544).
 * API 22 (parallel lines) ends it with the line slots of the most helpers a job takes
 * (vjo_vocr_helpers_bytes, VJO_VOCR_HELPERS_MAX = 2: the caller's slot, a line's pixels, greedy
 * text and top-K, 68,160 B, and per helper its recognizer arena and context and a slot, 254,672 B
 * for VOCR6-N on the host build): 1,218,192 B for VOCR6-N with VOCR6-L's cascade, the largest, in the same pages
 * (v0.4.1-vocr6.2's VOCR6-N has its gate's 48 B in its own context and its two helpers', VOCR6-L none: v0.4.0-vocr6.1's
 * pair, neither gated, took 1,218,048 B; 1,218,240 B for VOCR5-N with VOCR5-L's, both gated, at the same runtime pin;
 * 1,218,048 B before vita-vn-ocr f8a1856's structs, API 50: on the host its two models and four contexts are 32 B
 * larger each)
 * (1,211,648 B on main, 1,191,680 B with H15 and VOCR4-W20 before VOCR5). API 42 adds 6,400 B: vita-vn-ocr's
 * head gate (48 B in each gated blob's arena) and two-level head (1,456 B more in each two-level blob's) in each of
 * the four contexts such a job plans (its own, the cascade's and the two helpers'), and their fields in the
 * runtime's model and context: two pages more. Kept: an estimated 0.20 / 0.26 s less per 3-line box (Normal /
 * Large; docs/vita-vn-ocr.md "Head gate and two-level head"). The worker takes
 * helpers only when its memory check passes with them, one fewer at a time down to none.
 * API 19 holds no grey copy of the region (it was 770,048 B with one at 960x544).
 * API 54, the text spotter as the finder (ocr_finder = spotter, vita-vn-ocr #128): its arena and result take the
 * job's scratch, which the recognizer's arena reuses once the boxes are taken (core/vocr_ocr.c layout_spot, until #528), so the
 * scratch is the largest of the three: at most VJO_VOCR_SPOT_ARENA_MAX (262,144 B, the protocol's eligibility for a
 * 960 x 544 frame, include/vjo_vocr_sizes.h) and the 1,684 B result (VocrTextspot6Set), 263,840 B 16-byte aligned,
 * against the Normal tier's job of 189,424 B (VOCR6-N's arena and structs since v0.4.1-vocr6.2; 189,376 B before):
 * 79,392 B more for that largest job, 1,297,584 B (1,297,488 B with v0.4.0-vocr6.1's VOCR6-N: its helpers' two
 * contexts are the gate's 48 B larger each now; 848 B of the budget's last page left), and the Large tier's 1,360 B
 * more (host/tests/test_ledger.c computes both). Its
 * candidates' own arenas at a full 960 x 544 region, read halved: 244,607 B (t64n8c16) and 256,247 B (qS4, qM4, qL5) [vita-vn-ocr
 * eval/textspot6_a9_20261026.json, the runtime's vocr_textspot6_arena_bytes_flags; a 900 x 200 dialogue box:
 * 235,119 B and 168,239 B]. Only a job with the spotter takes it (a GRAD job's workspace is as before), and only in
 * the room its check leaves beside the tier, LM and cascade: 1,220,608 -> 1,298,432 B.
 * #528 (mem-game): the runtime's structs lead the workspace and the spotter's part runs from the scratch's start over
 * every part after it (the refinement's arena, the LM's and the cascade's extras, the helpers' line slots), which the
 * job writes only after the spotter's boxes are copied out (core/vocr_ocr.c run_job), so it grows a job only by what
 * it passes the job and those extras by: the largest job is VOCR6-N's with VOCR6-L's cascade again, the spotter's part
 * within it: 1,215,840 B with the Vita's 32-bit structs (the gate's ARM lane; the runtime's model and context are
 * 1,872 and 2,048 B there [arm-vita-eabi-gcc], 2,912 and 2,064 B on a 64-bit host, whose figure is 1,218,192 B, a page
 * more), so the budget is 297 pages, 1,216,512 B (exact on 32-bit builds: host/tests/test_ledger.c). A job's own
 * workspace with the spotter: Small's 114,688 B and Normal's 57,344 B less at a textbox with the LM (host build). */
#define VJO_BUDGET_GAME_WORKSPACE 1216512u
/* API 22: one thread per line a helper recognizes, created for the line and deleted once it is
 * placed (game_ocr/main.c pool_run): at most VJO_VOCR_HELPERS_MAX at once, none between jobs.
 * Its deepest chain: vocr_run 600 + run_head 1,320 + mm_i16 616 = 2,536 B with the Vita compiler
 * (-fstack-usage, the build's flags, vita-vn-ocr f8a1856's runtime, since API 50; 592 + 1,320 + 616 = 2,528 B with
 * 47cf3a3's from API 42, 448 + 1,328 + 616 = 2,392 B with 817d203's before it; the two-level path's vocr_run 600 +
 * run_head2 1,240 + mm_i16 616 = 2,456 B), about a sixth of it. */
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
 * the helper's line slot first, so no helper reads it; a larger one runs on the worker's thread. API 53 (#379): a job
 * of columns holds the rectangle around its columns in it (core/vocr_ocr.h VJO_VOCR_COLUMNS_BATCH, this bound), each
 * column turned out of it into the rotation buffer as it is read. */
#define VJO_BUDGET_GAME_LINE 516096u
/* API 55, the pause for the frame (config.ini ocr_pause, off by default; docs/game-pause.md): a job that asks for it
 * copies the region from one frame, in one display call, into this block ("VjoGameVocrFrame": region width x height
 * grey bytes, at most 960 x 544 = 522,240 B, page-rounded), and reads every row from it, a line taller than a strip
 * included (core/vocr_ocr.h VjoGreySource.frozen), so it takes no VJO_BUDGET_GAME_LINE block. When the kernel refuses
 * the copy or cannot finish it, the worker frees the block before the job reads on, so a job holds this block or a
 * tall line's, never both: the worst case counts the larger (game_ocr/main.c checks both at compile time). Taken only
 * when it fits beside everything the job's memory check chose, with that check's reserve (never at the cost of a model
 * tier, the LM or the cascade, nor of another model's resident weights; it may cost a helper), on the same ladder
 * (USER_RW, CDRAM, phycont), freed with the job. */
#define VJO_BUDGET_GAME_FRAME 524288u
#define VJO_BUDGET_GAME_LINE_OR_FRAME VJO_BUDGET_GAME_FRAME
/* API 20: the beam + LM decoder's LM, resident like the weights while the game is the
 * active one, USER_RW only: the largest tier, 3,603,552 B (include/vjo_vocr_sizes.h; VOCR6's
 * 4 MB LM, vita-vn-ocr #127; ctclm.2's was 3,781,072 B), page-rounded. The worker's memory
 * check takes the largest tier the request allows that fits beside the reserve, a smaller
 * one, or none. */
#define VJO_BUDGET_GAME_LM 3604480u
/* API 20: the confidence cascade's larger model, resident like the weights, USER_RW only,
 * page-rounded; taken only when the memory check passes with it. API 21: the cascade's model
 * is the next tier up of the job's (Small's is Normal's, Normal's is Large's, Large has none;
 * a request by name's, the host tools', VOCR4-W20), so its largest is the Large tier's
 * (VJO_VOCR_MAX_BYTES, VOCR6-L's 2,475,680 B since VOCR6, VOCR5-L's 2,477,472 B since API 42, the same pages;
 * game_ocr/main.c refuses a larger cascade block). Raised from 1,560,576 B, VOCR4-W20's, Large before VOCR5, then
 * from 2,473,984 B (v0.3.0's VOCR5-L).
 * A job's own weights and its cascade's are at most Normal's and Large's: the two budgets, each the
 * largest model's, count 1,286,144 B more than any job holds (v0.4.1-vocr6.2's VOCR6-N, 1,191,936 B of weights block
 * with its gate section, beside VOCR6-L's cascade; 1,290,240 B with v0.4.0-vocr6.1's 1,187,840 B, as with VOCR5-N's
 * 1,191,936 B), an over-count kept so that each item's worst case stays its own. */
#define VJO_BUDGET_GAME_CASCADE 2478080u
/* Free USER memory every game job leaves beside its page-rounded allocations: a fixed 1 MiB for every
 * model size, its cascade's included, and for a request by model name (owner, 2026-10-23, PR #280; #277's console
 * test build carried it, and the owner reports that it works, 2026-10-28). あおかな (PCSG00700) reported 3 MiB free,
 * yet Small was refused: its 804 KiB model and 192 KiB workspace came beside the old 4 MiB reserve. The former
 * margins, 4 MiB for Small, 8 MiB for Normal and 16 MiB for Large by the largest model tier the job holds
 * (vita-vn-ocr docs/model-tiers.md §1.3 and §1.5), were an untuned proposal; 1 MiB is an estimate too (no figures
 * of that run were reported). It is headroom left free, never allocated: model, LM,
 * cascade, spotter and helper memory is counted separately beside it; the spotter's block keeps it when it loads,
 * before the check, and the blocks a job takes after its check keep it too (game_ocr/main.c job_reserve: the weights,
 * the LM, the cascade's model, a tall line over one column's bound), and an Anki screenshot's check keeps it
 * (VJO_GAME_RESERVE). The
 * three tier names stay for the shared model table (tools/model_tiers.json "reserve_bytes", core/vocr_models.c). */
#define VJO_GAME_RESERVE_SMALL (1u * 1024u * 1024u)
#define VJO_GAME_RESERVE_NORMAL (1u * 1024u * 1024u)
#define VJO_GAME_RESERVE_LARGE (1u * 1024u * 1024u)
#define VJO_GAME_RESERVE VJO_GAME_RESERVE_SMALL    /* kept free in the game before either block (game_ocr/main.c) */
/* The capture pass of a job a Rubi verified table site answers after OCR was refused for memory
 * (game_ocr/main.c, core/hook_select.h vjo_hook_select_no_memory): one strip's block (at most
 * VJO_VOCR_STRIP_BYTES, 56 KiB), freed with the job, beside this reserve, the same 1 MiB as OCR's since PR #280
 * (smaller than OCR's 4 MiB before it). A game with too little room for any OCR job, or whose last model's weights
 * are refused after its check passed (#280's audit F1: アマツツミ, PCSG01198, 2 MiB measured on a console, 2026-10-23,
 * passes Small's check at 1 MiB, and on a console that reports free memory in whole MiB its weights can then be
 * refused, while less than 2 MiB and the job's blocks is really free), can still show its hook's text when this
 * capture-only pass fits.
 * It adds nothing to
 * VJO_WORST_GAME_JOB: it is the capture block every job may hold. API 43: a trusted hook's answer
 * (VJO_GAME_OCR_S_TRUST, core/hook_select.h vjo_hook_select_trusted) takes the same one strip beside
 * the same reserve, and after 8 such answers in a row gives back the resident weights, LM and cascade
 * model: no new block, and the job's own worst case is the capture block's. API 49: it takes no strip at
 * all while the region shows the trusted line's scene (VJO_ROWS_HOOK publishes its capture without rows),
 * and API 43's strip otherwise; API 51: no strip on another scene either (the kernel publishes the region's
 * current scene), only while the region has no screen yet. */
#define VJO_GAME_RESERVE_HOOK_ONLY (1u * 1024u * 1024u)
/* API 23: an Anki screenshot's block (game_ocr/main.c run_shot, "VjoGameShot"): the JPEG
 * encoder's state (core/jpegsw.h VJO_JPEG_STATE_BYTES, 4,096 B at most), one strip of
 * 64 colour rows at 960 px (245,760 B) and the JPEG, at most VJO_GAME_OCR_SHOT_MAX
 * (196,608 B), page-rounded: 450,560 B, only while a shot runs. A shot is a job of
 * the one job slot, so it never runs beside an OCR job's workspace, capture, tall
 * line, frame block or helpers (1,929,216 B of the job's worst case besides the resident weights, LM,
 * cascade model, SC3 charset and, API 54, the text spotter's block, which a shot keeps: 9,101,312 + 450,560 B
 * under the job's 11,022,336 B, VOCR6's 4 MB LM, API 55's frame block): it adds nothing to VJO_WORST_GAME_JOB. Its own memory check
 * keeps VJO_GAME_RESERVE free beside it. */
#define VJO_BUDGET_GAME_SHOT 450560u
/* A MAGES SC3 hook's charset (game_ocr/main.c hooks_sc3_load, "VjoGameSc3"; docs/native-hooks.md "SC3 text"): the
 * game's glyph table, read at the first job of a game with an SC3 site and held until the worker stops, USER_RW,
 * page-rounded: its file is at most VJO_SC3_MAX_BYTES (core/hook_sc3.h, 16,384 B: CHAOS;CHILD's 16,032 B take 16,384 B,
 * STEINS;GATE's 5,364 B and ROBOTICS;NOTES's 6,432 B 8,192 B). Only in those games; held beside every job, so in the
 * job's worst case: 10,403,840 -> 10,420,224 B. Its load keeps VJO_GAME_RESERVE_HOOK_ONLY free beside it. */
#define VJO_BUDGET_GAME_SC3 16384u
/* API 54: the text spotter's weights and their parsed form (game_ocr/main.c resident_alloc, "VjoGameVocrSpot"; core/
 * vocr_ocr.h VjoVocrSpotResident), USER_RW only, resident like the LM while the game is the active one and its jobs
 * ask for the spotter (ocr_finder = spotter, the default), given back with the others when a job does not fit,
 * the game leaves the front or a job asks for GRAD. Its worst case is the largest blob the protocol's rule admits,
 * VJO_VOCR_SPOT_MAX_BYTES (512,000 B), and the parsed tables (sizeof(vocr_textspot6), 3,964 B on the Vita
 * [arm-vita-eabi-gcc, the pinned header]), each 16-byte aligned, page-rounded: 516,096 B, so the pin can move to
 * vita-vn-ocr's published pick without a change here (game_ocr/main.c refuses a larger block; core/vocr_ocr.c
 * checks the sum against this on 32-bit builds). The pin's 52,176 B (v0.3.10-textspot8.1) take 57,344 B. */
#define VJO_BUDGET_GAME_SPOT 516096u
/* API 26: the text-presence gate's weights (game_ocr/main.c gate_alloc, "VjoGameVocrGate"; core/vocr_ocr.h
 * VjoVocrGateResident), USER_RW only, resident like the LM while the game is the active one and its jobs ask for the
 * gate (ocr_text_gate = on; off by default), given back with the others: the pinned file's 1,540 B
 * (VJO_VOCR_GATE_BYTES), page-rounded. #418: they were 1,560 B of the worker's .bss, in every game whether the gate
 * was on or off; the block is taken in the room beyond the job's reserve and the blocks its memory check counted. */
#define VJO_BUDGET_GAME_GATE 4096u
#define VJO_WORST_GAME_PERMANENT (VJO_BUDGET_GAME_MODULE_PAGES + VJO_BUDGET_GAME_STACK)
#define VJO_WORST_GAME_JOB (VJO_BUDGET_GAME_WEIGHTS + VJO_BUDGET_GAME_WORKSPACE + VJO_BUDGET_GAME_CAPTURE + \
                            VJO_BUDGET_GAME_LINE_OR_FRAME + VJO_BUDGET_GAME_LM + VJO_BUDGET_GAME_CASCADE + \
                            VJO_BUDGET_GAME_HELPERS + VJO_BUDGET_GAME_SC3 + VJO_BUDGET_GAME_SPOT + \
                            VJO_BUDGET_GAME_GATE)
/* The recognizer ceiling: weights <= 2,502,656 B, the owner's int8 ceiling for the Large tier
 * (2026-10-14 18:09Z: Small < 1 MB, Normal <= 1.6 MB, Large <= 2.5 MB; 2,500,000 B page-rounded;
 * 1,601,536 B, Normal's 1.6 MB, before VOCR5's Large), and a full-screen workspace of at most
 * VJO_BUDGET_GAME_WORKSPACE (by construction since API 22), and (API 20) the 4 MB LM and the
 * cascade's larger model: a job's peak, which the preflight (free USER minus the job's reserve,
 * 1 MiB since PR #280, 4-16 MiB by tier before) has to cover. Raised from 2,392,064 B for the LM and the cascade (8,105,984 B:
 * 9 % of the 90,177,536 B free USER, the least any job saw in CLANNAD [measured, #81's
 * v0.8.0-test.1 log]) and for API 22's parallel lines (the helpers' line slots in the workspace
 * and two 20,480 B thread stacks: 8,720,384 B, 9.7 %). Raised again for VOCR5's Large tier, the
 * weights and cascade budgets at 2,473,984 B each and the workspace with VOCR5-L's cascade beside
 * VOCR5-N: 10,567,680 B, 11.7 % of that free USER (a job's real peak, VOCR5-N with VOCR5-L's
 * cascade, the 4 MB LM and both helpers, is 1,286,144 B less: the two model budgets' over-count).
 * A game with less runs fewer helpers, a smaller LM, no cascade or none of them, then a smaller
 * tier (game_ocr/main.c), never a smaller tier for the helpers. */
#define VJO_LIMIT_GAME_WEIGHTS 2502656u
/* API 54: raised from 1,220,608 B by the spotter's part of the scratch (VJO_BUDGET_GAME_WORKSPACE): +77,824 B; #528
 * (mem-game): back to 1,220,608 B, the part running over the job's extras (the budget a page under it). */
#define VJO_LIMIT_GAME_WORKSPACE 1220608u
/* API 34 adds the measured 8192 B of worker module pages to the prior ceiling, and vita-vn-ocr #109's
 * counter re-decode its 4,096 B. API 42 (vita-vn-ocr #120's head gate, #122's two-level head) adds the Large
 * blob's page in the weights and cascade budgets (8,192 B), the workspace's two pages (8,192 B) and the worker's
 * two text pages (8,192 B, both builds): the job's worst case 10,567,680 -> 10,584,064 B, 11.7 % of that free
 * USER (a real peak, VOCR5-N with VOCR5-L's cascade, the 4 MB LM and both helpers, 9,297,920 B, is 1,286,144 B
 * less). VOCR6's 4 MB LM (vita-vn-ocr #127, 3,603,552 B against ctclm.2's 3,781,072 B) lowers the LM budget and
 * the job's worst case by 180,224 B, to 10,403,840 B (11.5 %; the real peak 9,117,696 B); the limit is kept. VOCR6's
 * Normal and Large (vita-vn-ocr #125) change no budget: Large's block and the workspace keep their pages, and Normal's
 * weights block is a page smaller, so the real peak (VOCR6-N with VOCR6-L's cascade, the 4 MB LM and both helpers) is
 * 9,113,600 B, 1,290,240 B under the worst case without an SC3 charset (10,403,840 B; 10,420,224 B with it). The
 * follow-up set (v0.4.1-vocr6.2: VOCR6-S, VOCR6-N with its gate again, VOCR6-L) changes none either: Normal's
 * weights block is a page larger again (1,191,936 B), Small's (823,296 B), Large's and the workspace keep their pages,
 * so that real peak is 9,117,696 B with GRAD and 9,252,864 B with the spotter (its workspace 1,298,432 B and the
 * pin's 57,344 B block), 1,744,896 B under the worst case without an SC3 charset (10,997,760 B; 11,014,144 B with it).
 * API 54 adds the text spotter's: its block (VJO_BUDGET_GAME_SPOT, 516,096 B), its part of the workspace (77,824 B)
 * and the worker's four text pages (16,384 B, both builds), 610,304 B: 11,214,848 B, 12.4 % of the 90,177,536 B
 * free USER the least any job saw in CLANNAD [measured, #81's v0.8.0-test.1 log] (the worst case 11,014,144 B with
 * an SC3 charset, 12.2 %). Only a job with ocr_finder = spotter takes them, and the spotter only takes the room its check leaves
 * beside the tier, the LM and the cascade (game_ocr/main.c). API 55's frame block (ocr_pause, off by default) in
 * place of the tall line's, the larger of the two (a job holds one of them, never both), adds 8,192 B: the worst
 * case 11,022,336 B with an SC3 charset (12.2 %), under the limit, which is kept. */
#define VJO_LIMIT_GAME_JOB 11214848u

#endif
