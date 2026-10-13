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

/* Every thread costs its stack plus at least 4 KiB of TLS from its process's
 * budget [SDK: Kernel-Reference, sceKernelCreateThread]. */
#define VJO_LEDGER_TLS 4096u
#define VJO_LEDGER_PAGE 4096u

/* ---- SceShell's partition (ScePhyMemPartShell, 50 MiB; pool "shell pages") ----
 * Module pages are permanent; they may only shrink (AGENTS.md rule 1). The
 * values are the packaged SELF segments rounded to pages [measured, PR #60]. */
#define VJO_BUDGET_SHELL_MODULE_PAGES 405504u      /* VitaJPOverlay_Shell.suprx: 225,280 text + 180,224 data/bss */
#define VJO_BUDGET_WORKER_IN_SHELL_PAGES 45056u    /* VitaJPOverlay_OCR.suprx, which *ALL also loads into SceShell */
#define VJO_STACK_SHELL_NET 65536u                 /* VjoNet: BearSSL needs it for TLS */
#define VJO_STACK_SHELL_CTL 16384u                 /* VjoControl */
#define VJO_STACK_SHELL_ANKI 65536u                /* VjoAnki, only while a card is saved or sent */
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
#define VJO_BUDGET_PAF_PLUGIN 8192u                /* the RCO (5,632 B file) and the Paf Plugin object [estimate] */
#define VJO_BUDGET_PAF_RESULTS_LOCAL 65536u        /* one result arena, local dictionary (worker.c LOCAL_RESULT_ARENA_SIZE) */
#define VJO_BUDGET_PAF_RESULTS_NET 571888u         /* 2 x 262,144 arenas + 47,600 connection pool (worker.c MEM_SIZE) */
#define VJO_BUDGET_PAF_ANKI 393216u                /* an Anki job with a picture field (anki.c ANKI_PICTURE_MEM) */
#define VJO_WORST_SHELL_PAF_PERMANENT VJO_BUDGET_PAF_PLUGIN
#define VJO_WORST_SHELL_PAF (VJO_WORST_SHELL_PAF_PERMANENT + VJO_BUDGET_PAF_RESULTS_NET + VJO_BUDGET_PAF_ANKI)
/* What the measured free heap leaves us once the reserve is kept: the
 * reserve is the headroom. Later sessions saw 355-441 KiB and 190 KiB free
 * [measured]; under those every allocation is refused, which the code
 * survives (tested), so this limit is the best case, not a guarantee. */
#define VJO_LIMIT_SHELL_PAF (VJO_PAF_FREE_MEASURED - VJO_PAF_RESERVE)

/* ---- the kernel's partition (ScePhyMemPartKD, 36 MiB shared by every driver; pool "kernel") ---- */
#define VJO_BUDGET_KERNEL_MODULE_PAGES 114688u     /* VitaJPOverlay_Kernel.skprx: 32,768 text + 81,920 data/bss [measured] */
#define VJO_STACK_KERNEL 16384u                    /* VjoWorker */
#define VJO_BUDGET_KERNEL_STACK VJO_STACK_KERNEL
#define VJO_BUDGET_KERNEL_CAPTURE (960u * 544u * 4u) /* VjoCapture at a full frame, KERNEL_RW, per job */
#define VJO_WORST_KERNEL_PERMANENT (VJO_BUDGET_KERNEL_MODULE_PAGES + VJO_BUDGET_KERNEL_STACK)
#define VJO_WORST_KERNEL (VJO_WORST_KERNEL_PERMANENT + VJO_BUDGET_KERNEL_CAPTURE)
/* No cited safe size exists for this pool: its free space on a retail console
 * is unmeasured, and a 2,088,960 B capture has failed there once (#14) and
 * succeeded since. Until the self-test measures the partition, the limit is
 * today's worst case: a ratchet, so nothing grows unnoticed. */
#define VJO_LIMIT_KERNEL VJO_WORST_KERNEL

/* ---- the game's partition (ScePhyMemPartGame, 256 MiB minus the game's own use; pool "game") ---- */
#define VJO_BUDGET_GAME_MODULE_PAGES VJO_BUDGET_WORKER_IN_SHELL_PAGES
#define VJO_STACK_GAME 32768u                      /* VjoGameVocr */
#define VJO_BUDGET_GAME_STACK (VJO_STACK_GAME + VJO_LEDGER_TLS)
#define VJO_BUDGET_GAME_WEIGHTS 1560576u           /* F20, the largest accepted model, page-rounded (core/vocr_ocr.c) */
#define VJO_BUDGET_GAME_WORKSPACE 770048u          /* F20 at a 960x544 region, page-rounded (vjo_vocr_job_bytes) */
#define VJO_GAME_RESERVE (4u * 1024u * 1024u)      /* kept free in the game before either block (game_ocr/main.c) */
#define VJO_WORST_GAME_PERMANENT (VJO_BUDGET_GAME_MODULE_PAGES + VJO_BUDGET_GAME_STACK)
#define VJO_WORST_GAME_JOB (VJO_BUDGET_GAME_WEIGHTS + VJO_BUDGET_GAME_WORKSPACE)
/* The recognizer ceiling tier: weights <= 1,601,536 B and a full-screen
 * workspace <= 790,528 B, peak 2,392,064 B, which the preflight (free USER
 * minus the 4 MiB reserve) has to cover [vita-vn-ocr docs/vita-memory-budget.md]. */
#define VJO_LIMIT_GAME_WEIGHTS 1601536u
#define VJO_LIMIT_GAME_WORKSPACE 790528u
#define VJO_LIMIT_GAME_JOB 2392064u

#endif
