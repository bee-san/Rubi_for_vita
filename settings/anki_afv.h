/* The Rubi app's side of Anki for Vita (#220): cards handed to it through files (by themselves when
 * the app starts and when its Anki page is shown, unless Rubi's setting is off; or on Enter on the
 * page's row), their results read back. Anki for Vita is a separate Vita app (bee-san/Anki_for_vita) that keeps a copy of the user's
 * Anki collection and syncs it with a sync server; other apps hand it notes through plain files under
 * ux0:data/Anki_for_vita/ (its docs/interface.md, version 1). Rubi has no network code: it only writes
 * and reads these files, as it writes .apkg files.
 *
 * Send (vjo_afv_send, the Anki page's worker thread): every queued card that is not in the inbox yet
 * becomes inbox/rubi/<id>.json, a Lapis note made by the .apkg export's own code: <id> the queue ID,
 * its GUID vjo_apkg_guid's (the same note whichever way it reaches Anki), its fields
 * vjo_lapis_field_value's, its screenshot inbox/rubi/media/vitajp_<32 hex>.jpg (vjo_apkg_media_name),
 * the note type inbox/rubi/notetypes/lapis-1.7.0.json (core/lapis.c's schema-11 JSON). Every file is
 * new: written to <name>.tmp, flushed, closed, renamed (the Vita's rename refuses an existing name, so
 * no inbox file is ever replaced), and the memory card synced; the media and the note type first,
 * synced, then the notes (their .tmp files written meanwhile, renamed after that sync). A card whose
 * note is in the inbox already is left alone. A media or note type file that is there already must
 * hold Rubi's bytes (else the card, or the send, stops; a note type file is taken back while no note
 * of Rubi's is in the inbox). The cards stay queued, so the .apkg export keeps working, until their
 * result is final.
 *
 * Results (vjo_afv_results, when the page is shown, and before a send): outbox/rubi/<id>.json (its
 * .old when a replace was stopped; both missing: three more tries 100 ms apart), only names of 32
 * lowercase hex digits that are Rubi's own cards (queued, archived, or found in Anki before: no other
 * file is opened), each read whole up to VJO_AFV_RESULT_MAX: valid UTF-8, no NUL, no key twice,
 * "v" 1 (another version's is absent), its id its name's; a malformed one is ignored and logged.
 *   added or exists, synced: the card moves to anki_queue/exported/, as an export moves it;
 *   duplicate: it becomes anki_queue/<id>.duplicate.json, the queue's "found in Anki" state;
 *   rejected: it stays queued, for an .apkg; the page shows why; a send never hands it in again (the
 *     app takes an id whose result is final as done);
 *   added or exists, not synced yet: nothing changes.
 * A final result is removed once it is applied (the interface lets a client), with a leftover .old,
 * and so is one whose card is not queued but archived or a duplicate already; anything else is left
 * alone. Then Rubi's own inbox files no note needs go (the interface: the client removes them): the
 * media files no note left in the inbox names, the note type's file when no note is left.
 *
 * All calls are made by one thread at a time (the page's). Every storage call that fails is logged
 * (vjo_log, log.txt) with its error code; no card's text is logged. */
#ifndef VJO_SETTINGS_ANKI_AFV_H
#define VJO_SETTINGS_ANKI_AFV_H

#include <stddef.h>
#include <stdint.h>

#include "../core/anki.h"
#include "../core/anki_index.h" /* VJO_AFV_DIR */
#include "../core/arena.h"
#include "anki_export.h"

