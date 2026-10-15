#include "client.h"

#include <stddef.h>
#include <string.h>

#include "jiten.h"
#include "hachidori.h"
#include "local_dict.h"
#include "jpdb.h"
#include "port.h"
#include "textfilter.h"
#include "utf.h"

typedef struct {
    const char *json;
    size_t len;
} JsonBody;

static int write_json_body(void *ud, VjoConn *c)
{
    JsonBody *b = (JsonBody *)ud;
    return vjo_conn_send_all(c, b->json, b->len);
}

static const VjoDictBackend backends[VJO_DICT_COUNT] = {
    [VJO_DICT_JPDB] = {VJO_JPDB_HOST, VJO_JPDB_PATH, "Authorization: Bearer %s\r\n",
                       vjo_jpdb_build_request, vjo_jpdb_parse_response, vjo_jpdb_error_message},
    [VJO_DICT_JITEN] = {VJO_JITEN_HOST, VJO_JITEN_PATH, "X-Api-Key: %s\r\n",
                        vjo_jiten_build_request, vjo_jiten_parse_response, vjo_jiten_error_message},
};

const VjoDictBackend *vjo_dict_backend(int dictionary)
{
    return &backends[dictionary >= 0 && dictionary < VJO_DICT_COUNT ? dictionary : VJO_DICT_JPDB];
}

int vjo_dict_lookup(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg, const char *text,
                    VjoDictResult *res, VjoErr *err)
{
    const VjoDictBackend *be = vjo_dict_backend(cfg->dictionary);
    const char *key = vjo_config_api_key(cfg);
    VjoHttpRequest req;
    VjoHttpResponse resp;
    JsonBody body;
    char auth[192];

    memset(err, 0, sizeof(*err));
    memset(res, 0, sizeof(*res));
    err->dict = cfg->dictionary;
    if (cfg->dictionary == VJO_DICT_LOCAL)
        return vjo_local_lookup(a, p, cfg, text, res, err);
    if (cfg->dictionary == VJO_DICT_HACHIDORI) {
        int rc = vjo_hachidori_lookup(a, p, cfg, text, res, err);
        if (rc)
            memset(res, 0, sizeof(*res));
        return rc;
    }
    if (!*key)
        return err->rc = VJO_E_NO_KEY;
    body.json = be->build_request(a, text);
    if (!body.json)
        return err->rc = VJO_E_OOM;
    body.len = strlen(body.json);
    vjo_snprintf(auth, sizeof(auth), be->auth_header, key);

    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.host = be->host;
    req.path = be->path;
    req.content_type = "application/json; charset=utf-8";
    req.extra_headers = auth;
    req.body_len = body.len;
    req.write_body = write_json_body;
    req.ud = &body;

    if (vjo_http_request(a, p, 443, 1, &req, VJO_DICT_MAX_RESPONSE, &resp, err)) {
        if (err->rc == VJO_E_STATUS && resp.body)
            err->detail = be->error_message(a, resp.body, resp.body_len);
        return err->rc;
    }
    if (be->parse_response(a, resp.body, resp.body_len, res) < 0)
        return err->rc = VJO_E_PARSE;
    return VJO_OK;
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
    entry->text = vjo_entry_format(a, entry->vocab);
    if (!entry->text) return VJO_E_OOM;
    out->list.entries = entry;
    out->list.n_entries = 1;
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
    if (!out->sentence || cfg->dictionary != VJO_DICT_LOCAL) {
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
    if (!pages || !out->sentence || cfg->dictionary != VJO_DICT_LOCAL ||
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

const char *vjo_err_text(VjoArena *a, int stage, const VjoErr *err)
{
    const VjoDictInfo *di = vjo_dict_info(err->dict);
    const char *dict = di->name, *key = di->key_setting;
    char what[48];
    VjoBuf b;
    if (stage == VJO_STAGE_OCR)
        vjo_snprintf(what, sizeof(what), "Text recognition");
    else
        vjo_snprintf(what, sizeof(what), "%s lookup", dict);
    vjo_buf_init(&b, a);
    switch (err->rc) {
    case VJO_OK:
        return "";
    case VJO_E_NO_KEY:
        vjo_buf_printf(&b, "%s API key is not set. Add %s to ux0:data/VitaJPOverlay/config.ini", dict, key);
        break;
    case VJO_E_NET:
    case VJO_E_NO_HOST:
        if (err->rc == VJO_E_NO_HOST) {
            vjo_buf_puts(&b, "Hachidori relay host is not set or invalid. Set hachidori_host = HOST[:PORT] in "
                             "ux0:data/VitaJPOverlay/config.ini (default port 19633; trusted LAN only).");
            break;
        }
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
        if (stage == VJO_STAGE_DICT && err->dict == VJO_DICT_LOCAL) {
            vjo_buf_puts(&b, "Local lookup exceeds the text/result limit. Select a smaller OCR region or enable fewer dictionaries.");
            break;
        }
        vjo_buf_printf(&b, "%s failed: response too large", what);
        break;
    case VJO_E_STATUS:
        if (stage == VJO_STAGE_DICT && key && (err->http_status == 401 || err->http_status == 403))
            vjo_buf_printf(&b, "%s rejected the API key (HTTP %d). Check %s in config.ini", dict,
                           err->http_status, key);
        else if (stage == VJO_STAGE_DICT && err->http_status == 429)
            vjo_buf_printf(&b, "%s rate limit reached (HTTP 429). Try again shortly.", dict);
        else
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
