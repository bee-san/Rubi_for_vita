#include "client.h"

#include <stddef.h>
#include <string.h>

#include "local_dict.h"
#include "port.h"
#include "textfilter.h"
#include "utf.h"

typedef struct {
    const VjoLensRequest *lr;
    const VjoJpegSource *src;
    uint8_t *chunk;
    uint32_t chunk_len;
} LensBody;

static int write_lens_body(void *ud, VjoConn *c)
{
    LensBody *b = (LensBody *)ud;
    uint32_t off = 0;
    int rc = vjo_conn_send_all(c, b->lr->prefix, b->lr->prefix_len);
    while (rc == VJO_OK && off < b->src->size) {
        uint32_t n = b->src->size - off;
        if (n > b->chunk_len)
            n = b->chunk_len;
        if (b->src->read(b->src->ud, off, b->chunk, n) < 0)
            return VJO_E_SOURCE;
        rc = vjo_conn_send_all(c, b->chunk, n);
        off += n;
    }
    if (rc == VJO_OK)
        rc = vjo_conn_send_all(c, b->lr->suffix, b->lr->suffix_len);
    return rc;
}

int vjo_lens_ocr(VjoArena *a, const VjoPlatform *p, const VjoJpegSource *src,
                 VjoLensResult *res, const char **text, VjoErr *err)
{
    uint8_t rnd[24];
    VjoLensRequest lr;
    LensBody body;
    VjoHttpRequest req;
    VjoHttpResponse resp;
    char *t;

    memset(err, 0, sizeof(*err));
    p->random(p->ud, rnd, sizeof(rnd));
    if (vjo_lens_build_request(a, rnd, src->width, src->height, src->size, &lr) < 0)
        return err->rc = VJO_E_OOM;
    body.lr = &lr;
    body.src = src;
    body.chunk_len = 4096;
    body.chunk = (uint8_t *)vjo_arena_alloc(a, body.chunk_len);
    if (!body.chunk)
        return err->rc = VJO_E_OOM;

    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.host = VJO_LENS_HOST;
    req.path = VJO_LENS_PATH;
    req.content_type = "application/x-protobuf";
    req.extra_headers = "X-Goog-Api-Key: " VJO_LENS_API_KEY "\r\n"
                        "User-Agent: " VJO_LENS_USER_AGENT "\r\n";
    req.body_len = lr.body_len;
    req.write_body = write_lens_body;
    req.ud = &body;

    if (p->log) {
        char line[128];
        vjo_snprintf(line, sizeof(line), "Lens image=%lux%lu jpeg=%lu protobuf=%lu",
                     (unsigned long)src->width, (unsigned long)src->height,
                     (unsigned long)src->size, (unsigned long)lr.body_len);
        p->log(p->ud, line);
    }

    if (vjo_http_request(a, p, 443, 1, &req, VJO_LENS_MAX_RESPONSE, &resp, err))
        return err->rc;
    if (resp.gzip) {
        err->detail = "gzip response";
        return err->rc = VJO_E_PARSE;
    }
    if (vjo_lens_parse_response(a, (const uint8_t *)resp.body, resp.body_len, res) < 0)
        return err->rc = VJO_E_PARSE;
    t = vjo_lens_text(a, res);
    if (!t)
        return err->rc = VJO_E_OOM;
    *text = t;
    return VJO_OK;
}

int vjo_dict_lookup(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg, const char *text,
                    VjoDictResult *res, VjoErr *err)
{
    memset(err, 0, sizeof(*err));
    err->dict = VJO_DICT_LOCAL;
    return vjo_local_lookup(a, p, cfg, text, res, err); /* it clears res */
}

static int is_blank(const char *s)
{
    for (; *s; s++)
        if ((uint8_t)*s > 0x20)
            return 0;
    return 1;
}