#define VJO_AFV_CLIENT "rubi"
#define VJO_AFV_INBOX_ROOT VJO_AFV_DIR "/inbox"
#define VJO_AFV_INBOX_DIR VJO_AFV_INBOX_ROOT "/" VJO_AFV_CLIENT
#define VJO_AFV_MEDIA_DIR VJO_AFV_INBOX_DIR "/media"
#define VJO_AFV_NOTETYPES_DIR VJO_AFV_INBOX_DIR "/notetypes"
#define VJO_AFV_OUTBOX_ROOT VJO_AFV_DIR "/outbox"
#define VJO_AFV_OUTBOX_DIR VJO_AFV_OUTBOX_ROOT "/" VJO_AFV_CLIENT
#define VJO_AFV_STATUS_PATH VJO_AFV_DIR "/status.json"
/* The note type's file in the inbox: Lapis 1.7.0, vendored (third_party/lapis, core/lapis.c). */
#define VJO_AFV_NOTETYPE_FILE "lapis-1.7.0.json"
/* Where to get the app (the page says so when it is not installed). */
#define VJO_AFV_WHERE "github.com/bee-san/Anki_for_vita"
/* Rubi's own choice whether the app hands its queued cards over by itself (the owner, 2026-10-19:
 * "write directly to the anki sync app"): "anki_for_vita = auto" (the default, also when the file is
 * not there) or "anki_for_vita = off". The Rubi app's alone, not in config.ini: the Shell, which reads
 * config.ini, has no use for it and no text room for one more key (anki_page.h). */
#define VJO_AFV_SETTING_PATH VJO_EXPORT_DATA_DIR "/anki_for_vita.txt"
/* The interface's limit for a note's file; a card whose note is larger stays queued. */
#define VJO_AFV_NOTE_MAX (64u * 1024u)
/* A result's and status.json's read: the interface's limit for a JSON file (Anki for Vita writes them
 * in a few hundred bytes; a later version may add keys). One is read at a time. */
#define VJO_AFV_RESULT_MAX (64u * 1024u)
/* The cards one send hands over, the result files one pass lists (as many: a result is a card handed
 * over; more are logged and read once fewer are left), the inbox notes a sweep of Rubi's inbox media
 * reads (more: the media stay until they are fewer). */
#define VJO_AFV_SEND_MAX VJO_APKG_MAX_CARDS
#define VJO_AFV_RESULTS_MAX VJO_APKG_MAX_CARDS
#define VJO_AFV_SWEEP_NOTES 256
#define VJO_AFV_MESSAGE_MAX 160

/* ---------------- Rubi's setting ---------------- */

/* VJO_AFV_SETTING_PATH (its .old when a replace was stopped): 1 auto (also without the file, or with a
 * value it does not know), 0 off. a is given back. */
int vjo_afv_auto(VjoArena *a);
/* Writes it (auto 1, off 0) by the interface's replace rule (.tmp, the file to .old, the .tmp to its
 * name, the card synced, the .old removed): 0, or -1 (logged; the file as it was, or its .old). */
int vjo_afv_set_auto(int on);

/* ---------------- status.json ---------------- */

typedef struct {
    int known;                    /* status.json was there and read */
    int signed_in;
    char server[64];              /* a host name, as the app wrote it ("" none) */
    int64_t last_sync;            /* seconds UTC of the last sync that ended without an error; 0 never */
    char last_result[24];         /* ok, error, cancelled, full_sync_needed ("" none) */
    int pending;                  /* inbox notes whose result is not final yet (every client's) */
} VjoAfvStatus;

/* status.json's text into s: 0, or -1 (not a version-1 status; s->known 0). */
int vjo_afv_status_parse(VjoArena *a, const char *json, size_t len, VjoAfvStatus *s);
/* Reads ux0:data/Anki_for_vita/status.json (its .old when a replace was stopped): s->known 0 when
 * there is none or it is not one. a is given back. */
void vjo_afv_status_read(VjoArena *a, VjoAfvStatus *s);
/* One line for the page: signed in or not, the last sync (how long before now, seconds UTC; 0
 * unknown) and its result, the notes waiting. */
void vjo_afv_status_text(const VjoAfvStatus *s, int64_t now, char *out, size_t cap);

/* ---------------- results ---------------- */

enum { VJO_AFV_NONE = 0, VJO_AFV_ADDED, VJO_AFV_EXISTS, VJO_AFV_DUPLICATE, VJO_AFV_REJECTED };

