#include "svc_api.h"
#include "svc_api_defs.h"
#include "math_crc.h"
#include "hal_systick.h"
#include <string.h>

/* The API dispatcher: framing, CRC, the staged validation of docs/api-v3-spec.md section 4, subscriptions and the
 * per-transport state. It knows no resource: those come from the generated tables (svc_api_tables.c, from
 * tools/api_spec.py) and their handlers (svc_api_res_*.c). This file therefore builds and runs on the host
 * (tests/test_api_core.c). */

typedef struct {
    bool        connected;
    ApiSendFn   send_fn;
    ApiReadyFn  ready_fn;
    uint8_t     active_subs;          /* number of active subscriptions: 0 makes the periodic walks free */
    ApiSub      sub[API2_SUB_SLOTS];
} ApiTransportState;

/* The subscribable resources only (a few dozen of the ~90), found once at init: the per-tick walks must not scan
 * every table row. */
typedef struct { const ApiResource *r; uint8_t cat; } SubResource;
static SubResource s_sub_res[API2_SUB_SLOTS];
static uint8_t     s_sub_res_n;

static ApiTransportState    s_t[API_TRANSPORT_COUNT];
static uint16_t             s_rx_malformed = 0;
static ApiSettingsChangedFn s_settings_changed_fn = 0;

/* ---------------- helpers ---------------- */

static void note_malformed(void)
{
    if (s_rx_malformed < UINT16_MAX) {
        s_rx_malformed++;
    }
}

uint16_t svc_api_rx_malformed(void)   { return s_rx_malformed; }
void     svc_api_clear_counters(void) { s_rx_malformed = 0; }

bool api_transport_connected(ApiTransport t)
{
    return t < API_TRANSPORT_COUNT && s_t[t].connected;
}

bool api_transport_ready(ApiTransport t)
{
    if (t >= API_TRANSPORT_COUNT || !s_t[t].connected) return false;
    return s_t[t].ready_fn == 0 || s_t[t].ready_fn();
}

/* Builds a framed packet [opcode][len][status][data][crc] and hands it to the transport. `urgent` goes straight to
 * the transport's send_fn: true for a direct reply to a request (may use the reserved TX space), false for a
 * subscription push or a bulk chunk (must not). */
static void send_framed(ApiTransport t, uint16_t opcode, Api2Status status,
                        const uint8_t *data, uint16_t data_len, bool urgent)
{
    if (t >= API_TRANSPORT_COUNT)             return;
    if (!s_t[t].connected || !s_t[t].send_fn) return;

    uint16_t payload_len = (uint16_t)(1U + data_len);   /* status byte + data */
    if (payload_len > API2_PACKET_MAX_PAYLOAD) {
        note_malformed();
        return;
    }

    uint8_t buf[API2_PACKET_MAX_SIZE];
    buf[0] = (uint8_t)(opcode & 0xFFU);
    buf[1] = (uint8_t)((opcode >> 8) & 0xFFU);
    buf[2] = (uint8_t)(payload_len & 0xFFU);
    buf[3] = (uint8_t)((payload_len >> 8) & 0xFFU);
    buf[4] = (uint8_t)status;
    if (data && data_len) {
        memcpy(&buf[5], data, data_len);
    }
    uint16_t before_crc = (uint16_t)(API2_PACKET_HDR_BYTES + payload_len);
    uint16_t crc = math_crc16(buf, before_crc);
    buf[before_crc + 0U] = (uint8_t)(crc & 0xFFU);
    buf[before_crc + 1U] = (uint8_t)((crc >> 8) & 0xFFU);

    s_t[t].send_fn(buf, (uint16_t)(before_crc + API2_PACKET_CRC_BYTES), urgent);
}

static void send_response(ApiTransport t, uint16_t opcode, Api2Status status,
                          const uint8_t *data, uint16_t data_len)
{
    send_framed(t, opcode, status, data, data_len, true);
}

void api_send_push(ApiTransport t, uint16_t opcode, const uint8_t *data, uint16_t len)
{
    send_framed(t, opcode, API2_STATUS_OK, data, len, false);
}

/* A subscription push: [issue_seq][page 0][data]. */
static void send_sub_push(ApiTransport t, uint16_t opcode, ApiSub *s, const uint8_t *data, uint16_t len)
{
    uint8_t push[2U + API2_PUSH_DATA_MAX];
    if (len > API2_PUSH_DATA_MAX) {
        note_malformed();
        return;
    }
    push[0] = s->issue_seq++;
    push[1] = 0U;   /* page */
    memcpy(&push[2], data, len);
    api_send_push(t, opcode, push, (uint16_t)(2U + len));
}

