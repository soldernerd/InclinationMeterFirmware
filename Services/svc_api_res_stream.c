#include "svc_api_res.h"
#include "config.h"
#include "system_state.h"
#include "svc_displacement.h"
#include "svc_log.h"
#include <string.h>

/* API streams: the debug log, the gapless phasor stream and the raw-ADC bulk transfer, plus the periodic hook the
 * dispatcher calls (api_res_update). */

/* ---------------- Debug log (event subscription) ---------------- */

/* s->a = cursor into the log ring (0 = flush whatever backlog is held), s->b = minimum severity. */
static Api2Status log_start(const ApiResource *r, ApiTransport t, ApiSub *s, const uint8_t *in, uint16_t len)
{
    (void)r; (void)t; (void)len;
    if (in[0] > (uint8_t)API2_LOG_ERROR) {
        return API2_STATUS_INVALID_PARAMETER;
    }
    if (!s->active) {
        s->a = 0;
    }
    s->b = in[0];
    return API2_STATUS_OK;
}

static bool log_poll(const ApiResource *r, ApiTransport t, ApiSub *s, uint8_t *out, uint16_t *len)
{
    (void)r; (void)t;
    char    msg[SVC_LOG_MSG_MAX];
    uint8_t mlen = 0;
    Api2LogSeverity sev = API2_LOG_INFO;
    if (!svc_log_drain(&s->a, (Api2LogSeverity)s->b, &sev, msg, &mlen)) {
        return false;
    }
    out[0] = (uint8_t)sev;
    memcpy(&out[1], msg, mlen);
    *len = (uint16_t)(1U + mlen);
    return true;
}

const ApiEventOps api_ev_log = { log_start, 0, log_poll };

Api2Status api_h_debug_log(const ApiResource *r, ApiCall *c)
{
    (void)r; (void)c;
    return API2_STATUS_VERB_NOT_VALID;   /* a stream has no "current value" */
}

/* ---------------- Bulk transfer state: one at a time, device-wide ---------------- */

static struct {
    bool         active;
    enum { BULK_IDLE = 0, BULK_CAPTURING, BULK_SENDING } phase;
    ApiTransport transport;
    uint16_t     send_pos;   /* next sample index to send, during SENDING */
    uint8_t      page;       /* wrapping chunk counter */
} s_bulk;

bool api_bulk_active(void)
{
    return s_bulk.active;
}

static void bulk_abort(void)
{
    svc_displacement_capture_end();
    s_bulk.active = false;
    s_bulk.phase  = BULK_IDLE;
}

Api2Status api_h_bulk_raw_adc(const ApiResource *r, ApiCall *c)
{
    (void)r;
    if (c->verb == API2_VERB_CANCEL_BULK) {
        if (!s_bulk.active) {
            return API2_STATUS_NOTHING_TO_CANCEL;
        }
        bulk_abort();
        svc_log(API2_LOG_INFO, "bulk: cancelled");
        return API2_STATUS_OK;
    }

    /* START_BULK */
    if (s_bulk.active) {
        return API2_STATUS_BUSY_EXCLUSIVE;
    }
    if (!g_system_state.ads_ok) {
        return API2_STATUS_BUSY_RESOURCE;
    }
    if (svc_displacement_is_running()) {
        return API2_STATUS_BUSY_EXCLUSIVE;
    }
    if (svc_displacement_capture_begin() != DRV_OK) {
        return API2_STATUS_BUSY_RESOURCE;
    }
    s_bulk.active    = true;
    s_bulk.phase     = BULK_CAPTURING;
    s_bulk.transport = c->t;
    s_bulk.send_pos  = 0;
    s_bulk.page      = 0;
    svc_log(API2_LOG_INFO, "bulk: raw adc capture started");
    return API2_STATUS_OK;
}

/* Chunk pump: runs every tick while a transfer is active. CAPTURING waits for the RAM buffer to fill; SENDING emits a
 * few chunks per tick, only while the owning transport's TX ring has headroom, so the link is paced to the wire and
 * other traffic still gets a turn. */