/* Sets ocr_text, filtered and sentence (part of the OCR stage). */
static int filter_text(VjoArena *a, const VjoConfig *cfg, const char *ocr_text, VjoOverlayData *out)
{
    char *copy;
    out->ocr_text = ocr_text;
    out->filtered = vjo_filter_lines(a, ocr_text, cfg->non_japanese_filter);
    copy = out->filtered ? vjo_arena_strndup(a, out->filtered, strlen(out->filtered)) : NULL;
    if (!copy) {
        out->failed_stage = VJO_STAGE_OCR;
        return out->err.rc = VJO_E_OOM;
    }
    out->sentence = vjo_java_trim(copy); /* as vjo_entries_build trims the header */
    return VJO_OK;
}

int vjo_overlay_lookup(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg, VjoOverlayData *out)
{
    VjoDictResult jr;
    char *stripped;

    if (!out->filtered) { /* no successful vjo_overlay_ocr */
        out->failed_stage = VJO_STAGE_DICT;
        return out->err.rc = out->err.rc ? out->err.rc : VJO_E_PARSE;
    }
    memset(&jr, 0, sizeof(jr));
    stripped = vjo_strip_newlines(a, out->filtered);
    if (!stripped) {
        out->failed_stage = VJO_STAGE_DICT;
        return out->err.rc = VJO_E_OOM;
    }
    /* Blank text: no lookup, nothing to show. */
    if (!is_blank(stripped)) {
        if (vjo_dict_lookup(a, p, cfg, stripped, &jr, &out->err)) {
            out->failed_stage = VJO_STAGE_DICT;
            memset(&jr, 0, sizeof(jr));
        }
    }
    if (vjo_entries_build(a, out->filtered, &jr, &out->list) < 0) {
        out->failed_stage = VJO_STAGE_DICT;
        return out->err.rc = VJO_E_OOM;
    }
    return out->err.rc;
}

int vjo_overlay_from_text(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                          const char *ocr_text, VjoOverlayData *out)
{
    if (vjo_overlay_ocr_text(a, cfg, ocr_text, out))
        return out->err.rc;
    return vjo_overlay_lookup(a, p, cfg, out);
}

/* One selected local entry, from a raw-sentence point token: highlight bytes
 * are converted only over the matched span so CR/newlines and supplementary
 * codepoints keep correct bytes. */
static int show_local_entry(VjoArena *a, VjoOverlayData *out, const VjoDictResult *result, size_t cursor_byte)
{
    size_t end = cursor_byte, len = strlen(out->sentence);
    VjoEntry *entry;
    int units = 0;
    while (end < len && units < result->tokens[0].len16)
        units += vjo_utf16_units(vjo_utf8_next(out->sentence, len, &end));
    if (units != result->tokens[0].len16) return VJO_E_PARSE;
    entry = vjo_arena_zalloc(a, sizeof(*entry));
    if (!entry) return VJO_E_OOM;
    entry->vocab = &result->vocab[0];
    entry->pos16 = result->tokens[0].pos16;
    entry->hl_start = (int)cursor_byte;
    entry->hl_end = (int)end;
    entry->text = vjo_entry_format(a, entry->vocab, result->source);
    if (!entry->text) return VJO_E_OOM;
    out->list.entries = entry;
    out->list.n_entries = 1;
    out->list.source = result->source;
    return VJO_OK;
}

static void local_lookup_begin(VjoOverlayData *out)
{
    memset(&out->err, 0, sizeof(out->err));
    out->err.dict = VJO_DICT_LOCAL;
    out->failed_stage = VJO_STAGE_NONE;
    out->list = (VjoEntryList){.header = out->sentence ? out->sentence : ""};
}

