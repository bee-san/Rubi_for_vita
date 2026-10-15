#include "game_ocr_client.h"
#include "../core/conn.h"
#include <string.h>

/* The kernel answers a submission BUSY while the worker still cleans up the
 * previous job (one cancelled a moment ago: a lookup went first, the screen
 * changed) or a Lens/Anki capture is in flight. That passes in a few ms, so
 * the submission is repeated every SUBMIT_RETRY_US for SUBMIT_BUSY_US; only
 * then is the worker taken for unavailable. */
#define SUBMIT_RETRY_US 20000u
#define SUBMIT_BUSY_US 2000000

int vjo_game_ocr_exchange(const VjoGameOcrClient *client,
                          const VjoGameOcrRequest *request,
                          VjoGameOcrResult *result,
                          int (*cancelled)(void *), void *cancel_ud,
                          int64_t timeout_us)
{
    int rc, seq;
    int64_t start;
    if (!result) return VJO_E_SOURCE;
    memset(result, 0, sizeof(*result));
    if (!client || !request || !client->submit || !client->read ||
        !client->cancel || !client->now_us || !client->delay_us || timeout_us <= 0)
        return VJO_E_SOURCE;
    if (cancelled && cancelled(cancel_ud)) return VJO_E_CANCELLED;
    start = client->now_us(client->ud);
    if (start < 0) return VJO_E_SOURCE;
    while ((seq = client->submit(client->ud, request)) == VJO_ERR_BUSY &&
           client->now_us(client->ud) - start < SUBMIT_BUSY_US) {
        if (cancelled && cancelled(cancel_ud)) return VJO_E_CANCELLED;
        client->delay_us(client->ud, SUBMIT_RETRY_US);
    }
    if (seq <= 0) return VJO_E_OCR_UNAVAILABLE;
    for (;;) {
        if (cancelled && cancelled(cancel_ud)) { rc = VJO_E_CANCELLED; break; }
        int64_t now = client->now_us(client->ud);
        if (now < start || now - start >= timeout_us) { rc = VJO_E_OCR_INFERENCE; break; }
        memset(result, 0, sizeof(*result));
        result->size = sizeof(*result);
        rc = client->read(client->ud, (uint32_t)seq, result);
        if (!rc) {
            if (result->size != sizeof(*result) || result->seq != (uint32_t)seq ||
                !memchr(result->text, 0, sizeof(result->text)) || result->rc > 0 ||
                result->cleanup_status) { rc = VJO_E_OCR_INFERENCE; break; }
            rc = result->rc;
            if (rc) memset(result->text, 0, sizeof(result->text));
            if (cancelled && cancelled(cancel_ud)) {
                memset(result->text, 0, sizeof(result->text));
                return VJO_E_CANCELLED;
            }
            return rc;
        }
        if (rc != 1) { rc = VJO_E_CANCELLED; break; }
        client->delay_us(client->ud, 50000);
    }
    client->cancel(client->ud, (uint32_t)seq);
    memset(result->text, 0, sizeof(result->text));
    return rc;
}
