#include "game_ocr_client.h"
#include "../core/errors.h"
#include <string.h>

/* Cleanup or idle screenshot-reserve preparation can make Submit BUSY.
 * Retry within the exchange's existing deadline: slow storage must not start
 * a job whose model open would time out while queued behind that preparation. */
#define SUBMIT_RETRY_US 20000u
/* The result is asked for every READ_PERIOD_US. A kernel that waits for it
 * (kernel/game_ocr.c READ_WAIT_US) answers a pending read only after that long,
 * and the result as soon as the game completes it: then no sleep follows. One
 * that answers at once is followed by the rest of the period here, as before. */
#define READ_PERIOD_US 50000

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
    for (;;) {
        if (cancelled && cancelled(cancel_ud)) return VJO_E_CANCELLED;
        int64_t now = client->now_us(client->ud);
        if (now < start || now - start >= timeout_us) return VJO_E_OCR_UNAVAILABLE;
        seq = client->submit(client->ud, request);
        if (seq != VJO_ERR_BUSY) break;
        now = client->now_us(client->ud);
        if (now < start || now - start >= timeout_us) return VJO_E_OCR_UNAVAILABLE;
        int64_t remaining = timeout_us - (now - start);
        client->delay_us(client->ud, remaining < SUBMIT_RETRY_US ? (uint32_t)remaining : SUBMIT_RETRY_US);
    }
    if (seq <= 0) return VJO_E_OCR_UNAVAILABLE;
    for (;;) {
        if (cancelled && cancelled(cancel_ud)) { rc = VJO_E_CANCELLED; break; }
        int64_t now = client->now_us(client->ud);
        if (now < start || now - start >= timeout_us) { rc = VJO_E_OCR_INFERENCE; break; }
        memset(result, 0, sizeof(*result));
        result->size = sizeof(*result);
        rc = client->read(client->ud, (uint32_t)seq, result);
        int64_t spent = client->now_us(client->ud) - now;
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
        if (spent < READ_PERIOD_US / 2) /* the kernel did not wait: never spin */
            client->delay_us(client->ud, (uint32_t)(READ_PERIOD_US - (spent > 0 ? spent : 0)));
    }
    client->cancel(client->ud, (uint32_t)seq);
    memset(result->text, 0, sizeof(result->text));
    return rc;
}