int vjo_overlay_lookup_at(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                          size_t cursor_byte, VjoOverlayData *out)
{
    size_t mark = vjo_arena_mark(a);
    VjoDictResult result;
    VjoLocalPages *pages = NULL;
    int limit = cfg->local_dictionary_results, rc;

    local_lookup_begin(out);
    out->pages = NULL;
    out->page = 0;
    if (!out->sentence) {
        rc = VJO_E_PARSE;
        goto failed;
    }
    /* Paging keeps only these locators ahead of the shown entry, so a page
     * change releases one entry and reads one payload. */
    if (limit > 1 && !(pages = vjo_arena_zalloc(a, sizeof(*pages)))) {
        rc = VJO_E_OOM;
        goto failed;
    }
    rc = vjo_local_lookup_pages(a, p, cfg, out->sentence, cursor_byte, limit, pages, &result, &out->err);
    if (rc) goto failed;
    if (!result.n_tokens) { /* no match: nothing stays allocated */
        vjo_arena_release(a, mark);
        return VJO_OK;
    }
    if ((rc = show_local_entry(a, out, &result, cursor_byte))) goto failed;
    if (limit > 1) out->pages = pages;
    return VJO_OK;
failed:
    vjo_arena_release(a, mark);
    out->list.entries = NULL;
    out->list.n_entries = 0;
    out->failed_stage = VJO_STAGE_DICT;
    return out->err.rc = rc;
}

int vjo_overlay_lookup_page(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                            int page, VjoOverlayData *out)
{
    VjoLocalPages *pages = out->pages;
    VjoDictResult result;
    size_t keep;
    int rc;

    local_lookup_begin(out);
    if (!pages || !out->sentence ||
        (uint8_t *)pages < a->base || (size_t)((uint8_t *)(pages + 1) - a->base) > a->used ||
        page < 0 || page >= pages->count) {
        out->failed_stage = VJO_STAGE_DICT;
        return out->err.rc = VJO_E_PARSE;
    }
    keep = (size_t)((uint8_t *)(pages + 1) - a->base);
    vjo_arena_release(a, keep); /* the previous page's entry */
    out->page = page;
    rc = vjo_local_read_page(a, p, cfg, pages, page, &result, &out->err);
    if (!rc) rc = show_local_entry(a, out, &result, pages->cursor_byte);
    if (rc) {
        vjo_arena_release(a, keep);
        out->list.entries = NULL;
        out->list.n_entries = 0;
        out->failed_stage = VJO_STAGE_DICT;
        out->err.rc = rc;
    }
    return rc;
}

int vjo_overlay_page_count(const VjoOverlayData *out)
{
    return out->pages ? out->pages->count : out->list.n_entries > 0;
}

int vjo_overlay_ocr_text(VjoArena *a, const VjoConfig *cfg, const char *text, VjoOverlayData *out)
{
    memset(out, 0, sizeof(*out));
    out->list.header = "";
    return filter_text(a, cfg, text, out);
}

int vjo_overlay_ocr(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                    const VjoJpegSource *src, VjoOverlayData *out)
{
    VjoLensResult lr;
    const char *text = NULL;
    memset(out, 0, sizeof(*out));
    out->list.header = "";
    /* This API accepts JPEG for Lens. Local OCR requires the raw-pixel
     * runner; never upload a JPEG when the caller selected local OCR. */
    if (cfg->ocr_backend != VJO_OCR_LENS) {
        out->failed_stage = VJO_STAGE_OCR;
        out->err.detail = "Local OCR requires raw pixels. Use vjo-ocr for host tests.";
        return out->err.rc = VJO_E_OCR_UNAVAILABLE;
    }
    if (vjo_lens_ocr(a, p, src, &lr, &text, &out->err)) {
        out->failed_stage = VJO_STAGE_OCR;
        return out->err.rc;
    }
    return filter_text(a, cfg, text, out);
}

int vjo_overlay_from_jpeg(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                          const VjoJpegSource *src, VjoOverlayData *out)
{
    if (vjo_overlay_ocr(a, p, cfg, src, out))
        return out->err.rc;
    return vjo_overlay_lookup(a, p, cfg, out);
}