/* CRC over the full received frame; checked after category / verb / resource are known to be valid. */
static bool check_crc(ApiTransport t, uint16_t opcode, const uint8_t *frame, uint16_t paylen)
{
    uint16_t before_crc = (uint16_t)(API2_PACKET_HDR_BYTES + paylen);
    uint16_t calc = math_crc16(frame, before_crc);
    uint16_t got  = (uint16_t)(frame[before_crc + 0U] | (frame[before_crc + 1U] << 8));
    if (calc != got) {
        send_response(t, opcode, API2_STATUS_BAD_CRC, 0, 0);
        return false;
    }
    return true;
}

static const ApiCategory *find_category(uint8_t cat)
{
    for (uint8_t i = 0; i < g_api_category_count; ++i) {
        if (g_api_categories[i].id == cat) return &g_api_categories[i];
    }
    return 0;
}

static const ApiResource *find_resource(const ApiCategory *c, uint8_t res)
{
    for (uint8_t i = 0; i < c->count; ++i) {
        if (c->res[i].id == res) return &c->res[i];
    }
    return 0;
}

/* ---------------- subscriptions ---------------- */

static Api2Status subscribe(ApiTransport t, const ApiResource *r, const uint8_t *in, uint16_t len)
{
    ApiSub *s = &s_t[t].sub[r->slot];
    if (r->sub == API2_SUB_INTERVAL) {
        uint32_t interval_ms = (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16)
                             | ((uint32_t)in[3] << 24);
        if (interval_ms < API2_INTERVAL_MIN_MS || interval_ms > API2_INTERVAL_MAX_MS) {
            return API2_STATUS_INVALID_PARAMETER;
        }
        if (!s->active) {
            s->issue_seq = 0;
            s_t[t].active_subs++;
        }
        s->active       = true;
        s->interval_ms  = interval_ms;
        s->last_push_ms = hal_systick_get_ms();
        return API2_STATUS_OK;
    }
    /* event driven: the resource validates the request and arms what it needs */
    if (!s->active) {
        memset(s, 0, sizeof *s);
    }
    Api2Status st = r->ev->start(r, t, s, in, len);
    if (st == API2_STATUS_OK && !s->active) {
        s->active = true;
        s_t[t].active_subs++;
    }
    return st;
}

static Api2Status unsubscribe(ApiTransport t, const ApiResource *r)
{
    ApiSub *s = &s_t[t].sub[r->slot];
    if (!s->active) {
        return API2_STATUS_NOT_SUBSCRIBED;
    }
    if (r->sub == API2_SUB_EVENT && r->ev->stop) {
        r->ev->stop(r, t, s);
    }
    s->active = false;
    if (s_t[t].active_subs > 0U) s_t[t].active_subs--;
    return API2_STATUS_OK;
}

static void clear_subs(ApiTransport t)
{
    if (t >= API_TRANSPORT_COUNT) return;
    for (uint8_t ci = 0; ci < g_api_category_count; ++ci) {
        const ApiCategory *c = &g_api_categories[ci];
        for (uint8_t i = 0; i < c->count; ++i) {
            const ApiResource *r = &c->res[i];
            if (r->sub == API2_SUB_NONE) continue;
            ApiSub *s = &s_t[t].sub[r->slot];
            if (s->active && r->sub == API2_SUB_EVENT && r->ev && r->ev->stop) {
                r->ev->stop(r, t, s);   /* subscriber gone: release whatever it armed */
            }
        }
    }
    memset(s_t[t].sub, 0, sizeof s_t[t].sub);
    s_t[t].active_subs = 0;
}

/* ---------------- dispatch ---------------- */

/* Order of checks (docs/api-v3-spec.md section 4): category known -> verb valid for the category -> resource known
 * -> verb valid for the resource -> CRC -> payload length -> handler. Each failure answers with its own status;
 * nothing after a failed stage runs. */