static void bulk_pump(void)
{
    if (!s_bulk.active) return;

    ApiTransport t = s_bulk.transport;
    if (!api_transport_connected(t)) {          /* peer vanished mid-transfer */
        bulk_abort();
        return;
    }
    /* the acquisition died while the buffer was still filling (an ADC integrity fault stops it) */
    if (s_bulk.phase == BULK_CAPTURING && !svc_displacement_capture_done() && !svc_displacement_is_running()) {
        svc_log(API2_LOG_ERROR, "bulk: acquisition stopped during the capture -- aborted");
        bulk_abort();
        return;
    }

    if (s_bulk.phase == BULK_CAPTURING) {
        if (!svc_displacement_capture_done()) return;
        uint16_t drops = svc_displacement_capture_drops();
        svc_displacement_capture_end();   /* stop the stream ASAP */
        s_bulk.phase    = BULK_SENDING;
        s_bulk.send_pos = 0;
        s_bulk.page     = 0;
        svc_logf(API2_LOG_INFO, "bulk: raw adc capture full, %u ring overflows", (unsigned)drops);
    }

    const uint8_t *buf   = svc_displacement_capture_buffer();
    const uint16_t total = svc_displacement_capture_sample_count();
    enum { BPS = ADC_BULK_BYTES_PER_SAMPLE };

    for (uint8_t k = 0; k < ADC_BULK_CHUNKS_PER_TICK && s_bulk.send_pos < total; ++k) {
        if (!api_transport_ready(t)) break;

        uint16_t n = (uint16_t)(total - s_bulk.send_pos);
        if (n > ADC_BULK_CHUNK_SAMPLES) n = ADC_BULK_CHUNK_SAMPLES;

        uint8_t payload[1U + ADC_BULK_CHUNK_SAMPLES * BPS];
        payload[0] = s_bulk.page++;
        memcpy(&payload[1], &buf[(size_t)s_bulk.send_pos * BPS], (size_t)n * BPS);
        api_send_push(t, API2_OP_BULK_RAW_ADC_START_BULK, payload, (uint16_t)(1U + (size_t)n * BPS));
        s_bulk.send_pos = (uint16_t)(s_bulk.send_pos + n);
    }

    if (s_bulk.send_pos >= total) {
        svc_logf(API2_LOG_INFO, "bulk: raw adc sent (%u samples)", (unsigned)total);
        s_bulk.active = false;
        s_bulk.phase  = BULK_IDLE;
    }
}

/* ---------------- Phasor stream (event subscription) ---------------- */

/* The subscription arms the ADC tap and the per-batch FIFO in svc_displacement.c; only one subscriber at a time. */
static Api2Status phasor_start(const ApiResource *r, ApiTransport t, ApiSub *s, const uint8_t *in, uint16_t len)
{
    (void)r; (void)t; (void)in; (void)len;
    if (s->active) {
        return API2_STATUS_OK;   /* re-subscribe: nothing to do */
    }
    if (s_bulk.active || !g_system_state.ads_ok) {
        return API2_STATUS_BUSY_EXCLUSIVE;
    }
    if (svc_displacement_phasor_stream_begin() != DRV_OK) {
        return API2_STATUS_BUSY_RESOURCE;
    }
    svc_log(API2_LOG_INFO, "stream: phasor batch stream started");
    return API2_STATUS_OK;
}

static void phasor_stop(const ApiResource *r, ApiTransport t, ApiSub *s)
{
    (void)r; (void)t; (void)s;
    svc_displacement_phasor_stream_end();
    svc_logf(API2_LOG_INFO, "stream: phasor batch stream stopped (%u FIFO drops)",
             (unsigned)svc_displacement_phasor_stream_drops());
}

static bool phasor_poll(const ApiResource *r, ApiTransport t, ApiSub *s, uint8_t *out, uint16_t *len)
{
    (void)r; (void)t; (void)s;
    DisplacementPhasorEntry e;
    if (!svc_displacement_phasor_stream_peek(&e)) {
        return false;
    }
    _Static_assert(sizeof e == sizeof(Api2TopicsPhasorStreamPush), "phasor entry layout == wire layout");
    memcpy(out, &e, sizeof e);
    *len = (uint16_t)sizeof e;
    svc_displacement_phasor_stream_consume();   /* handed over: the frame is built, the ring decides about space */
    return true;
}

const ApiEventOps api_ev_phasor_stream = { phasor_start, phasor_stop, phasor_poll };

/* ---------------- hooks called by the dispatcher ---------------- */

void api_res_update(void)
{
    bulk_pump();
    api_zero_cal_apply_if_ready();
}

void api_res_transport_disconnected(ApiTransport t)
{
    if (s_bulk.active && s_bulk.transport == t) {
        bulk_abort();
    }
}
