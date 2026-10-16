#include "taicfg.h"
#include "games.h"
#include <string.h>

static int space(char c) { return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r' || c == '\n'; }

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }

/* The worker's path in any case, the device's too, with or without a '/'
 * after "ur0:": the card's file system ignores case, and a second line for a
 * spelling taiHEN loads as well would load the worker twice. */
static int is_worker(const char *s, size_t n)
{
    static const char want[] = VJO_TAI_WORKER;
    size_t w = sizeof(want) - 1;
    size_t skip = n > 4 && s[4] == '/' && lower((unsigned char)s[0]) == 'u' && lower((unsigned char)s[1]) == 'r' &&
                  s[2] == '0' && s[3] == ':';
    if (n - skip != w) return 0;
    for (size_t i = 0, j = 0; i < n; i++) {
        if (i == 4 && skip) continue;
        if (lower((unsigned char)s[i]) != lower((unsigned char)want[j++])) return 0;
    }
    return 1;
}

int vjo_tai_title_ok(const char *id)
{
    if (!id) return 0;
    for (int i = 0; i < 9; i++)
        if (!((id[i] >= 'A' && id[i] <= 'Z') || (id[i] >= '0' && id[i] <= '9'))) return 0;
    return !id[9];
}

int vjo_tai_worker_state(const char *text, size_t len, const char *title)
{
    int state = VJO_TAI_ABSENT, record = 0, halt = 0, all = 0;
    if (!text || !vjo_game_id_valid(title)) return VJO_TAI_BLOCKED;
    size_t tn = strlen(title);
    for (size_t pos = 0; pos < len;) {
        /* taiHEN reads the file as a C string. */
        if (!text[pos]) return state ? state : VJO_TAI_BLOCKED;
        size_t start = pos;
        while (pos < len && text[pos] && text[pos] != '\r' && text[pos] != '\n') pos++;
        size_t end = pos;
        if (end - start >= VJO_TAI_LINE_MAX) return state ? state : VJO_TAI_BLOCKED;
        if (pos < len && text[pos]) pos++; /* one CR or LF ends the line */
        while (start < end && space(text[start])) start++;
        while (end > start && space(text[end - 1])) end--;
        if (start == end || text[start] == '#') continue;
        if (text[start] == '*') {
            /* The previous section matched and halts: parsing stops here. */
            if (record && halt) return state ? state : VJO_TAI_BLOCKED;
            size_t at = start + 1;
            while (at < end && space(text[at])) at++;
            halt = at < end && text[at] == '!';
            if (halt)
                for (at++; at < end && space(text[at]); at++);
            size_t n = end - at;
            all = n == 3 && !memcmp(text + at, "ALL", 3);
            record = all || (n == tn && !memcmp(text + at, title, tn));
        } else if (record && is_worker(text + start, end - start)) {
            if (!all) return VJO_TAI_PRESENT;
            state = VJO_TAI_UNDER_ALL;
        }
    }
    /* An appended "*TITLE" line would close a halting section that matched. */
    if (!state && record && halt) return VJO_TAI_BLOCKED;
    return state;
}

int vjo_tai_worker_block(char *out, size_t cap, const char *text, size_t len, const char *title)
{
    static const char note[] = "# VitaJPOverlay: the OCR worker in this Adrenaline bubble's PSP game (Overlay app)";
    if (!out || !vjo_tai_title_ok(title) || (len && !text)) return -1;
    const char *nl = text && memchr(text, '\r', len) ? "\r\n" : "\n";
    size_t nn = strlen(nl), tn = strlen(title), at = 0;
    int lead = len && text[len - 1] != '\n' && text[len - 1] != '\r';
    size_t need = (lead ? nn : 0) + sizeof(note) - 1 + nn + 1 + tn + nn + sizeof(VJO_TAI_WORKER) - 1 + nn;
    int pad = (len + need) % 4096 == 0;
    if (pad) need += nn;
    if (need >= cap) return -1;
#define PUT(s, n) (memcpy(out + at, (s), (n)), at += (n))
    if (lead) PUT(nl, nn);
    PUT(note, sizeof(note) - 1); PUT(nl, nn);
    PUT("*", 1); PUT(title, tn); PUT(nl, nn);
    PUT(VJO_TAI_WORKER, sizeof(VJO_TAI_WORKER) - 1); PUT(nl, nn);
    if (pad) PUT(nl, nn);
#undef PUT
    out[at] = '\0';
    return (int)at;
}
