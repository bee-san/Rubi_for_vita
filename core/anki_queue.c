#include "anki_queue.h"
#include <string.h>
#include "bearssl.h"
#include "json.h"

/* ---------------- encode ---------------- */

static void put_str(VjoBuf *b, const char *s)
{
    vjo_json_write_string(b, s ? s : "");
}

/* An optional string: null when there is none. */
static void put_opt(VjoBuf *b, const char *s)
{
    if (s)
        vjo_json_write_string(b, s);
    else
        vjo_buf_puts(b, "null");
}

char *vjo_queue_encode(VjoArena *a, const VjoAnkiNote *n, int picture)
{
    VjoBuf b;
    if (n->n_meanings < 0 || n->n_meanings > VJO_QUEUE_MAX_MEANINGS || n->n_pos < 0 ||
        n->n_pos > VJO_QUEUE_MAX_POS || n->n_pitch < 0 || n->n_pitch > VJO_MAX_PITCH || !n->spelling ||
        !*n->spelling)
        return NULL;
    vjo_buf_init(&b, a);
    vjo_buf_puts(&b, "{\"version\":2,\"spelling\":");
    put_str(&b, n->spelling);
    vjo_buf_puts(&b, ",\"reading\":");
    put_str(&b, n->reading);
    vjo_buf_puts(&b, ",\"sentence\":");
    put_str(&b, n->sentence);
    vjo_buf_printf(&b, ",\"rank\":%d,\"hl_start\":%d,\"hl_end\":%d,\"picture\":%s,\"meanings\":[",
                   n->rank, n->hl_start, n->hl_end, picture ? "true" : "false");
    for (int i = 0; i < n->n_meanings; i++) {
        if (i)
            vjo_buf_putc(&b, ',');
        put_str(&b, n->meanings[i]);
    }
    vjo_buf_puts(&b, "],\"sentence_furigana\":");
    put_opt(&b, n->sentence_furigana);
    vjo_buf_printf(&b, ",\"sf_hl_start\":%d,\"sf_hl_end\":%d,\"source\":", n->sf_hl_start, n->sf_hl_end);
    put_opt(&b, n->source);
    vjo_buf_puts(&b, ",\"pitch\":[");
    for (int i = 0; i < n->n_pitch; i++)
        vjo_buf_printf(&b, "%s%d", i ? "," : "", n->pitch[i]);
    vjo_buf_puts(&b, "],\"pos\":[");
    for (int i = 0; i < n->n_pos; i++) {
        if (i)
            vjo_buf_putc(&b, ',');
        put_str(&b, n->pos[i]);
    }
    vjo_buf_puts(&b, "],\"misc\":");
    put_opt(&b, n->misc);
    vjo_buf_puts(&b, "}\n");
    return b.oom || b.len > VJO_QUEUE_TEXT_MAX ? NULL : vjo_buf_cstr(&b);
}

/* ---------------- decode ---------------- */

/* An exact integer token (vjo_json_int also accepts fractions). */
static int integer(const VjoJson *j, int t, int *out)
{
    long value;
    if (vjo_json_int(j, t, &value) < 0)
        return -1;
    for (int i = j->t[t].start; i < j->t[t].end; i++)
        if ((j->js[i] < '0' || j->js[i] > '9') && !(i == j->t[t].start && j->js[i] == '-'))
            return -1;
    *out = (int)value;
    return 0;
}

static int number(const VjoJson *j, const char *key, int *out)
{
    return integer(j, vjo_json_obj_get(j, 0, key), out);
}

static const char *string(VjoArena *a, const VjoJson *j, int t)
{
    if (!vjo_json_is_type(j, t, JSMN_STRING))
        return NULL;
    /* Embedded NUL would silently truncate the note on send. */
    for (int i = j->t[t].start; i < j->t[t].end; i++) {
        if ((unsigned char)j->js[i] < 0x20)
            return NULL;
        if (j->js[i] == '\\') {
            if (i + 5 < j->t[t].end && !memcmp(j->js + i, "\\u0000", 6))
                return NULL;
            i++;
        }
    }
    return vjo_json_str(a, j, t);
}

/* An optional string: absent or null = none (0, *out NULL). */
static int opt_string(VjoArena *a, const VjoJson *j, const char *key, const char **out)
{
    int t = vjo_json_obj_get(j, 0, key);
    *out = NULL;
    if (t < 0 || vjo_json_is_null(j, t))
        return 0;
    return (*out = string(a, j, t)) ? 0 : -1;
}

