#include "pspbubble.h"
#include "games.h"
#include <stdint.h>
#include <string.h>

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A path as AdrBubbleBooter keeps it: NUL-terminated inside its field, not
 * empty, with a device ("ux0:"), and no control characters. */
static int copy_path(const char *s, size_t field, char *out, size_t cap)
{
    const char *end = memchr(s, 0, field);
    size_t n = end ? (size_t)(end - s) : 0;
    if (!n || n >= cap || !memchr(s, ':', n)) return -2;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] < 0x20 || s[i] == 0x7F) return -2;
    memcpy(out, s, n);
    out[n] = '\0';
    return 0;
}

int vjo_abb_boot_bin(const void *data, size_t len, char *out, size_t cap)
{
    const unsigned char *p = data;
    if (!p || !out || !cap) return -1;
    out[0] = '\0';
    if (len < 4 || le32(p) != VJO_ABB_MAGIC) return -1;
    size_t at = len == VJO_ABB_BOOT_SIZE ? 0x40 : 0x20;
    if (len <= at) return -2;
    size_t field = len - at < VJO_ABB_PATH_MAX ? len - at : VJO_ABB_PATH_MAX;
    return copy_path((const char *)p + at, field, out, cap);
}

int vjo_abb_boot_inf(const char *text, size_t len, char *out, size_t cap)
{
    char value[VJO_ABB_PATH_MAX];
    int found = 0;
    if (!text || !out || !cap) return -1;
    out[0] = '\0';
    /* bootconv.suprx reads 1 KiB as a C string: it ends at the first NUL. */
    if (len > VJO_ABB_INF_MAX) len = VJO_ABB_INF_MAX;
    const char *nul = memchr(text, 0, len);
    if (nul) len = (size_t)(nul - text);
    /* Lines end at CR or LF; a line that starts with "PATH" (any case; so
     * never a '#' comment) holds the path after its first '=', cut to 255
     * bytes; the last one wins. Nothing is trimmed. */
    for (size_t pos = 0; pos < len;) {
        size_t start = pos;
        while (pos < len && text[pos] != '\r' && text[pos] != '\n') pos++;
        size_t n = pos - start;
        while (pos < len && (text[pos] == '\r' || text[pos] == '\n')) pos++;
        const char *line = text + start, *eq = memchr(line, '=', n);
        if (n < 4 || !eq) continue;
        int match = 1;
        for (int i = 0; i < 4; i++) match &= (line[i] | 0x20) == "path"[i];
        size_t vn = n - (size_t)(eq + 1 - line);
        if (!match) continue;
        if (vn >= sizeof(value)) vn = sizeof(value) - 1;
        memcpy(value, eq + 1, vn);
        value[vn] = '\0';
        found = 1;
    }
    /* "%spspemu%s" with the part after the value's last ':' (bootconv.suprx:97-103). */
    const char *colon = NULL;
    for (const char *c = value; found && *c; c++)
        if (*c == ':') colon = c;
    if (!colon) return -1;
    size_t tail = strlen(colon + 1), head = sizeof("ux0:pspemu") - 1;
    if (!tail || head + tail >= cap) return -1;
    memcpy(out, "ux0:pspemu", head);
    memcpy(out + head, colon + 1, tail + 1);
    for (size_t i = 0; out[i]; i++)
        if ((unsigned char)out[i] < 0x20) { out[0] = '\0'; return -1; }
    return 0;
}

static int one_line(const char *s)
{
    size_t n = strlen(s);
    return !memchr(s, '\r', n) && !memchr(s, '\n', n);
}

int vjo_psp_info_format(char *out, size_t cap, const char *disc, const char *title, const char *path)
{
    if (!out || !cap || !disc || !title || !path) return -1;
    if ((disc[0] && !vjo_game_id_valid(disc)) || !one_line(title) || !one_line(path)) return -1;
    size_t a = strlen(disc), b = strlen(title), c = strlen(path), n = a + b + c + 3;
    if (n >= cap || n >= VJO_PSP_INFO_MAX) return -1;
    char *p = out;
    memcpy(p, disc, a); p += a; *p++ = '\n';
    memcpy(p, title, b); p += b; *p++ = '\n';
    memcpy(p, path, c); p += c; *p++ = '\n';
    *p = '\0';
    return (int)n;
}

char *vjo_psp_info_lines(char *text, char **title, char **path)
{
    char *p = text, **next[2] = {title, path};
    for (int k = 0; k < 3; k++) {
        while (*p && *p != '\n') p++;
        if (*p) *p++ = '\0';
        if (k < 2) *next[k] = p;
    }
    return text;
}
