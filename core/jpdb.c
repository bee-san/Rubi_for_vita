#include "jpdb.h"

#include <string.h>

#include "json.h"

char *vjo_jpdb_build_request(VjoArena *a, const char *text)
{
    VjoBuf b;
    vjo_buf_init(&b, a);
    vjo_buf_puts(&b, "{\"text\":");
    vjo_json_write_string(&b, text);
    vjo_buf_puts(&b, ",\"token_fields\":[\"vocabulary_index\",\"position\",\"length\",\"furigana\"]"
                     ",\"position_length_encoding\":\"utf16\""
                     ",\"vocabulary_fields\":[\"vid\",\"sid\",\"rid\",\"spelling\",\"reading\","
                     "\"meanings\"]}");
    return vjo_buf_cstr(&b);
}

static long get_long(const VjoJson *j, int arr, int k, long dflt)
{
    long v;
    if (vjo_json_int(j, vjo_json_arr_get(j, arr, k), &v) == 0)
        return v;
    return dflt;
}

static int parse_vocab(VjoArena *a, const VjoJson *j, int arr, VjoVocab *v)
{
    int m;
    if (!vjo_json_is_type(j, arr, JSMN_ARRAY))
        return -1;
    /* 0-2: vid, sid, rid (requested, unused) */
    v->spelling = vjo_json_str(a, j, vjo_json_arr_get(j, arr, 3));
    v->reading = vjo_json_str(a, j, vjo_json_arr_get(j, arr, 4));
    if (!v->spelling || !v->reading)
        return -1;
    /* Retain the legacy slot only when reading old recorded responses. New
     * requests exclude frequency_rank and place meanings at index 5. */
    v->rank = VJO_NO_RANK;
    m = vjo_json_arr_get(j, arr, j->t[arr].size >= 7 ? 6 : 5);
    v->meanings = NULL;
    v->n_meanings = 0;
    if (vjo_json_is_type(j, m, JSMN_ARRAY) && j->t[m].size > 0) {
        int n = j->t[m].size;
        v->meanings = (const char **)vjo_arena_alloc(a, sizeof(char *) * (size_t)n);
        if (!v->meanings)
            return -1;
        for (int k = 0; k < n; k++) {
            const char *s = vjo_json_str(a, j, vjo_json_arr_get(j, m, k));
            if (!s)
                return -1;
            v->meanings[k] = s;
        }
        v->n_meanings = n;
    }
    return 0;
}

int vjo_jpdb_parse_response(VjoArena *a, const char *json, size_t len, VjoDictResult *out)
{
    VjoJson j;
    int voc, tok, n;

    memset(out, 0, sizeof(*out));
    if (vjo_json_parse(a, json, len, &j) < 0)
        return -1;
    voc = vjo_json_obj_get(&j, 0, "vocabulary");
    if (!vjo_json_is_type(&j, voc, JSMN_ARRAY))
        return -1;
    n = j.t[voc].size;
    if (n > 0) {
        int i = voc + 1;
        out->vocab = (VjoVocab *)vjo_arena_zalloc(a, sizeof(VjoVocab) * (size_t)n);
        if (!out->vocab)
            return -1;
        for (int k = 0; k < n; k++) {
            if (parse_vocab(a, &j, i, &out->vocab[k]) < 0)
                return -1;
            i = vjo_json_skip(&j, i);
        }
    }
    out->n_vocab = n;

    tok = vjo_json_obj_get(&j, 0, "tokens");
    if (vjo_json_is_type(&j, tok, JSMN_ARRAY) && j.t[tok].size > 0) {
        int i = tok + 1, nt = j.t[tok].size, o = 0;
        out->tokens = (VjoToken *)vjo_arena_zalloc(a, sizeof(VjoToken) * (size_t)nt);
        if (!out->tokens)
            return -1;
        for (int k = 0; k < nt; k++) {
            if (vjo_json_is_type(&j, i, JSMN_ARRAY)) {
                long vi = get_long(&j, i, 0, -1);
                VjoToken *t = &out->tokens[o++];
                t->vocab = (vi >= 0 && vi < n) ? (int)vi : -1;
                t->pos16 = (int)get_long(&j, i, 1, 0);
                t->len16 = (int)get_long(&j, i, 2, 0);
            }
            i = vjo_json_skip(&j, i);
        }
        out->n_tokens = o;
    }
    return 0;
}

const char *vjo_jpdb_error_message(VjoArena *a, const char *json, size_t len)
{
    VjoJson j;
    int t;
    if (vjo_json_parse(a, json, len, &j) < 0)
        return NULL;
    t = vjo_json_obj_get(&j, 0, "error_message");
    if (t < 0)
        t = vjo_json_obj_get(&j, 0, "error");
    if (!vjo_json_is_type(&j, t, JSMN_STRING))
        return NULL;
    return vjo_json_str(a, &j, t);
}
