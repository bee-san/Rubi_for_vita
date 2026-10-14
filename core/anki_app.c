#include "anki_app.h"

#include <string.h>

#include "port.h"
#include "utf.h"
#include "../shell/anki_queue.h"

/* send_one: the audio source is down (the card waits, as with △). */
#define AUDIO_DOWN (-1000)

int vjo_anki_app_address(const char *typed, char *out, size_t cap)
{
    char host[64];
    int port, url = 0;
    size_t n;
    while (*typed == ' ')
        typed++;
    if (vjo_ieq_prefix(typed, "http://")) {
        typed += 7;
        url = 1;
    }
    vjo_snprintf(out, cap, "%s", typed);
    n = strlen(out);
    while (n && (out[n - 1] == ' ' || out[n - 1] == '/'))
        out[--n] = '\0';
    if ((url && !n) || vjo_anki_endpoint(out, host, sizeof(host), &port) < 0)
        return -1;
    if (vjo_ieq(out, "auto"))
        vjo_snprintf(out, cap, "auto");
    return 0;
}

/* ---------------- finding Anki ---------------- */

static int stopped(const VjoAnkiAppOps *ops)
{
    return ops->stopped && ops->stopped(ops->ud);
}

static void status(const VjoAnkiAppOps *ops, const char *msg)
{
    if (ops->status)
        ops->status(ops->ud, msg);
}

static void ip_text(uint32_t ip, char *out, size_t cap)
{
    vjo_snprintf(out, cap, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)(ip >> 16) & 255u,
                 (unsigned)(ip >> 8) & 255u, (unsigned)ip & 255u);
}

/* The local /24 (or smaller subnet), VJO_ANKI_PROBE_MAX hosts at a time;
 * the first that answers as AnkiConnect wins. */
static int search(VjoArena *a, const VjoPlatform *p, const VjoAnkiAppOps *ops, char *host, size_t cap)
{
    uint32_t ip, mask, cand[254];
    uint8_t open[VJO_ANKI_PROBE_MAX];
    char net[16], msg[96];
    int n, rc;
    if (!ops->local_ipv4 || !ops->probe || ops->local_ipv4(ops->ud, &ip, &mask) < 0)
        return VJO_E_NET;
    n = vjo_anki_scan_candidates(ip, mask, cand, 254);
    ip_text(ip & (mask | 0xFFFFFF00u), net, sizeof(net));
    for (int b = 0; b < n; b += VJO_ANKI_PROBE_MAX) {
        int k;
        if (stopped(ops))
            return VJO_E_CANCELLED;
        vjo_snprintf(msg, sizeof(msg), "Searching %s for AnkiConnect… %d of %d", net, b, n);
        status(ops, msg);
        k = ops->probe(ops->ud, cand + b, n - b < VJO_ANKI_PROBE_MAX ? n - b : VJO_ANKI_PROBE_MAX,
                       VJO_ANKI_PORT, VJO_ANKI_SCAN_WINDOW_US, open);
        for (int i = 0; i < k; i++) {
            VjoErr err;
            if (!open[i])
                continue;
            ip_text(cand[b + i], host, cap);
            /* AnkiConnect's own refusal (an apiKey set) is AnkiConnect too: the test says why */
            rc = vjo_anki_probe(a, p, host, VJO_ANKI_PORT, &err);
            if (rc == VJO_OK || rc == VJO_E_ANKI)
                return VJO_OK;
        }
    }
    return VJO_E_NOT_FOUND;
}