static void dispatch(ApiTransport t, uint16_t opcode, const uint8_t *frame, uint16_t paylen)
{
    uint8_t verb = API2_OPCODE_VERB(opcode);
    uint8_t cat  = API2_OPCODE_CATEGORY(opcode);
    uint8_t res  = API2_OPCODE_RESOURCE(opcode);

    const ApiCategory *c = find_category(cat);
    if (c == 0) {
        send_response(t, opcode, API2_STATUS_UNKNOWN_CATEGORY, 0, 0);
        return;
    }
    if (verb > (uint8_t)API2_VERB_CANCEL_BULK || (c->verbs & API2_VERB_BIT(verb)) == 0U) {
        send_response(t, opcode, API2_STATUS_VERB_NOT_VALID, 0, 0);
        return;
    }
    const ApiResource *r = find_resource(c, res);
    if (r == 0) {
        send_response(t, opcode, API2_STATUS_UNKNOWN_RESOURCE, 0, 0);
        return;
    }
    if ((r->verbs & API2_VERB_BIT(verb)) == 0U) {
        send_response(t, opcode, API2_STATUS_VERB_NOT_VALID, 0, 0);
        return;
    }
    if (!check_crc(t, opcode, frame, paylen)) return;

    const uint8_t *in = &frame[API2_PACKET_HDR_BYTES];

    /* payload length, then the verb-specific work */
    switch (verb) {
        case API2_VERB_SUBSCRIBE:
            if (paylen != r->sub_in_len) {
                send_response(t, opcode, API2_STATUS_BAD_LENGTH, 0, 0);
                return;
            }
            send_response(t, opcode, subscribe(t, r, in, paylen), 0, 0);
            return;
        case API2_VERB_UNSUBSCRIBE:
            if (paylen != 0U) {
                send_response(t, opcode, API2_STATUS_BAD_LENGTH, 0, 0);
                return;
            }
            send_response(t, opcode, unsubscribe(t, r), 0, 0);
            return;
        case API2_VERB_GET:
        case API2_VERB_START_BULK:
        case API2_VERB_CANCEL_BULK:
            if (paylen != 0U) {
                send_response(t, opcode, API2_STATUS_BAD_LENGTH, 0, 0);
                return;
            }
            break;
        default:   /* SET, EXECUTE */
            if (paylen < r->in_min || paylen > r->in_max) {
                send_response(t, opcode, API2_STATUS_BAD_LENGTH, 0, 0);
                return;
            }
            break;
    }

    uint8_t out[API2_RESPONSE_DATA_MAX];
    ApiCall call = {
        .t = t, .verb = verb, .res = res, .in = in, .in_len = paylen,
        .out = out, .out_cap = (uint16_t)sizeof out, .out_len = 0, .after_reply = 0,
    };
    Api2Status st = r->handler(r, &call);
    if (st == API2_STATUS_OK) {
        send_response(t, opcode, st, out, call.out_len);
    } else {
        send_response(t, opcode, st, 0, 0);
    }
    if (call.after_reply) {
        call.after_reply();
    }
}

/* ---------------- public API ---------------- */

void svc_api_init(void)
{
    memset(s_t, 0, sizeof s_t);
    s_rx_malformed = 0;
    s_sub_res_n = 0;
    for (uint8_t ci = 0; ci < g_api_category_count; ++ci) {
        const ApiCategory *c = &g_api_categories[ci];
        for (uint8_t i = 0; i < c->count; ++i) {
            if (c->res[i].sub != API2_SUB_NONE && s_sub_res_n < API2_SUB_SLOTS) {
                s_sub_res[s_sub_res_n].r   = &c->res[i];
                s_sub_res[s_sub_res_n].cat = c->id;
                s_sub_res_n++;
            }
        }
    }
}

void svc_api_register_transport(ApiTransport t, ApiSendFn send_fn)
{
    if (t >= API_TRANSPORT_COUNT) return;
    s_t[t].send_fn = send_fn;
}

void svc_api_register_transport_ready(ApiTransport t, ApiReadyFn ready_fn)
{
    if (t >= API_TRANSPORT_COUNT) return;
    s_t[t].ready_fn = ready_fn;
}

void svc_api_register_settings_changed(ApiSettingsChangedFn fn)
{
    s_settings_changed_fn = fn;
}

void svc_api_settings_changed(void)
{
    if (s_settings_changed_fn) {
        s_settings_changed_fn();   /* App re-applies (scheduler periods, ...) */
    }
}

void svc_api_connected(ApiTransport t)
{
    if (t >= API_TRANSPORT_COUNT) return;
    s_t[t].connected = true;
    clear_subs(t);
}