typedef struct {
    int status;                         /* VJO_AFV_* */
    int synced;                         /* the server has the note (added, exists) */
    char message[VJO_AFV_MESSAGE_MAX];  /* why, for rejected; control characters made spaces */
} VjoAfvResult;

/* A result's text for card id (32 hex digits): 0, or -1 (not a version-1 result for that id). */
int vjo_afv_result_parse(VjoArena *a, const char *json, size_t len, const char *id, VjoAfvResult *r);
/* Final: the card leaves the queue (or stays rejected) and its result file can go. */
int vjo_afv_result_final(const VjoAfvResult *r);

typedef struct {
    int in_anki;     /* added or exists, synced: moved to anki_queue/exported/ */
    int duplicate;   /* Anki has the word already: anki_queue/<id>.duplicate.json */
    int rejected;    /* refused: still queued (message: the first one's why) */
    int waiting;     /* added or exists, not synced yet */
    int malformed;   /* result files ignored (log.txt) */
    int failed;      /* a move or removal failed: tried again next time (log.txt) */
    int removed;     /* final result files removed */
    int swept;       /* inbox files of Rubi's no note needs any more, removed (media, the note type) */
    int more;        /* over VJO_AFV_RESULTS_MAX: read next time */
    char message[VJO_AFV_MESSAGE_MAX];
} VjoAfvResults;

/* Applies the outbox's results to the queue, as above. 0, VJO_E_CANCELLED (ui's stop: what was done
 * is kept), or VJO_E_LOCAL_IO (the outbox cannot be read; nothing changed). ui may be NULL. */
int vjo_afv_results(VjoArena *a, const VjoExportUi *ui, VjoAfvResults *r);
/* One or two sentences about r for the screen ("" when there is nothing to say). */
void vjo_afv_results_text(const VjoAfvResults *r, char *out, size_t cap);

/* ---------------- a card's note ---------------- */

/* The card's note file (version 1 of the interface): id the queue ID, note its record's, media its
 * screenshot's name or NULL, created the note's time. Its fields are the .apkg note's, by Lapis's
 * names, each with its value (vjo_lapis_field_value; a 0x1F, Anki's field separator, made a space as
 * vjo_lapis_fields makes it); empty fields are left out (the interface: a field not given is empty),
 * but for the two duplicate_check names. NULL on OOM; *len its length. */
char *vjo_afv_note_json(VjoArena *a, const char id[VJO_QUEUE_ID_SIZE], const VjoAnkiNote *note, const char *media,
                        int64_t created, size_t *len);
/* The note's created time for a card's time: the .apkg note's (the card's, VJO_APKG_NO_TIME without
 * one), or VJO_APKG_NO_TIME when it is outside the interface's 2006-2100 (a console clock that was
 * wrong). */
int64_t vjo_afv_created(int64_t card_time);

/* ---------------- send ---------------- */

typedef struct {
    int rc;              /* VJO_OK, or why nothing was handed over (VJO_E_*) */
    int installed;       /* Anki for Vita speaks interface version 1 */
    int sent;            /* notes handed over now */
    int refused;         /* left queued: Anki for Vita refused them (final under their id: an .apkg takes them) */
    int already;         /* in the inbox already: left alone */
    int unreadable;      /* left queued: a damaged or edited record, a read that failed (log.txt) */
    int too_large;       /* left queued: its note would be over VJO_AFV_NOTE_MAX */
    int failed;          /* left queued: a file of it could not be written (log.txt) */
    int missing_picture; /* handed over without an absent or damaged screenshot */
    int left;            /* over VJO_AFV_SEND_MAX: for the next send */
    int stopped;         /* Cancel: not handed over (each stays queued, untouched) */
    VjoAfvResults results; /* applied first */
} VjoAfvSendResult;

/* Hands the queue's cards to Anki for Vita, as above, after applying its results. A one- or
 * two-sentence result for the screen in msg. Returns r->rc. */
int vjo_afv_send(VjoArena *a, const VjoExportUi *ui, VjoAfvSendResult *r, char *msg, size_t cap);

#endif