int vjo_anki_app_endpoint(VjoArena *a, const VjoPlatform *p, const VjoAnkiAppOps *ops, const char *anki_host,
                          char *host, size_t cap, int *port, int *searched)
{
    char line[80];
    int mode = vjo_anki_endpoint(anki_host, host, cap, port), rc;
    *searched = 0;
    if (mode == VJO_ANKI_MANUAL)
        return VJO_OK;
    if (mode != VJO_ANKI_AUTO)
        return VJO_E_NET;
    /* The saved address, when AnkiConnect still answers there. */
    if (ops->load_host && ops->load_host(ops->ud, line, sizeof(line)) >= 0) {
        size_t n = strlen(line);
        VjoErr err;
        while (n && (unsigned char)line[n - 1] <= ' ')
            line[--n] = '\0';
        if (vjo_anki_endpoint(line, host, cap, port) == VJO_ANKI_MANUAL &&
            ((rc = vjo_anki_probe(a, p, host, *port, &err)) == VJO_OK || rc == VJO_E_ANKI))
            return VJO_OK;
    }
    if ((rc = search(a, p, ops, host, cap)) != VJO_OK)
        return rc;
    *port = VJO_ANKI_PORT;
    *searched = 1;
    vjo_snprintf(line, sizeof(line), "%s:%d\n", host, *port);
    if (ops->save_host)
        ops->save_host(ops->ud, line);
    return VJO_OK;
}

/* ---------------- the connection test ---------------- */

/* Why Anki cannot be reached, for the screen. */
static void unreachable(char *msg, size_t cap, int rc, const char *anki_host, const VjoAnkiTest *t)
{
    if (rc == VJO_E_CANCELLED)
        vjo_snprintf(msg, cap, "Cancelled.");
    else if (!anki_host[0])
        vjo_snprintf(msg, cap, "Set AnkiConnect's address first: your computer's IP, or auto.");
    else if (rc == VJO_E_NOT_FOUND)
        vjo_snprintf(msg, cap, "No AnkiConnect found on this network. In Anki, set AnkiConnect's "
                               "webBindAddress to 0.0.0.0, restart Anki, then test again.");
    else if (!t->host[0])
        vjo_snprintf(msg, cap, "No network address: is Wi-Fi on and connected?");
    else
        vjo_snprintf(msg, cap, "No AnkiConnect at %s:%d. Is Anki open, AnkiConnect's webBindAddress 0.0.0.0, "
                               "and the port allowed in the firewall?", t->host, t->port);
}

/* Is s one of the n strings of array token arr? */
static int has_string(VjoArena *a, const VjoJson *j, int arr, const char *s)
{
    if (!vjo_json_is_type(j, arr, JSMN_ARRAY))
        return 0;
    for (int i = 0, t = arr + 1; i < j->t[arr].size; i++, t = vjo_json_skip(j, t)) {
        size_t mark = vjo_arena_mark(a);
        const char *v = vjo_json_is_type(j, t, JSMN_STRING) ? vjo_json_str(a, j, t) : NULL;
        int same = v && !strcmp(v, s);
        vjo_arena_release(a, mark);
        if (same)
            return 1;
    }
    return 0;
}

