#include "anki_queue.h"
#include <stddef.h>
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

/* ,"key":"value" (or null) */
static void put_key(VjoBuf *b, const char *key, const char *s)
{
    vjo_buf_printf(b, ",\"%s\":", key);
    put_opt(b, s);
}

static void put_strings(VjoBuf *b, const char *const *v, int n)
{
    vjo_buf_putc(b, '[');
    for (int i = 0; i < n; i++) {
        if (i)
            vjo_buf_putc(b, ',');
        put_str(b, v[i]);
    }
    vjo_buf_putc(b, ']');
}

/* Version 3's facts about the card: its game and how its text was read. */
static const struct {
    const char *key;
    size_t offset;
} facts[] = {
    {"game", offsetof(VjoAnkiNote, game)},
    {"title_id", offsetof(VjoAnkiNote, title_id)},
    {"platform", offsetof(VjoAnkiNote, platform)},
    {"ocr_engine", offsetof(VjoAnkiNote, ocr_engine)},
    {"ocr_model", offsetof(VjoAnkiNote, ocr_model)},
};

char *vjo_queue_encode(VjoArena *a, const VjoAnkiNote *n, int picture)
{
    VjoBuf b;
    if (n->n_meanings < 0 || n->n_meanings > VJO_QUEUE_MAX_MEANINGS || n->n_pos < 0 ||
        n->n_pos > VJO_QUEUE_MAX_POS || n->n_pitch < 0 || n->n_pitch > VJO_MAX_PITCH || !n->spelling ||
        !*n->spelling || n->n_more < 0 || n->n_more > VJO_ANKI_MAX_MORE)
        return NULL;
    for (int k = 0; k < n->n_more; k++)
        if (n->more[k].n_meanings < 0 || n->more[k].n_meanings > VJO_QUEUE_MAX_MEANINGS)
            return NULL;
    vjo_buf_init(&b, a);
    /* Version 4: rank 0 (VJO_NO_RANK) is "none"; 1-3 wrote 99999 for it. */
    vjo_buf_puts(&b, "{\"version\":4,\"spelling\":");
    put_str(&b, n->spelling);
    vjo_buf_puts(&b, ",\"reading\":");
    put_str(&b, n->reading);
    vjo_buf_puts(&b, ",\"sentence\":");
    put_str(&b, n->sentence);
    vjo_buf_printf(&b, ",\"rank\":%d,\"hl_start\":%d,\"hl_end\":%d,\"picture\":%s,\"meanings\":",
                   n->rank, n->hl_start, n->hl_end, picture ? "true" : "false");
    put_strings(&b, n->meanings, n->n_meanings);
    put_key(&b, "sentence_furigana", n->sentence_furigana);
    vjo_buf_printf(&b, ",\"sf_hl_start\":%d,\"sf_hl_end\":%d", n->sf_hl_start, n->sf_hl_end);
    put_key(&b, "source", n->source);
    vjo_buf_puts(&b, ",\"pitch\":[");
    for (int i = 0; i < n->n_pitch; i++)
        vjo_buf_printf(&b, "%s%d", i ? "," : "", n->pitch[i]);
    vjo_buf_puts(&b, "],\"pos\":");
    put_strings(&b, n->pos, n->n_pos);
    put_key(&b, "misc", n->misc);
    /* Version 3: the rest of what × knows. */
    put_key(&b, "furigana", n->furigana);
    put_key(&b, "dictionary", n->dictionary);
    vjo_buf_puts(&b, ",\"more\":[");
    for (int k = 0; k < n->n_more; k++) {
        const VjoAnkiGloss *g = &n->more[k];
        vjo_buf_puts(&b, k ? ",{\"dictionary\":" : "{\"dictionary\":");
        put_opt(&b, g->dictionary);
        put_key(&b, "spelling", g->spelling);
        put_key(&b, "reading", g->reading);
        vjo_buf_puts(&b, ",\"meanings\":");
        put_strings(&b, g->meanings, g->n_meanings);
        vjo_buf_putc(&b, '}');
    }
    vjo_buf_putc(&b, ']');
    for (unsigned i = 0; i < sizeof(facts) / sizeof(facts[0]); i++)
        put_key(&b, facts[i].key, *(const char *const *)((const char *)n + facts[i].offset));
    /* time: unsigned, until 2106. The recognizer reports no confidence yet:
     * the key is there for when it does. */
    vjo_buf_printf(&b, ",\"time\":%u,\"ocr_confidence\":null}\n", (unsigned)n->unix_time);
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

/* An optional string of object obj: absent or null = none (0, *out NULL). */
static int opt_string(VjoArena *a, const VjoJson *j, int obj, const char *key, const char **out)
{
    int t = vjo_json_obj_get(j, obj, key);
    *out = NULL;
    if (t < 0 || vjo_json_is_null(j, t))
        return 0;
    return (*out = string(a, j, t)) ? 0 : -1;
}

/* An array of strings of object obj (absent = none unless required), at most max. */
static int strings(VjoArena *a, const VjoJson *j, int obj, const char *key, int max, int required,
                   const char ***out, int *n)
{
    int arr = vjo_json_obj_get(j, obj, key);
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

/* Version 3's other dictionaries' entries. */
static int more(VjoArena *a, const VjoJson *j, VjoAnkiNote *n)
{
    int arr = vjo_json_obj_get(j, 0, "more");
    VjoAnkiGloss *g;
    if (arr < 0)
        return 0;
    if (!vjo_json_is_type(j, arr, JSMN_ARRAY) || j->t[arr].size > VJO_ANKI_MAX_MORE ||
        !(g = (VjoAnkiGloss *)vjo_arena_zalloc(a, sizeof(*g) * (size_t)(j->t[arr].size + 1))))
        return -1;
    for (int k = 0, t = arr + 1; k < j->t[arr].size; k++, t = vjo_json_skip(j, t))
        if (!vjo_json_is_type(j, t, JSMN_OBJECT) || opt_string(a, j, t, "dictionary", &g[k].dictionary) ||
            opt_string(a, j, t, "spelling", &g[k].spelling) || opt_string(a, j, t, "reading", &g[k].reading) ||
            strings(a, j, t, "meanings", VJO_QUEUE_MAX_MEANINGS, 1, &g[k].meanings, &g[k].n_meanings))
            return -1;
    n->more = g;
    n->n_more = j->t[arr].size;
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
        vjo_json_skip(&j, 0) != j.n || number(&j, "version", &version) || version < 1 ||
        version > VJO_QUEUE_VERSION || number(&j, "rank", &n->rank) || number(&j, "hl_start", &n->hl_start) ||
        number(&j, "hl_end", &n->hl_end) ||
        vjo_json_bool(&j, vjo_json_obj_get(&j, 0, "picture"), picture))
        return -1;
    if (version < 4 && n->rank == VJO_QUEUE_OLD_NO_RANK)
        n->rank = VJO_NO_RANK; /* versions 1-3's "none"; a version 4 rank of 99999 is one */
    else if (version >= 4 && n->rank < 0)
        return -1;
    n->spelling = string(a, &j, vjo_json_obj_get(&j, 0, "spelling"));
    n->reading = string(a, &j, vjo_json_obj_get(&j, 0, "reading"));
    n->sentence = string(a, &j, vjo_json_obj_get(&j, 0, "sentence"));
    if (!n->spelling || !*n->spelling || !n->reading || !n->sentence ||
        strings(a, &j, 0, "meanings", VJO_QUEUE_MAX_MEANINGS, 1, &n->meanings, &n->n_meanings))
        return -1;
    if (version == 1)
        return version;
    /* Version 2: upstream's values. */
    if (opt_string(a, &j, 0, "sentence_furigana", &n->sentence_furigana) ||
        (n->sentence_furigana && (number(&j, "sf_hl_start", &n->sf_hl_start) ||
                                  number(&j, "sf_hl_end", &n->sf_hl_end))) ||
        opt_string(a, &j, 0, "source", &n->source) || opt_string(a, &j, 0, "misc", &n->misc) ||
        strings(a, &j, 0, "pos", VJO_QUEUE_MAX_POS, 0, &n->pos, &n->n_pos))
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
    if (version == 2)
        return version;
    /* Version 3: what the note needs of the rest. */
    if (opt_string(a, &j, 0, "furigana", &n->furigana) || opt_string(a, &j, 0, "dictionary", &n->dictionary) ||
        more(a, &j, n))
        return -1;
    {
        /* when the card was saved (its note's creation and modification
         * time): digits written with %u, read without a 32-bit long */
        int k = vjo_json_obj_get(&j, 0, "time");
        int64_t t = 0;
        if (k >= 0 && vjo_json_is_type(&j, k, JSMN_PRIMITIVE) && j.t[k].end - j.t[k].start <= 10) {
            for (int i = j.t[k].start; i < j.t[k].end && t >= 0; i++)
                t = json[i] >= '0' && json[i] <= '9' ? t * 10 + (json[i] - '0') : -1;
            if (t > 0)
                n->unix_time = t;
        }
    }
    return version;
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
