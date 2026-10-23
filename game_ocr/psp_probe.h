/* The debug build's read-only PSP probe in the game worker (docs/native-hooks.md "PSP probe (debug
 * build)"): game_ocr/psp_probe.c, built into the debug build's worker only (vita/CMakeLists.txt). The
 * worker's loop calls vjo_psp_probe_after once the kernel has accepted a job's result (game_ocr/main.c
 * worker_main), so nothing the probe does delays that job's popup; it delays only the worker's next job, by
 * its own time. It acts only in the PSP emulator's process and only after a job whose request carries
 * VJO_GAME_OCR_S_PSP_PROBE, which only the debug build's Shell sets (config.ini psp_probe, on by default). Anywhere else (the host tests and the
 * Vita3K smoke app, which build game_ocr/main.c without the worker's other sources) it is nothing, and the
 * normal build has no call. */
#ifndef VJO_GAME_OCR_PSP_PROBE_H
#define VJO_GAME_OCR_PSP_PROBE_H

#include <stdint.h>

#include "../include/vjo_game_ocr.h"

#if VJO_DEBUG && !defined(VJO_HOST) && !defined(VJO_GAME_OCR_NO_MODULE_ENTRY)
/* psp: this process is the PSP emulator's (game_ocr/main.c psp_title); title: the worker's title (its
 * process's: NPXS10028, PSPEMUCFW or a bubble's, at most 12 bytes); r and res: the job's request and the
 * result the kernel has; job_us: the job's time in the worker. */
void vjo_psp_probe_after(int psp, const char *title, const VjoGameOcrRequest *r, const VjoGameOcrResult *res,
                         uint32_t job_us);
#else
#define vjo_psp_probe_after(psp, title, r, res, job_us)                                                     \
    ((void)(psp), (void)(title), (void)(r), (void)(res), (void)(job_us))
#endif

#endif
