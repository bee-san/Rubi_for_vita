/* Bounded OCR-only transport. No pointers or executable addresses cross it. */
#ifndef VJO_GAME_OCR_H
#define VJO_GAME_OCR_H

#include <stdint.h>

#define VJO_GAME_OCR_MODULE "VitaJPOverlay_OCR"
#define VJO_GAME_OCR_MODEL_DIR_BYTES 256u
#define VJO_GAME_OCR_TEXT_BYTES 4096u
#define VJO_GAME_OCR_MODEL_BYTES 32u
/* The kernel's one staging buffer, and the most one model Read copies: the
 * game reads and hashes the weights in pieces of this size. */
#define VJO_GAME_OCR_MODEL_CHUNK 32768u

typedef struct {
    uint32_t size;
    uint32_t seq;      /* Assigned by Submit; never reused within this kernel module's lifetime. */
    uint32_t done_seq; /* Exact completed single-pass capture owned by this job. */
    uint32_t width, height, stride;
    char model[VJO_GAME_OCR_MODEL_BYTES];
    char model_dir[VJO_GAME_OCR_MODEL_DIR_BYTES];
} VjoGameOcrRequest;

/* VjoGameOcrResult.weights: how this job had verified weights. */
#define VJO_GAME_OCR_WEIGHTS_NONE 0u     /* no line found, or the job failed first */
#define VJO_GAME_OCR_WEIGHTS_READ 1u     /* read from the card and SHA-256 checked */
#define VJO_GAME_OCR_WEIGHTS_RESIDENT 2u /* kept from an earlier job; the file is unchanged */

typedef struct {
    uint32_t size, seq;
    int32_t rc;
    uint32_t workspace_bytes, lines, find_us, load_us, recognize_us;
    int32_t io_error; /* model I/O error, separate from recognized text */
    int32_t cleanup_status; /* Zero only after workspace and file cleanup completed. */
    uint32_t weights;        /* VJO_GAME_OCR_WEIGHTS_* */
    uint32_t resident_bytes; /* weights block kept in the game for the next job (page-rounded), 0 none */
    /* The model file's modification time could not be read (native error):
     * every job then reads and checks the weights again. */
    int32_t stat_error;
    /* UTF-8 OCR text on success; every failed result carries empty text. */
    char text[VJO_GAME_OCR_TEXT_BYTES];
} VjoGameOcrResult;

/* Model Open's report of the file: its pinned length and the version the
 * game keys its resident weights on. */
typedef struct {
    uint32_t size;      /* sizeof(VjoGameOcrModelInfo) */
    uint32_t bytes;     /* file length; equals the pinned model size */
    int32_t stat_rc;    /* 0, or the native error of the modification-time query */
    uint32_t reserved;
    uint64_t mtime;     /* last modification, packed (kernel/game_ocr.c); 0 if unknown */
} VjoGameOcrModelInfo;

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
/* Model-only broker. The kernel worker thread does the file I/O; each call
 * waits (bounded, without the kernel lock) for that thread to finish the
 * operation. VJO_ERR_BUSY means it is still queued/in flight: repeat the exact
 * operation after yielding. Open returns the verified file length and, when
 * info is non-NULL, copies a VjoGameOcrModelInfo there. Read returns copied
 * bytes (possibly short or EOF), at most VJO_GAME_OCR_MODEL_CHUNK. Close
 * drains even a cancelled job. The kernel never stores a user pointer,
 * accepts paths, or exports a file FD. */
int vjoOcrModelOpen(uint32_t seq, VjoGameOcrModelInfo *info);
int vjoOcrModelRead(uint32_t seq, uint32_t offset, uint32_t bytes, void *dst);
int vjoOcrModelClose(uint32_t seq);
#ifdef __cplusplus
}
#endif

#endif
