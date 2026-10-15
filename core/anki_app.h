/* What the Overlay app (settings/anki_page.c) does with Anki: it finds
 * AnkiConnect on the LAN, tests the connection and sends the queued cards.
 * None of this is in the Shell plugin: SceShell only saves cards (×) and
 * sends them to a known address (△, shell/anki.c). The queue's storage is
 * shell/anki_queue.c's, the notes are core/anki.c's Kiku notes. */
#ifndef VJO_ANKI_APP_H
#define VJO_ANKI_APP_H

#include "anki.h"

/* The app's services (settings/net.c on the Vita, stubs on the host). */
typedef struct {
    void *ud;
    /* This console's IPv4 address and netmask (host byte order); < 0 if none. */
    int (*local_ipv4)(void *ud, uint32_t *ip, uint32_t *mask);
    /* Connects to port of up to VJO_ANKI_PROBE_MAX hosts at once; open[i] = 1
     * for those that accept within window_us. Returns the hosts tried. */
    int (*probe)(void *ud, const uint32_t *ips, int n, int port, int window_us, uint8_t *open);
    /* anki_host.txt, where a search keeps what it found ("IP:port"): its
     * text into line (0), or < 0 when there is none; and writing it. */
    int (*load_host)(void *ud, char *line, size_t cap);
    void (*save_host)(void *ud, const char *line);
    int (*stopped)(void *ud);               /* the user cancelled (the cancel button) */
    void (*status)(void *ud, const char *msg); /* progress, for the screen */
    const char *enter_glyph; /* the console's enter button (core/buttons.h), as messages name it */
} VjoAnkiAppOps;

/* What was typed for AnkiConnect's address, as anki_host: trimmed,
 * "http://" and a trailing '/' dropped, auto in lowercase, "" for nothing.
 * -1 when it is not an IP[:port], a host name[:port] or auto. */
int vjo_anki_app_address(const char *typed, char *out, size_t cap);

#define VJO_ANKI_PROBE_MAX 32
#define VJO_ANKI_SCAN_WINDOW_US 350000 /* per batch of VJO_ANKI_PROBE_MAX hosts */

/* Where Anki is: anki_host's IP[:port], or for auto the saved address when
 * AnkiConnect answers there, else a search of the local /24 (the first
 * AnkiConnect wins, saved for △). *searched: a search found it. Returns
 * VJO_OK, VJO_E_NET (not set), VJO_E_NOT_FOUND, VJO_E_CANCELLED. */
int vjo_anki_app_endpoint(VjoArena *a, const VjoPlatform *p, const VjoAnkiAppOps *ops, const char *anki_host,
                          char *host, size_t cap, int *port, int *searched);

/* The connection test's findings. */
typedef struct {
    int rc;            /* VJO_OK: cards can be sent; else why not (VJO_E_*) */
    char host[64];
    int port, searched;
    int kiku;          /* the Kiku note type exists */
    int missing;       /* how many of the fields a card fills it lacks */
    char missing_names[96]; /* their names, comma-separated (cut) */
    int deck;          /* the Kiku deck exists (else the first card creates it) */
} VjoAnkiTest;

/* version, modelFieldNames(Kiku), deckNames. Fills t and a one- or
 * two-sentence result for the screen in msg. Returns t->rc. */
int vjo_anki_app_test(VjoArena *a, const VjoPlatform *p, const VjoAnkiAppOps *ops, const VjoConfig *cfg,
                      VjoAnkiTest *t, char *msg, size_t cap);

typedef struct {
    int queued;     /* cards when it began */
    int sent, duplicates, unreadable, too_large, no_audio;
    int rc;         /* VJO_OK: the queue was gone through; else why it stopped */
} VjoAnkiSend;

/* Sends every queued card, oldest ID first, as △ does (findNotes by the
 * card's tag first, so a lost reply never adds a note twice; a duplicate is
 * archived; a card leaves the queue only once Anki has it), with its
 * screenshot when the card has one and it fits in a. Progress through
 * ops->status, the result in s and msg. Returns s->rc. */
int vjo_anki_app_send(VjoArena *a, const VjoPlatform *p, const VjoAnkiAppOps *ops, const VjoConfig *cfg,
                      VjoAnkiSend *s, char *msg, size_t cap);

#endif