int vjo_anki_app_test(VjoArena *a, const VjoPlatform *p, const VjoAnkiAppOps *ops, const VjoConfig *cfg,
                      VjoAnkiTest *t, char *msg, size_t cap)
{
    VjoJson j;
    VjoErr err;
    int r, rc;
    size_t mark = vjo_arena_mark(a), used = 0;
    memset(t, 0, sizeof(*t));
    msg[0] = '\0';
    rc = vjo_anki_app_endpoint(a, p, ops, cfg->anki_host, t->host, sizeof(t->host), &t->port, &t->searched);
    if (rc == VJO_OK)
        rc = vjo_anki_probe(a, p, t->host, t->port, &err);
    if (rc != VJO_OK) {
        unreachable(msg, cap, rc == VJO_E_NOT_FOUND || rc == VJO_E_CANCELLED || rc == VJO_E_NET ? rc : VJO_E_NET,
                    cfg->anki_host, t);
        if (rc == VJO_E_PARSE || rc == VJO_E_STATUS || rc == VJO_E_HTTP)
            vjo_snprintf(msg, cap, "%s:%d answers, but not as AnkiConnect (API version 6): %s", t->host, t->port,
                         vjo_anki_err_text(a, &err));
        else if (rc == VJO_E_ANKI) /* AnkiConnect refuses (an apiKey is set: leave it unset) */
            vjo_snprintf(msg, cap, "AnkiConnect at %s:%d refuses: %s. Leave its apiKey unset.", t->host, t->port,
                         err.detail ? err.detail : "error");
        vjo_arena_release(a, mark);
        return t->rc = rc;
    }
    /* The note type and the fields a card fills. */
    rc = vjo_anki_call(a, p, t->host, t->port,
                       "{\"action\":\"modelFieldNames\",\"version\":6,\"params\":{\"modelName\":\"" VJO_ANKI_NOTE_TYPE
                       "\"}}",
                       &j, &r, &err);
    if (rc == VJO_OK && vjo_json_is_type(&j, r, JSMN_ARRAY)) {
        t->kiku = 1;
        for (int f = 0; f < VJO_KIKU_FIELDS; f++) {
            if (has_string(a, &j, r, vjo_kiku_field[f]))
                continue;
            if (t->missing++ < 4 && used + strlen(vjo_kiku_field[f]) + 3 < sizeof(t->missing_names))
                used += (size_t)vjo_snprintf(t->missing_names + used, sizeof(t->missing_names) - used, "%s%s",
                                             used ? ", " : "", vjo_kiku_field[f]);
        }
    } else if (rc != VJO_E_ANKI) {
        vjo_snprintf(msg, cap, "AnkiConnect at %s:%d stopped answering: %s", t->host, t->port,
                     vjo_anki_err_text(a, &err));
        vjo_arena_release(a, mark);
        return t->rc = rc ? rc : VJO_E_PARSE;
    }
    rc = vjo_anki_call(a, p, t->host, t->port, "{\"action\":\"deckNames\",\"version\":6}", &j, &r, &err);
    t->deck = rc == VJO_OK && has_string(a, &j, r, VJO_ANKI_DECK);
    if (!t->kiku)
        vjo_snprintf(msg, cap, "AnkiConnect OK at %s:%d, but Anki has no Kiku note type: import Kiku_v2.1.0.apkg "
                               "(github.com/youyoumu/kiku), then test again.", t->host, t->port);
    else if (t->missing)
        vjo_snprintf(msg, cap, "AnkiConnect OK at %s:%d, but the Kiku note type lacks %d field%s (%s): update "
                               "Kiku to v2.1.0.", t->host, t->port, t->missing, t->missing > 1 ? "s" : "",
                     t->missing_names);
    else
        vjo_snprintf(msg, cap, "Connected to AnkiConnect at %s:%d%s. Kiku note type OK; deck Kiku %s.", t->host,
                     t->port, t->searched ? " (found and saved)" : "",
                     t->deck ? "exists" : "will be created with the first card");
    vjo_arena_release(a, mark);
    return t->rc = t->kiku ? VJO_OK : VJO_E_ANKI;
}

/* ---------------- sending the queue ---------------- */

/* Like shell/anki.c op_send: the card's tag first, then its audio, then addNote. */
static int send_one(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg, const char *host, int port,
                    VjoAnkiNote *note, VjoAnkiMedia *media, int *no_audio, VjoErr *err)
{
    int found;
    if (vjo_anki_find_queued(a, p, host, port, note->queue_tag, &found, err))
        return err->rc;
    if (found)
        return VJO_OK;
    if (vjo_anki_audio_enabled(cfg)) {
        VjoErr ae;
        uint64_t now = p->unix_time ? p->unix_time(p->ud) * 1000u : 0;
        vjo_anki_find_audio(a, p, cfg, note, now, media, &ae);
        if (ae.rc != VJO_OK && ae.rc != VJO_E_OOM) {
            /* The card waits while the audio source is down, as with △. */
            err->rc = VJO_E_ANKI;
            err->detail = vjo_anki_audio_err_text(a, &ae);
            return AUDIO_DOWN;
        }
        *no_audio |= !media->audio_url;
    }
    return vjo_anki_add(a, p, host, port, note, media, err);
}