void svc_api_disconnected(ApiTransport t)
{
    if (t >= API_TRANSPORT_COUNT) return;
    s_t[t].connected = false;
    clear_subs(t);
    api_res_transport_disconnected(t);
}

void svc_api_receive(ApiTransport t, const uint8_t *data, uint16_t len)
{
    if (t >= API_TRANSPORT_COUNT) return;
    if (data == 0 || len < API2_PACKET_HDR_BYTES + API2_PACKET_CRC_BYTES) {
        note_malformed();
        return;
    }
    uint16_t opcode = (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
    uint16_t paylen = (uint16_t)(data[2] | ((uint16_t)data[3] << 8));
    if ((uint32_t)API2_PACKET_HDR_BYTES + paylen + API2_PACKET_CRC_BYTES > len) {
        note_malformed();
        return;
    }
    dispatch(t, opcode, data, paylen);
}

void svc_api_reassembler_feed_byte(ApiTransport t, ApiByteReassembler *r, uint8_t b)
{
    if (r->pos == 0) {
        r->started_ms = hal_systick_get_ms();
    }
    if (r->pos < API2_PACKET_MAX_SIZE) {
        r->buf[r->pos++] = b;
    }
    if (r->pos >= API2_PACKET_HDR_BYTES) {
        uint16_t paylen = (uint16_t)(r->buf[2] | ((uint16_t)r->buf[3] << 8));
        uint32_t total = (uint32_t)API2_PACKET_HDR_BYTES + paylen + API2_PACKET_CRC_BYTES;
        if (total > API2_PACKET_MAX_SIZE) {
            note_malformed();
            r->pos = 0;
        } else if (r->pos >= total) {
            svc_api_receive(t, r->buf, (uint16_t)total);
            r->pos = 0;
        }
    }
}

void svc_api_reassembler_check_timeout(ApiByteReassembler *r, uint32_t timeout_ms)
{
    if (r->pos > 0 && (hal_systick_get_ms() - r->started_ms) > timeout_ms) {
        r->pos = 0;
    }
}

/* Event-driven pushes (the resource decides when), then the resource side's own periodic work. */
void svc_api_update(void)
{
    api_res_update();

    for (ApiTransport t = 0; t < API_TRANSPORT_COUNT; ++t) {
        if (!s_t[t].connected || s_t[t].active_subs == 0U) continue;
        for (uint8_t k = 0; k < s_sub_res_n; ++k) {
            const ApiResource *r = s_sub_res[k].r;
            if (r->sub != API2_SUB_EVENT) continue;
            ApiSub *s = &s_t[t].sub[r->slot];
            if (!s->active) continue;
            for (uint8_t n = 0; n < r->burst; ++n) {
                if (!api_transport_ready(t)) break;
                uint8_t  data[API2_PUSH_DATA_MAX];
                uint16_t len = 0;
                if (!r->ev->poll(r, t, s, data, &len)) break;
                send_sub_push(t, API2_OPCODE(API2_VERB_SUBSCRIBE, s_sub_res[k].cat, r->id), s, data, len);
            }
        }
    }
}

/* Pushes every interval subscription that is due, built by the resource's GET handler. */
void svc_api_subscriptions_update(void)
{
    uint32_t now = hal_systick_get_ms();
    for (ApiTransport t = 0; t < API_TRANSPORT_COUNT; ++t) {
        if (!s_t[t].connected || s_t[t].active_subs == 0U) continue;
        for (uint8_t k = 0; k < s_sub_res_n; ++k) {
            const ApiResource *r = s_sub_res[k].r;
            if (r->sub != API2_SUB_INTERVAL) continue;
            ApiSub *s = &s_t[t].sub[r->slot];
            if (!s->active || (uint32_t)(now - s->last_push_ms) < s->interval_ms) continue;

            uint8_t out[API2_PUSH_DATA_MAX];
            ApiCall call = {
                .t = t, .verb = API2_VERB_GET, .res = r->id, .in = 0, .in_len = 0,
                .out = out, .out_cap = (uint16_t)sizeof out, .out_len = 0, .after_reply = 0,
            };
            if (r->handler(r, &call) == API2_STATUS_OK) {
                send_sub_push(t, API2_OPCODE(API2_VERB_SUBSCRIBE, s_sub_res[k].cat, r->id), s, out, call.out_len);
            }
            s->last_push_ms = now;
        }
    }
}
