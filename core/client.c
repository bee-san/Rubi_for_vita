#include "client.h"

#include <stddef.h>
#include <string.h>

#include "local_dict.h"
#include "port.h"
#include "textfilter.h"
#include "utf.h"

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

    if (!out->filtered) { /* no recognized text (vjo_overlay_ocr_text) */
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
    entry->conjugation = result->tokens[0].conjugation;
    entry->hl_start = (int)cursor_byte;
    entry->hl_end = (int)end;
    entry->text = vjo_entry_format(a, entry->vocab, result->source);
    if (!entry->text) return VJO_E_OOM;
    out->list.entries = entry;
    out->list.n_entries = 1;
    out->list.source = result->source;
    return VJO_OK;
}

/* The other dictionaries' entries for the shown word, read now and shown below it (VjoEntryList.more):
 * one payload each into the same arena, after page 0's. One that cannot be read (no room left in the
 * arena, a replaced file) is left out with those after it; page 0 stays as it is. */
static void stack_pages(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg, VjoOverlayData *out)
{
    const VjoLocalPages *pages = out->pages;
    const VjoVocab **more;
    if (pages->count < 2 || !(more = vjo_arena_alloc(a, sizeof(*more) * (size_t)(pages->count - 1))))
        return;
    out->list.more = more;
    for (int i = 1; i < pages->count; i++) {
        size_t mark = vjo_arena_mark(a);
        VjoDictResult r;
        VjoErr err;
        if (vjo_local_read_page(a, p, cfg, pages, i, &r, &err) || !r.n_vocab) {
            vjo_arena_release(a, mark);
            return;
        }
        more[out->list.n_more++] = &r.vocab[0];
    }
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
    if (limit > 1) {
        out->pages = pages;
        stack_pages(a, p, cfg, out);
    }
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

const char *vjo_err_text(VjoArena *a, int stage, const VjoErr *err)
{
    /* Cold diagnostics use the data segment's existing room so the normal
     * Shell's full code pages do not grow (docs/memory-ledger.md). */
    static const char memory_unknown[] VJO_DATA_TABLE(error_memory_unknown) = ". Game RAM unavailable (0x%08X)";
    static const char memory_stats[] VJO_DATA_TABLE(error_memory_stats) =
        ". Game RAM (KiB): free %u + reclaim %u; need %u (%u workspace + %u models + %u reserve)";
    static const char capture_error[] VJO_DATA_TABLE(error_capture) =
        "Cannot capture the game screen. Close the overlay and try again on a dialogue line.";
    static const char dictionary_io[] VJO_DATA_TABLE(error_dictionary_io) =
        "Cannot read local dictionaries. Check the .vjdict files and local_dictionaries in config.ini.";
    static const char ocr_region[] VJO_DATA_TABLE(error_ocr_region) =
        "Local OCR needs a tight dialogue region with horizontal white text. Select a region, excluding portraits and menus.";
    /* (and these, for the reading statistics' adaptive time: docs/memory-ledger.md) */
    static const char dictionary_format[] VJO_DATA_TABLE(error_dictionary_format) =
        "Invalid or incomplete local dictionary. Reconvert the Yomitan ZIP and copy the complete .vjdict file again.";
    static const char ocr_model[] VJO_DATA_TABLE(error_ocr_model) =
        "Local OCR model is missing or invalid. Run tools/prepare_ocr.py and copy the complete model to ocr_model_dir.";
    static const char ocr_failed[] VJO_DATA_TABLE(error_ocr_failed) =
        "Local OCR failed. Try a smaller dialogue region; see log.txt for details.";
    /* Lookups are the local dictionaries' and recognition the game worker's: no network. */
    const char *what = stage == VJO_STAGE_OCR ? "Text recognition" : VJO_DICT_NAME " lookup";
    VjoBuf b;
    vjo_buf_init(&b, a);
    switch (err->rc) {
    case VJO_OK:
        return "";
    case VJO_E_OOM:
        vjo_buf_printf(&b, "%s failed: not enough memory", what);
        if (stage == VJO_STAGE_OCR && err->memory.reserve) {
            if (err->memory.free < 0) {
                vjo_buf_printf(&b, memory_unknown, (unsigned)err->memory.free);
            } else {
                /* Free is rounded down; requirements up. Sum in 64 bits before
                 * converting to KiB, including the reserve, which is not used. */
                uint64_t need = (uint64_t)err->memory.workspace + err->memory.models + err->memory.reserve;
                vjo_buf_printf(&b, memory_stats,
                    (unsigned)err->memory.free >> 10, (unsigned)(err->memory.reclaim >> 10),
                    (unsigned)((need + 1023u) >> 10),
                    (unsigned)(((uint64_t)err->memory.workspace + 1023u) >> 10),
                    (unsigned)(((uint64_t)err->memory.models + 1023u) >> 10),
                    (unsigned)(((uint64_t)err->memory.reserve + 1023u) >> 10));
            }
        }
        if (err->detail) vjo_buf_printf(&b, ". %s", err->detail);
        break;
    case VJO_E_SOURCE:
        vjo_buf_puts(&b, capture_error);
        if (err->detail) vjo_buf_printf(&b, " %s", err->detail);
        break;
    case VJO_E_LOCAL_IO:
        vjo_buf_puts(&b, dictionary_io);
        if (err->detail) vjo_buf_printf(&b, " %s", err->detail);
        break;
    case VJO_E_LOCAL_FORMAT:
        vjo_buf_puts(&b, dictionary_format);
        break;
    case VJO_E_OCR_MODEL:
        vjo_buf_puts(&b, err->detail ? err->detail : ocr_model);
        break;
    case VJO_E_OCR_REGION:
        vjo_buf_puts(&b, ocr_region);
        break;
    case VJO_E_OCR_UNAVAILABLE:
        vjo_buf_puts(&b, err->detail ? err->detail : "This build has no local OCR. Install a build made with VJO_WITH_VOCR.");
        break;
    case VJO_E_CANCELLED:
        vjo_buf_puts(&b, "Text recognition cancelled.");
        break;
    case VJO_E_SCREEN_CHANGED:
        /* No message (#81 request 4): the Shell never shows such a job; it
         * drops it quietly and tries again (shell/worker.c note_screen_changed). */
        return "";
    case VJO_E_OCR_INFERENCE:
        vjo_buf_puts(&b, ocr_failed);
        break;
    case VJO_E_TOO_LARGE:
        if (stage == VJO_STAGE_DICT) {
            vjo_buf_puts(&b, "Local lookup exceeds the text/result limit. Select a smaller OCR region or enable fewer dictionaries.");
            break;
        }
        vjo_buf_printf(&b, "%s failed: response too large", what);
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