int vjo_anki_app_send(VjoArena *a, const VjoPlatform *p, const VjoAnkiAppOps *ops, const VjoConfig *cfg,
                      VjoAnkiSend *s, char *msg, size_t cap)
{
    char id[VJO_QUEUE_ID_SIZE], after[VJO_QUEUE_ID_SIZE] = "", tag[16 + VJO_QUEUE_ID_SIZE], host[64], line[96];
    int port, searched, rc;
    size_t base = vjo_arena_mark(a);
    memset(s, 0, sizeof(*s));
    msg[0] = '\0';
    s->queued = vjo_queue_count();
    if (s->queued < 0) {
        vjo_snprintf(msg, cap, "Cannot read ux0:data/VitaJPOverlay/anki_queue (see log.txt).");
        return s->rc = VJO_E_PARSE;
    }
    if (!s->queued) {
        vjo_snprintf(msg, cap, "No cards queued. × in the overlay saves the word shown.");
        return s->rc = VJO_OK;
    }
    rc = vjo_anki_app_endpoint(a, p, ops, cfg->anki_host, host, sizeof(host), &port, &searched);
    if (rc != VJO_OK) {
        VjoAnkiTest t = {0};
        unreachable(msg, cap, rc, cfg->anki_host, &t);
        return s->rc = rc;
    }
    for (;;) {
        VjoAnkiNote note;
        VjoAnkiMedia media;
        VjoErr err;
        int left;
        vjo_arena_release(a, base);
        if (stopped(ops)) {
            rc = VJO_E_CANCELLED;
            break;
        }
        if ((left = vjo_queue_next(after, id)) < 0) {
            rc = VJO_E_PARSE;
            break;
        }
        if (!id[0]) {
            rc = VJO_OK;
            break;
        }
        vjo_snprintf(line, sizeof(line), "Sending to Anki… %d sent, %d left", s->sent,
                     left - s->unreadable - s->too_large);
        status(ops, line);
        memcpy(after, id, sizeof(after)); /* the next scan starts after it */
        vjo_snprintf(tag, sizeof(tag), "vita_queue_%s", id);
        /* With its screenshot when it has one (a card of an older build); a
         * damaged record or a missing screenshot is kept (log.txt says why). */
        if (vjo_queue_load(a, id, 1, &note, &media) < 0) {
            s->unreadable++;
            continue;
        }
        note.queue_tag = tag;
        {
            /* Room for the request and its exchange, as △ checks. */
            size_t mark = vjo_arena_mark(a);
            VjoAnkiBody body;
            int fits = vjo_anki_add_request(a, &note, &media, &body) == 0 && vjo_arena_free(a) >= 16 * 1024;
            vjo_arena_release(a, mark);
            if (!fits) {
                s->too_large++;
                continue;
            }
        }
        rc = send_one(a, p, cfg, host, port, &note, &media, &s->no_audio, &err);
        if (rc == VJO_E_ANKI_DUPLICATE) {
            if (vjo_queue_archive_duplicate(id) < 0)
                s->unreadable++;
            else
                s->duplicates++;
            continue;
        }
        if (rc != VJO_OK) {
            vjo_snprintf(msg, cap, "%d sent; the rest stay queued: %s%s", s->sent, rc == AUDIO_DOWN ? "audio: " : "",
                         rc == AUDIO_DOWN ? err.detail : vjo_anki_err_text(a, &err));
            break;
        }
        if (vjo_queue_remove(id) < 0) {
            vjo_snprintf(msg, cap, "%d sent; Anki has one more but it stays queued here (sending again is safe).",
                         s->sent);
            rc = VJO_E_PARSE;
            break;
        }
        s->sent++;
    }
    vjo_arena_release(a, base);
    s->rc = rc;
    if (rc == VJO_E_CANCELLED)
        vjo_snprintf(msg, cap, "Stopped: %d sent, the rest stay queued.", s->sent);
    else if (rc == VJO_E_PARSE && !msg[0])
        vjo_snprintf(msg, cap, "%d sent; cannot read the queue, cards kept (see log.txt).", s->sent);
    else if (rc == VJO_OK)
        vjo_snprintf(msg, cap, "Sent %d to Anki (deck Kiku)%s%s%s%s.", s->sent,
                     s->duplicates ? ", some already there" : "",
                     s->unreadable ? ", some kept: unreadable (see log.txt)" : "",
                     s->too_large ? ", some kept: too large" : "", s->no_audio ? ", some without audio" : "");
    return rc;
}