/* An array of strings (absent = none unless required), at most max. */
static int strings(VjoArena *a, const VjoJson *j, const char *key, int max, int required, const char ***out,
                   int *n)
{
    int arr = vjo_json_obj_get(j, 0, key);
    const char **v;
    *n = 0;
    *out = NULL;
    if (arr < 0)
        return required ? -1 : 0;
    if (!vjo_json_is_type(j, arr, JSMN_ARRAY) || j->t[arr].size > max)
        return -1;
    v = (const char **)vjo_arena_alloc(a, sizeof(char *) * (size_t)(j->t[arr].size + 1));
    if (!v)
        return -1;
    for (int i = 0, t = arr + 1; i < j->t[arr].size; i++, t = vjo_json_skip(j, t))
        if (!(v[i] = string(a, j, t)))
            return -1;
    *out = v;
    *n = j->t[arr].size;
    return 0;
}

int vjo_queue_decode(VjoArena *a, const char *json, size_t len, VjoAnkiNote *n, int *picture)
{
    VjoJson j;
    int version, pitch;
    memset(n, 0, sizeof(*n));
    n->sf_hl_start = n->sf_hl_end = -1;
    if (!len || len > VJO_QUEUE_TEXT_MAX || memchr(json, 0, len) ||
        vjo_json_parse(a, json, len, &j) < 0 || !vjo_json_is_type(&j, 0, JSMN_OBJECT) ||
        vjo_json_skip(&j, 0) != j.n || number(&j, "version", &version) || (version != 1 && version != 2) ||
        number(&j, "rank", &n->rank) || number(&j, "hl_start", &n->hl_start) ||
        number(&j, "hl_end", &n->hl_end) ||
        vjo_json_bool(&j, vjo_json_obj_get(&j, 0, "picture"), picture))
        return -1;
    n->spelling = string(a, &j, vjo_json_obj_get(&j, 0, "spelling"));
    n->reading = string(a, &j, vjo_json_obj_get(&j, 0, "reading"));
    n->sentence = string(a, &j, vjo_json_obj_get(&j, 0, "sentence"));
    if (!n->spelling || !*n->spelling || !n->reading || !n->sentence ||
        strings(a, &j, "meanings", VJO_QUEUE_MAX_MEANINGS, 1, &n->meanings, &n->n_meanings))
        return -1;
    if (version == 1)
        return 0;
    /* Version 2: upstream's values. */
    if (opt_string(a, &j, "sentence_furigana", &n->sentence_furigana) ||
        (n->sentence_furigana && (number(&j, "sf_hl_start", &n->sf_hl_start) ||
                                  number(&j, "sf_hl_end", &n->sf_hl_end))) ||
        opt_string(a, &j, "source", &n->source) || opt_string(a, &j, "misc", &n->misc) ||
        strings(a, &j, "pos", VJO_QUEUE_MAX_POS, 0, &n->pos, &n->n_pos))
        return -1;
    pitch = vjo_json_obj_get(&j, 0, "pitch");
    if (pitch >= 0) {
        if (!vjo_json_is_type(&j, pitch, JSMN_ARRAY) || j.t[pitch].size > VJO_MAX_PITCH)
            return -1;
        for (int i = 0, t = pitch + 1; i < j.t[pitch].size; i++, t = vjo_json_skip(&j, t))
            if (integer(&j, t, &n->pitch[i]))
                return -1;
        n->n_pitch = j.t[pitch].size;
    }
    return 0;
}

/* ---------------- IDs ---------------- */

void vjo_queue_id(const char *json, size_t len, char id[VJO_QUEUE_ID_SIZE])
{
    static const char hex[] = "0123456789abcdef";
    br_sha256_context ctx;
    unsigned char digest[32];
    br_sha256_init(&ctx);
    br_sha256_update(&ctx, json, len);
    br_sha256_out(&ctx, digest);
    for (int i = 0; i < 16; i++) {
        id[2 * i] = hex[digest[i] >> 4];
        id[2 * i + 1] = hex[digest[i] & 15];
    }
    id[32] = '\0';
}

int vjo_queue_valid_id(const char *id)
{
    if (strlen(id) != 32)
        return 0;
    for (int i = 0; i < 32; i++)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f')))
            return 0;
    return 1;
}