void vjo_overlay_warm(const VjoPlatform *p, int timeout_us)
{
    vjo_net_warm(p, VJO_LENS_HOST, 443, timeout_us);
}

const char *vjo_err_text(VjoArena *a, int stage, const VjoErr *err)
{
    /* Lookups are the local dictionaries': only text recognition (Lens) uses the network. */
    const char *what = stage == VJO_STAGE_OCR ? "Text recognition" : VJO_DICT_NAME " lookup";
    VjoBuf b;
    vjo_buf_init(&b, a);
    switch (err->rc) {
    case VJO_OK:
        return "";
    case VJO_E_NET:
        vjo_buf_printf(&b, "%s failed: no network connection", what);
        break;
    case VJO_E_TLS:
        vjo_buf_printf(&b, "%s failed: secure connection error (TLS %d). Check the date/time.",
                       what, err->tls_error);
        break;
    case VJO_E_OOM:
        vjo_buf_printf(&b, "%s failed: not enough memory", what);
        if (err->detail) vjo_buf_printf(&b, ". %s", err->detail);
        break;
    case VJO_E_SOURCE:
        vjo_buf_puts(&b, "Cannot capture the game screen. Close the overlay and try again on a dialogue line.");
        if (err->detail) vjo_buf_printf(&b, " %s", err->detail);
        break;
    case VJO_E_LOCAL_IO:
        vjo_buf_puts(&b, "Cannot read local dictionaries. Check the .vjdict files and local_dictionaries in config.ini.");
        if (err->detail) vjo_buf_printf(&b, " %s", err->detail);
        break;
    case VJO_E_LOCAL_FORMAT:
        vjo_buf_puts(&b, "Invalid or incomplete local dictionary. Reconvert the Yomitan ZIP and copy the complete .vjdict file again.");
        break;
    case VJO_E_OCR_MODEL:
        vjo_buf_puts(&b, err->detail ? err->detail :
                     "Local OCR model is missing or invalid. Run tools/prepare_ocr.py and copy the complete model to ocr_model_dir.");
        break;
    case VJO_E_OCR_REGION:
        vjo_buf_puts(&b, "Local OCR needs a tight dialogue region with horizontal white text. Select a region, excluding portraits and menus.");
        break;
    case VJO_E_OCR_UNAVAILABLE:
        vjo_buf_puts(&b, err->detail ? err->detail : "This build has no local OCR. Install a build with ncnn or set ocr_backend = lens.");
        break;
    case VJO_E_CANCELLED:
        vjo_buf_puts(&b, "Text recognition cancelled.");
        break;
    case VJO_E_SCREEN_CHANGED:
        /* No message (#81 request 4): the Shell never shows such a job; it
         * drops it quietly and tries again (shell/worker.c note_screen_changed). */
        return "";
    case VJO_E_OCR_INFERENCE:
        vjo_buf_puts(&b, "Local OCR failed. Try a smaller dialogue region; see log.txt for details.");
        break;
    case VJO_E_TOO_LARGE:
        if (stage == VJO_STAGE_DICT) {
            vjo_buf_puts(&b, "Local lookup exceeds the text/result limit. Select a smaller OCR region or enable fewer dictionaries.");
            break;
        }
        vjo_buf_printf(&b, "%s failed: response too large", what);
        break;
    case VJO_E_STATUS:
        vjo_buf_printf(&b, "%s failed: HTTP %d", what, err->http_status);
        if (err->detail) {
            vjo_buf_puts(&b, " - ");
            vjo_buf_puts(&b, err->detail);
        }
        break;
    default:
        vjo_buf_printf(&b, "%s failed: unexpected response (%d)", what, err->rc);
        if (err->detail) {
            vjo_buf_puts(&b, " - ");
            vjo_buf_puts(&b, err->detail);
        }
        break;
    }
    return vjo_buf_cstr(&b);
}
