#include "anki_queue.h"
#include <string.h>
#include "bearssl.h"
#include "json.h"

char *vjo_queue_encode(VjoArena *a, const VjoAnkiNote *n,
                       const VjoConfig *cfg, const char *source_id)
{
    static const char *const fields[] = {"word", "reading", "furigana", "definition", "sentence", "source"};
    VjoBuf b;
    if (!n || !cfg || !n->spelling || !*n->spelling || !n->reading || !n->sentence ||
        n->n_meanings < 0 || n->n_meanings > 1024 || (n->n_meanings && !n->meanings))
        return NULL;
    vjo_buf_init(&b, a);
    vjo_buf_puts(&b, "{\"version\":2,\"spelling\":");
    vjo_json_write_string(&b, n->spelling);
    vjo_buf_puts(&b, ",\"reading\":"); vjo_json_write_string(&b, n->reading);
    vjo_buf_puts(&b, ",\"sentence\":"); vjo_json_write_string(&b, n->sentence);
    vjo_buf_printf(&b, ",\"hl_start\":%d,\"hl_end\":%d,\"meanings\":[", n->hl_start, n->hl_end);
    for (int i = 0; i < n->n_meanings; i++) {
        if (!n->meanings[i]) return NULL;
        if (i) vjo_buf_putc(&b, ',');
        vjo_json_write_string(&b, n->meanings[i]);
    }
    vjo_buf_puts(&b, "],\"source_id\":"); vjo_json_write_string(&b, source_id ? source_id : "");
    vjo_buf_puts(&b, ",\"deck\":"); vjo_json_write_string(&b, cfg->anki_deck);
    vjo_buf_puts(&b, ",\"note_type\":"); vjo_json_write_string(&b, cfg->anki_note_type);
    vjo_buf_puts(&b, ",\"tags\":"); vjo_json_write_string(&b, cfg->anki_tags);
    vjo_buf_puts(&b, ",\"fields\":{");
    for (int i = 0; i < VJO_ANKI_FIELD_COUNT; i++) {
        if (i) vjo_buf_putc(&b, ',');
        vjo_json_write_string(&b, fields[i]); vjo_buf_putc(&b, ':');
        vjo_json_write_string(&b, cfg->anki_field[i]);
    }
    vjo_buf_puts(&b, "}}");
    return b.oom || b.len > VJO_QUEUE_TEXT_MAX ? NULL : vjo_buf_cstr(&b);
}

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
