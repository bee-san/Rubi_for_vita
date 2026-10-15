/* Google Lens over HTTPS: one recognition of a JPEG (core/client.h
 * vjo_lens_ocr). On its own so that the game worker, which posts Lens's
 * requests since the Shell stopped (docs/shell-slim.md), links Lens without the
 * Shell's dictionary clients. */
#include "client.h"

#include <string.h>

#include "port.h"

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
    return vjo_overlay_ocr_text(a, cfg, text, out);
}

int vjo_overlay_from_jpeg(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                          const VjoJpegSource *src, VjoOverlayData *out)
{
    if (vjo_overlay_ocr(a, p, cfg, src, out))
        return out->err.rc;
    return vjo_overlay_lookup(a, p, cfg, out);
}
