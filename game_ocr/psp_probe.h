/* The debug build's read-only PSP probe in the game worker (docs/native-hooks.md "PSP probe (debug
 * build)"): game_ocr/psp_probe.c, built into the debug build's worker only (vita/CMakeLists.txt). The
 * job path calls vjo_psp_probe_job at each OCR job; it probes once per process, at the first one, in the
 * PSP emulator's process only and only when the text source is not OCR. Anywhere else (the host tests and
 * the Vita3K smoke app, which build game_ocr/main.c without the worker's other sources) it is nothing; the
 * normal build has no call. */
#ifndef VJO_GAME_OCR_PSP_PROBE_H
#define VJO_GAME_OCR_PSP_PROBE_H

#if VJO_DEBUG && !defined(VJO_HOST) && !defined(VJO_GAME_OCR_NO_MODULE_ENTRY)
/* psp: this process is the PSP emulator's (game_ocr/main.c psp_title). */
void vjo_psp_probe_job(int psp);
#else
#define vjo_psp_probe_job(psp) ((void)(psp))
#endif

#endif
