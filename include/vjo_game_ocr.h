/* Bounded OCR-only transport. No pointers or executable addresses cross it. */
#ifndef VJO_GAME_OCR_H
#define VJO_GAME_OCR_H

#include <stdint.h>

#define VJO_GAME_OCR_MODULE "VitaJPOverlay_OCR"
#define VJO_GAME_OCR_MODEL_DIR_BYTES 256u
#define VJO_GAME_OCR_TEXT_BYTES 4096u
#define VJO_GAME_OCR_MODEL_BYTES 32u

typedef struct {
    uint32_t size;
    uint32_t seq;      /* Assigned by Submit; never reused within this kernel module's lifetime. */
    uint32_t done_seq; /* Exact completed single-pass capture owned by this job. */
    uint32_t width, height, stride;
    char model[VJO_GAME_OCR_MODEL_BYTES];
    char model_dir[VJO_GAME_OCR_MODEL_DIR_BYTES];
} VjoGameOcrRequest;

typedef struct {
    uint32_t size, seq;
    int32_t rc;
    uint32_t workspace_bytes, lines, find_us, load_us, recognize_us;
    int32_t io_error; /* model I/O error, separate from recognized text */
    int32_t cleanup_status; /* Zero only after workspace and file cleanup completed. */
    /* UTF-8 OCR text on success; every failed result carries empty text. */
    char text[VJO_GAME_OCR_TEXT_BYTES];
} VjoGameOcrResult;

#ifdef __cplusplus
extern "C" {
#endif
int vjoOcrRegister(void); /* Current active game, verified module; retry at startup. */
int vjoOcrTake(VjoGameOcrRequest *out); /* 0 claimed, 1 no job, <0 error. */
int vjoOcrCancelled(uint32_t seq); /* Worker: 0 active, 1 cancelled, <0 stale. */
int vjoOcrReadRaw(uint32_t seq, uint32_t row, uint32_t n, void *dst);
int vjoOcrComplete(const VjoGameOcrResult *result); /* Worker: publish or drain. */
int vjoOcrSubmit(const VjoGameOcrRequest *request); /* Shell: sequence >0. */
int vjoOcrRead(uint32_t seq, VjoGameOcrResult *out); /* Shell: 0 done, 1 pending. */
int vjoOcrCancel(uint32_t seq); /* Signals cancellation; worker must acknowledge cleanup. */
#ifdef __cplusplus
}
#endif

#endif
