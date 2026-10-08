/* Host tests for the API dispatcher (Services/svc_api.c) against a small synthetic resource table: the staged
 * validation order, CRC and length checks, status propagation, response framing, after-reply actions, interval and
 * event subscriptions, per-transport isolation, the byte reassembler.  The real tables are test_api_tables.c. */
#include "test.h"
#include <string.h>

#include "../Math/math_crc.c"
#include "../Services/svc_api_defs.h"   /* only for API2_SUB_SLOTS and the table symbol declarations */

/* ---------------- host stubs ---------------- */
static uint32_t g_ms;
uint32_t hal_systick_get_ms(void) { return g_ms; }

static int g_update_calls, g_disconnect_calls;
void api_res_update(void) { g_update_calls++; }
void api_res_transport_disconnected(ApiTransport t) { (void)t; g_disconnect_calls++; }

/* ---------------- a synthetic table ---------------- */
#define CAT_RW   0x0   /* GET, SET, EXECUTE (everything allowed so the resource verb masks are what is tested) */
#define CAT_RO   0x4   /* GET, SUBSCRIBE, UNSUBSCRIBE */
#define CAT_EVT  0x5   /* SUBSCRIBE, UNSUBSCRIBE */
#define CAT_BLK  0x8   /* START_BULK, CANCEL_BULK */

static unsigned g_handler_calls;
static uint8_t  g_last_verb, g_last_in[8];
static uint16_t g_last_in_len;
static uint32_t g_stored;
static int      g_order_log[8], g_order_n;

static Api2Status h_get_pattern(const ApiResource *r, ApiCall *c)
{
    (void)r; g_handler_calls++; g_last_verb = c->verb;
    for (uint8_t i = 0; i < 8; ++i) c->out[i] = (uint8_t)(0x10 + i);
    c->out_len = 8;
    return API2_STATUS_OK;
}

static Api2Status h_big(const ApiResource *r, ApiCall *c)
{
    (void)r;
    for (uint16_t i = 0; i < API2_RESPONSE_DATA_MAX; ++i) c->out[i] = (uint8_t)i;
    c->out_len = API2_RESPONSE_DATA_MAX;
    return API2_STATUS_OK;
}

static Api2Status h_rw(const ApiResource *r, ApiCall *c)
{
    (void)r; g_handler_calls++; g_last_verb = c->verb; g_last_in_len = c->in_len;
    if (c->verb == API2_VERB_GET) {
        memcpy(c->out, &g_stored, 4);
        c->out_len = 4;
        return API2_STATUS_OK;
    }
    memcpy(g_last_in, c->in, c->in_len > 8 ? 8 : c->in_len);
    if (c->in_len >= 4) memcpy(&g_stored, c->in, 4);
    if (g_stored == 0xBAD0BAD0u) return API2_STATUS_INVALID_PARAMETER;
    if (g_stored == 0xB0B0B0B0u) return API2_STATUS_BUSY_RESOURCE;
    if (g_stored == 0xFFFF0001u) { c->out[0] = 0x77; c->out_len = 1; return API2_STATUS_INVALID_PARAMETER; }
    return API2_STATUS_OK;
}

static void order_after(void) { g_order_log[g_order_n++] = 2; }
static Api2Status h_exec_after(const ApiResource *r, ApiCall *c)
{
    (void)r;
    c->after_reply = order_after;
    return API2_STATUS_OK;
}

static Api2Status h_exec_var(const ApiResource *r, ApiCall *c)
{
    (void)r; g_handler_calls++; g_last_in_len = c->in_len;
    c->out[0] = c->in_len;
    c->out_len = 1;
    return API2_STATUS_OK;
}

static Api2Status h_bulk(const ApiResource *r, ApiCall *c)
{
    (void)r; g_handler_calls++; g_last_verb = c->verb;
    return API2_STATUS_OK;
}

static uint32_t g_counter;
static Api2Status h_counter(const ApiResource *r, ApiCall *c)
{
    (void)r;
    g_counter++;
    memcpy(c->out, &g_counter, 4);
    c->out_len = 4;
    return API2_STATUS_OK;
}

/* event resource: pushes `g_ev_pending` frames of 3 bytes, one per poll */
static int g_ev_pending, g_ev_start_calls, g_ev_stop_calls, g_ev_polls;
static Api2Status ev_start(const ApiResource *r, ApiTransport t, ApiSub *s, const uint8_t *in, uint16_t n)
{
    (void)r; (void)t; (void)s;
    g_ev_start_calls++;
    if (n == 1 && in[0] > 2) return API2_STATUS_INVALID_PARAMETER;
    return API2_STATUS_OK;
}
static void ev_stop(const ApiResource *r, ApiTransport t, ApiSub *s) { (void)r; (void)t; (void)s; g_ev_stop_calls++; }
static bool ev_poll(const ApiResource *r, ApiTransport t, ApiSub *s, uint8_t *out, uint16_t *len)
{
    (void)r; (void)t; (void)s;
    g_ev_polls++;
    if (g_ev_pending <= 0) return false;
    g_ev_pending--;
    out[0] = 0xE0; out[1] = (uint8_t)g_ev_pending; out[2] = 0xE2;
    *len = 3;
    return true;
}
static const ApiEventOps k_ev = { ev_start, ev_stop, ev_poll };

#define V(v) API2_VERB_BIT(v)
static const ApiResource k_rw[] = {
    /* id  verbs                                         sub            subin min max handler        ev ctx slot burst */
    { 0x01, V(API2_VERB_GET),                            API2_SUB_NONE, 4, 0, 0, h_get_pattern, 0, 0, 0, 1 },
    { 0x02, V(API2_VERB_GET) | V(API2_VERB_SET),         API2_SUB_NONE, 4, 4, 4, h_rw,          0, 0, 0, 1 },
    { 0x03, V(API2_VERB_EXECUTE),                        API2_SUB_NONE, 4, 1, 3, h_exec_var,    0, 0, 0, 1 },
    { 0x04, V(API2_VERB_EXECUTE),                        API2_SUB_NONE, 4, 0, 0, h_exec_after,  0, 0, 0, 1 },
    { 0x05, V(API2_VERB_GET),                            API2_SUB_NONE, 4, 0, 0, h_big,         0, 0, 0, 1 },
};
static const ApiResource k_ro[] = {
    { 0x00, V(API2_VERB_GET) | V(API2_VERB_SUBSCRIBE) | V(API2_VERB_UNSUBSCRIBE), API2_SUB_INTERVAL, 4, 0, 0,
      h_counter, 0, 0, 0, 1 },
};
static const ApiResource k_evt[] = {
    { 0x00, V(API2_VERB_SUBSCRIBE) | V(API2_VERB_UNSUBSCRIBE), API2_SUB_EVENT, 1, 0, 0, h_bulk, &k_ev, 0, 1, 3 },
};
static const ApiResource k_blk[] = {
    { 0x00, V(API2_VERB_START_BULK) | V(API2_VERB_CANCEL_BULK), API2_SUB_NONE, 4, 0, 0, h_bulk, 0, 0, 0, 1 },
};
const ApiCategory g_api_categories[] = {
    { CAT_RW,  V(API2_VERB_GET) | V(API2_VERB_SET) | V(API2_VERB_EXECUTE), k_rw,  5 },
    { CAT_RO,  V(API2_VERB_GET) | V(API2_VERB_SUBSCRIBE) | V(API2_VERB_UNSUBSCRIBE), k_ro, 1 },
    { CAT_EVT, V(API2_VERB_SUBSCRIBE) | V(API2_VERB_UNSUBSCRIBE), k_evt, 1 },
    { CAT_BLK, V(API2_VERB_START_BULK) | V(API2_VERB_CANCEL_BULK), k_blk, 1 },
};
const uint8_t g_api_category_count = 4;

#include "../Services/svc_api.c"

/* ---------------- capture of what the device sends ---------------- */
typedef struct { uint8_t b[API2_PACKET_MAX_SIZE]; uint16_t n; bool urgent; } Sent;
static Sent    g_sent[64];
static int     g_nsent;
static int     g_send_t;
static bool    g_ready = true;

static void send_usb(const uint8_t *d, uint16_t n, bool urgent)
{
    if (g_nsent < 64) { memcpy(g_sent[g_nsent].b, d, n); g_sent[g_nsent].n = n; g_sent[g_nsent].urgent = urgent; g_nsent++; }
    g_order_log[g_order_n++] = 1;
}
static void send_ble(const uint8_t *d, uint16_t n, bool urgent) { (void)d; (void)n; (void)urgent; g_send_t++; }
static bool ready_fn(void) { return g_ready; }

static void fresh(void)
{
    svc_api_init();
    svc_api_register_transport(API_TRANSPORT_USB, send_usb);
    svc_api_register_transport(API_TRANSPORT_BLE, send_ble);
    svc_api_register_transport_ready(API_TRANSPORT_USB, ready_fn);
    svc_api_connected(API_TRANSPORT_USB);
    svc_api_connected(API_TRANSPORT_BLE);
    g_nsent = 0; g_send_t = 0; g_ready = true; g_ms = 1000;
    g_handler_calls = 0; g_stored = 0; g_order_n = 0; g_counter = 0;
    g_ev_pending = 0; g_ev_start_calls = g_ev_stop_calls = g_ev_polls = 0;
    g_update_calls = g_disconnect_calls = 0;
    g_last_in_len = 0;
}

/* builds a request frame into buf, returns its length */
static uint16_t req(uint8_t *buf, uint16_t opcode, const void *payload, uint16_t len, bool bad_crc)
{
    buf[0] = (uint8_t)opcode; buf[1] = (uint8_t)(opcode >> 8);
    buf[2] = (uint8_t)len;    buf[3] = (uint8_t)(len >> 8);
    if (len) memcpy(&buf[4], payload, len);
    uint16_t crc = math_crc16(buf, (uint16_t)(4 + len));
    if (bad_crc) crc ^= 0x5A5A;
    buf[4 + len] = (uint8_t)crc; buf[5 + len] = (uint8_t)(crc >> 8);
    return (uint16_t)(6 + len);
}

static Api2Status send(uint16_t opcode, const void *payload, uint16_t len, bool bad_crc)
{
    uint8_t f[API2_PACKET_MAX_SIZE];
    int before = g_nsent;
    uint16_t n = req(f, opcode, payload, len, bad_crc);
    svc_api_receive(API_TRANSPORT_USB, f, n);
    if (g_nsent == before) return (Api2Status)0xFF;     /* no response at all */
    return (Api2Status)g_sent[g_nsent - 1].b[4];
}

static const Sent *last(void) { return &g_sent[g_nsent - 1]; }

#define OP(verb, cat, res) API2_OPCODE(verb, cat, res)

/* ---------------- tests ---------------- */

TEST(an_unknown_category_is_refused_before_the_crc_is_even_looked_at)
{
    fresh();
    CHECK_EQ(send(OP(API2_VERB_GET, 0xC, 0x00), 0, 0, true), API2_STATUS_UNKNOWN_CATEGORY);
    CHECK_EQ(g_handler_calls, 0);
}

TEST(the_verb_is_checked_against_the_category_before_the_resource_is_looked_up)
{
    fresh();
    /* EXECUTE is not valid in CAT_RO: VERB_NOT_VALID even though resource 0x77 does not exist either */
    CHECK_EQ(send(OP(API2_VERB_EXECUTE, CAT_RO, 0x77), 0, 0, false), API2_STATUS_VERB_NOT_VALID);
    CHECK_EQ(send(OP(API2_VERB_GET, CAT_BLK, 0x00), 0, 0, false), API2_STATUS_VERB_NOT_VALID);
    CHECK_EQ(send(OP(0xF, CAT_RW, 0x01), 0, 0, false), API2_STATUS_VERB_NOT_VALID);   /* a verb that does not exist */
}

TEST(an_unknown_resource_is_refused)
{
    fresh();
    CHECK_EQ(send(OP(API2_VERB_GET, CAT_RW, 0x77), 0, 0, false), API2_STATUS_UNKNOWN_RESOURCE);
}

TEST(the_verb_is_checked_against_the_resource_before_the_crc)
{
    fresh();
    /* resource 0x01 is GET only; SET with a bad CRC still says VERB_NOT_VALID */
    CHECK_EQ(send(OP(API2_VERB_SET, CAT_RW, 0x01), "abcd", 4, true), API2_STATUS_VERB_NOT_VALID);
    CHECK_EQ(g_handler_calls, 0);
}

TEST(a_bad_crc_stops_before_the_length_check_and_the_handler)
{
    fresh();
    CHECK_EQ(send(OP(API2_VERB_GET, CAT_RW, 0x01), 0, 0, true), API2_STATUS_BAD_CRC);
    CHECK_EQ(send(OP(API2_VERB_SET, CAT_RW, 0x02), "ab", 2, true), API2_STATUS_BAD_CRC);   /* wrong length AND bad crc */
    CHECK_EQ(g_handler_calls, 0);
}

TEST(payload_lengths_are_enforced_per_verb)
{
    fresh();
    CHECK_EQ(send(OP(API2_VERB_GET, CAT_RW, 0x01), "x", 1, false), API2_STATUS_BAD_LENGTH);            /* GET takes none */
    CHECK_EQ(send(OP(API2_VERB_SET, CAT_RW, 0x02), "abc", 3, false), API2_STATUS_BAD_LENGTH);          /* exactly 4 */
    CHECK_EQ(send(OP(API2_VERB_SET, CAT_RW, 0x02), "abcde", 5, false), API2_STATUS_BAD_LENGTH);
    CHECK_EQ(send(OP(API2_VERB_EXECUTE, CAT_RW, 0x03), 0, 0, false), API2_STATUS_BAD_LENGTH);          /* 1..3 */
    CHECK_EQ(send(OP(API2_VERB_EXECUTE, CAT_RW, 0x03), "abcd", 4, false), API2_STATUS_BAD_LENGTH);
    CHECK_EQ(send(OP(API2_VERB_EXECUTE, CAT_RW, 0x03), "a", 1, false), API2_STATUS_OK);
    CHECK_EQ(last()->b[5], 1);
    CHECK_EQ(send(OP(API2_VERB_EXECUTE, CAT_RW, 0x03), "abc", 3, false), API2_STATUS_OK);
    CHECK_EQ(last()->b[5], 3);
    CHECK_EQ(send(OP(API2_VERB_START_BULK, CAT_BLK, 0x00), "x", 1, false), API2_STATUS_BAD_LENGTH);
    CHECK_EQ(g_handler_calls, 2);                        /* only the two valid EXECUTEs reached a handler */
}

TEST(a_get_response_is_status_plus_data_echoes_the_opcode_and_is_urgent)
{
    fresh();
    uint16_t op = OP(API2_VERB_GET, CAT_RW, 0x01);
    CHECK_EQ(send(op, 0, 0, false), API2_STATUS_OK);
    const Sent *s = last();
    CHECK_EQ(s->n, 6 + 1 + 8);
    CHECK_EQ(s->b[0] | (s->b[1] << 8), op);
    CHECK_EQ(s->b[2] | (s->b[3] << 8), 9);               /* LEN = status + 8 data bytes */
    CHECK_EQ(s->b[5], 0x10);
    CHECK_EQ(s->b[12], 0x17);
    uint16_t crc = math_crc16(s->b, (uint16_t)(s->n - 2));
    CHECK_EQ(s->b[s->n - 2] | (s->b[s->n - 1] << 8), crc);
    CHECK(s->urgent);
}

TEST(a_handler_error_status_carries_no_data)
{
    fresh();
    uint32_t v = 0xBAD0BAD0u;
    CHECK_EQ(send(OP(API2_VERB_SET, CAT_RW, 0x02), &v, 4, false), API2_STATUS_INVALID_PARAMETER);
    CHECK_EQ(last()->n, 6 + 1);
    v = 0xFFFF0001u;                                     /* handler wrote data AND returned an error */
    CHECK_EQ(send(OP(API2_VERB_SET, CAT_RW, 0x02), &v, 4, false), API2_STATUS_INVALID_PARAMETER);
    CHECK_EQ(last()->n, 6 + 1);
    v = 0xB0B0B0B0u;
    CHECK_EQ(send(OP(API2_VERB_SET, CAT_RW, 0x02), &v, 4, false), API2_STATUS_BUSY_RESOURCE);
}

TEST(set_and_get_round_trip_through_a_handler)
{
    fresh();
    uint32_t v = 0x12345678u;
    CHECK_EQ(send(OP(API2_VERB_SET, CAT_RW, 0x02), &v, 4, false), API2_STATUS_OK);
    CHECK_EQ(last()->n, 6 + 1);                          /* SET answers status only */
    CHECK_EQ(g_last_verb, API2_VERB_SET);
    CHECK_EQ(send(OP(API2_VERB_GET, CAT_RW, 0x02), 0, 0, false), API2_STATUS_OK);
    uint32_t got; memcpy(&got, &last()->b[5], 4);
    CHECK_EQ(got, 0x12345678u);
}

TEST(the_largest_possible_response_fits_exactly)
{
    fresh();
    CHECK_EQ(send(OP(API2_VERB_GET, CAT_RW, 0x05), 0, 0, false), API2_STATUS_OK);
    CHECK_EQ(last()->n, API2_PACKET_MAX_SIZE);
    CHECK_EQ(svc_api_rx_malformed(), 0);
}

TEST(an_after_reply_action_runs_only_after_the_response_was_sent)
{
    fresh();
    CHECK_EQ(send(OP(API2_VERB_EXECUTE, CAT_RW, 0x04), 0, 0, false), API2_STATUS_OK);
    CHECK_EQ(g_order_n, 2);
    CHECK_EQ(g_order_log[0], 1);                         /* the transport got the frame first ... */
    CHECK_EQ(g_order_log[1], 2);                         /* ... then the action ran */
}

TEST(bulk_verbs_reach_the_handler_with_the_right_verb)
{
    fresh();
    CHECK_EQ(send(OP(API2_VERB_START_BULK, CAT_BLK, 0x00), 0, 0, false), API2_STATUS_OK);
    CHECK_EQ(g_last_verb, API2_VERB_START_BULK);
    CHECK_EQ(send(OP(API2_VERB_CANCEL_BULK, CAT_BLK, 0x00), 0, 0, false), API2_STATUS_OK);
    CHECK_EQ(g_last_verb, API2_VERB_CANCEL_BULK);
}

/* ---------------- interval subscriptions ---------------- */

static void sub_interval(uint32_t ms, Api2Status want)
{
    CHECK_EQ(send(OP(API2_VERB_SUBSCRIBE, CAT_RO, 0x00), &ms, 4, false), want);
}

TEST(interval_subscribe_validates_the_range)
{
    fresh();
    sub_interval(49, API2_STATUS_INVALID_PARAMETER);
    sub_interval(3600001u, API2_STATUS_INVALID_PARAMETER);
    sub_interval(50, API2_STATUS_OK);
    sub_interval(3600000u, API2_STATUS_OK);
    CHECK_EQ(send(OP(API2_VERB_SUBSCRIBE, CAT_RO, 0x00), "ab", 2, false), API2_STATUS_BAD_LENGTH);
}

TEST(an_interval_subscription_pushes_when_due_with_a_wrapping_issue_counter)
{
    fresh();
    sub_interval(100, API2_STATUS_OK);
    int base = g_nsent;
    svc_api_subscriptions_update();                      /* not due yet */
    CHECK_EQ(g_nsent, base);
    g_ms += 99; svc_api_subscriptions_update();
    CHECK_EQ(g_nsent, base);
    g_ms += 1;  svc_api_subscriptions_update();
    CHECK_EQ(g_nsent, base + 1);
    const Sent *s = last();
    CHECK_EQ(s->b[0] | (s->b[1] << 8), OP(API2_VERB_SUBSCRIBE, CAT_RO, 0x00));   /* pushes use the SUBSCRIBE opcode */
    CHECK_EQ(s->b[4], API2_STATUS_OK);
    CHECK_EQ(s->b[5], 0);                                /* issue_seq */
    CHECK_EQ(s->b[6], 0);                                /* page */
    uint32_t v; memcpy(&v, &s->b[7], 4);
    CHECK_EQ(v, 1);
    CHECK(!s->urgent);                                   /* pushes must leave the TX reserve free */
    g_ms += 100; svc_api_subscriptions_update();
    CHECK_EQ(last()->b[5], 1);
    for (int i = 0; i < 300; ++i) { g_nsent = 0; g_ms += 100; svc_api_subscriptions_update(); }
    CHECK_EQ(last()->b[5], (uint8_t)(301 & 0xFF));       /* the counter wrapped, nothing stopped */
}

TEST(resubscribing_changes_the_interval_without_restarting_the_issue_counter)
{
    fresh();
    sub_interval(100, API2_STATUS_OK);
    g_ms += 100; svc_api_subscriptions_update();
    g_ms += 100; svc_api_subscriptions_update();
    CHECK_EQ(last()->b[5], 1);
    sub_interval(1000, API2_STATUS_OK);
    g_ms += 1000; svc_api_subscriptions_update();
    CHECK_EQ(last()->b[5], 2);
}

TEST(unsubscribe_stops_the_pushes_and_a_second_one_says_not_subscribed)
{
    fresh();
    CHECK_EQ(send(OP(API2_VERB_UNSUBSCRIBE, CAT_RO, 0x00), 0, 0, false), API2_STATUS_NOT_SUBSCRIBED);
    sub_interval(100, API2_STATUS_OK);
    CHECK_EQ(send(OP(API2_VERB_UNSUBSCRIBE, CAT_RO, 0x00), 0, 0, false), API2_STATUS_OK);
    int n = g_nsent;
    g_ms += 1000; svc_api_subscriptions_update();
    CHECK_EQ(g_nsent, n);
    CHECK_EQ(send(OP(API2_VERB_UNSUBSCRIBE, CAT_RO, 0x00), 0, 0, false), API2_STATUS_NOT_SUBSCRIBED);
}

TEST(subscriptions_are_per_transport_and_a_reconnect_clears_them)
{
    fresh();
    sub_interval(100, API2_STATUS_OK);                   /* USB only */
    g_ms += 100; svc_api_subscriptions_update();
    CHECK_EQ(g_send_t, 0);                               /* BLE never subscribed */
    svc_api_disconnected(API_TRANSPORT_USB);
    CHECK_EQ(g_disconnect_calls, 1);
    svc_api_connected(API_TRANSPORT_USB);
    int n = g_nsent;
    g_ms += 1000; svc_api_subscriptions_update();
    CHECK_EQ(g_nsent, n);                                /* the old subscription is gone */
}

TEST(nothing_is_sent_to_a_disconnected_transport)
{
    fresh();
    svc_api_disconnected(API_TRANSPORT_USB);
    int n = g_nsent;
    uint8_t f[16];
    uint16_t len = req(f, OP(API2_VERB_GET, CAT_RW, 0x01), 0, 0, false);
    svc_api_receive(API_TRANSPORT_USB, f, len);
    CHECK_EQ(g_nsent, n);
}

/* ---------------- event subscriptions ---------------- */

static Api2Status sub_event(uint8_t arg)
{
    return send(OP(API2_VERB_SUBSCRIBE, CAT_EVT, 0x00), &arg, 1, false);
}

TEST(an_event_subscription_is_started_by_the_resource_and_can_be_refused)
{
    fresh();
    CHECK_EQ(sub_event(9), API2_STATUS_INVALID_PARAMETER);        /* start() said no */
    CHECK_EQ(g_ev_start_calls, 1);
    CHECK_EQ(send(OP(API2_VERB_UNSUBSCRIBE, CAT_EVT, 0x00), 0, 0, false), API2_STATUS_NOT_SUBSCRIBED);
    CHECK_EQ(sub_event(1), API2_STATUS_OK);
    CHECK_EQ(g_ev_start_calls, 2);
    CHECK_EQ(send(OP(API2_VERB_SUBSCRIBE, CAT_EVT, 0x00), 0, 0, false), API2_STATUS_BAD_LENGTH);
}

TEST(event_pushes_are_limited_per_tick_by_burst_and_by_transmit_headroom)
{
    fresh();
    CHECK_EQ(sub_event(1), API2_STATUS_OK);
    g_ev_pending = 8;
    int n = g_nsent;
    svc_api_update();
    CHECK_EQ(g_nsent - n, 3);                            /* burst = 3 */
    CHECK_EQ(g_update_calls, 1);
    const Sent *s = last();
    CHECK_EQ(s->b[0] | (s->b[1] << 8), OP(API2_VERB_SUBSCRIBE, CAT_EVT, 0x00));
    CHECK_EQ(s->b[5], 2);                                /* issue_seq counts the frames: 0, 1, 2 */
    CHECK_EQ(s->b[6], 0);
    CHECK_EQ(s->b[7], 0xE0);
    CHECK(!s->urgent);
    g_ready = false;
    n = g_nsent;
    svc_api_update();
    CHECK_EQ(g_nsent, n);                                /* no headroom: nothing pushed, nothing lost */
    CHECK_EQ(g_ev_pending, 5);
    g_ready = true;
    svc_api_update(); svc_api_update();
    CHECK_EQ(g_ev_pending, 0);
    n = g_nsent;
    svc_api_update();
    CHECK_EQ(g_nsent, n);                                /* poll() said nothing is due */
}

TEST(unsubscribing_or_disconnecting_stops_an_event_resource)
{
    fresh();
    CHECK_EQ(sub_event(1), API2_STATUS_OK);
    CHECK_EQ(send(OP(API2_VERB_UNSUBSCRIBE, CAT_EVT, 0x00), 0, 0, false), API2_STATUS_OK);
    CHECK_EQ(g_ev_stop_calls, 1);
    CHECK_EQ(sub_event(1), API2_STATUS_OK);
    svc_api_disconnected(API_TRANSPORT_USB);
    CHECK_EQ(g_ev_stop_calls, 2);                        /* released when the subscriber vanishes */
    svc_api_connected(API_TRANSPORT_USB);
    svc_api_connected(API_TRANSPORT_USB);
    CHECK_EQ(g_ev_stop_calls, 2);                        /* nothing active, nothing to stop */
}

TEST(an_event_resource_is_polled_only_while_subscribed)
{
    fresh();
    g_ev_pending = 5;
    svc_api_update();
    CHECK_EQ(g_ev_polls, 0);
}

/* ---------------- framing ---------------- */

TEST(short_and_inconsistent_frames_count_as_malformed_and_get_no_answer)
{
    fresh();
    uint8_t f[16] = {0};
    svc_api_receive(API_TRANSPORT_USB, f, 3);
    CHECK_EQ(svc_api_rx_malformed(), 1);
    req(f, OP(API2_VERB_GET, CAT_RW, 0x01), 0, 0, false);
    f[2] = 9;                                            /* declares 9 payload bytes but only 6 arrive */
    svc_api_receive(API_TRANSPORT_USB, f, 6);
    CHECK_EQ(svc_api_rx_malformed(), 2);
    CHECK_EQ(g_nsent, 0);
    svc_api_clear_counters();
    CHECK_EQ(svc_api_rx_malformed(), 0);
}

TEST(the_reassembler_builds_packets_from_single_bytes_and_chunks)
{
    fresh();
    ApiByteReassembler r; memset(&r, 0, sizeof r);
    uint8_t f[32];
    uint16_t n1 = req(f, OP(API2_VERB_GET, CAT_RW, 0x01), 0, 0, false);
    for (uint16_t i = 0; i < n1; ++i) svc_api_reassembler_feed_byte(API_TRANSPORT_USB, &r, f[i]);
    CHECK_EQ(g_nsent, 1);
    CHECK_EQ(r.pos, 0);
    /* two packets back to back */
    uint8_t two[64];
    uint16_t a = req(two, OP(API2_VERB_GET, CAT_RW, 0x01), 0, 0, false);
    uint16_t b = req(&two[a], OP(API2_VERB_GET, CAT_RW, 0x05), 0, 0, false);
    for (uint16_t i = 0; i < a + b; ++i) svc_api_reassembler_feed_byte(API_TRANSPORT_USB, &r, two[i]);
    CHECK_EQ(g_nsent, 3);
}

TEST(the_reassembler_drops_an_oversized_declaration_and_times_out_a_stalled_packet)
{
    fresh();
    ApiByteReassembler r; memset(&r, 0, sizeof r);
    uint8_t hdr[4] = { 0x01, 0x00, 0xFF, 0x00 };         /* LEN 255 > the packet limit */
    for (int i = 0; i < 4; ++i) svc_api_reassembler_feed_byte(API_TRANSPORT_USB, &r, hdr[i]);
    CHECK_EQ(r.pos, 0);
    CHECK_EQ(svc_api_rx_malformed(), 1);

    g_ms = 5000;
    svc_api_reassembler_feed_byte(API_TRANSPORT_USB, &r, 0x01);
    svc_api_reassembler_feed_byte(API_TRANSPORT_USB, &r, 0x00);
    CHECK_EQ(r.pos, 2);
    g_ms += 100; svc_api_reassembler_check_timeout(&r, 250);
    CHECK_EQ(r.pos, 2);
    g_ms += 200; svc_api_reassembler_check_timeout(&r, 250);
    CHECK_EQ(r.pos, 0);
}

static int g_changed;
static void changed_cb(void) { g_changed++; }

TEST(the_settings_changed_hook_is_called_when_asked)
{
    fresh();
    svc_api_settings_changed();                          /* nothing registered: harmless */
    svc_api_register_settings_changed(changed_cb);
    svc_api_settings_changed();
    CHECK_EQ(g_changed, 1);
    svc_api_register_settings_changed(0);
}

int main(void)
{
    RUN(an_unknown_category_is_refused_before_the_crc_is_even_looked_at);
    RUN(the_verb_is_checked_against_the_category_before_the_resource_is_looked_up);
    RUN(an_unknown_resource_is_refused);
    RUN(the_verb_is_checked_against_the_resource_before_the_crc);
    RUN(a_bad_crc_stops_before_the_length_check_and_the_handler);
    RUN(payload_lengths_are_enforced_per_verb);
    RUN(a_get_response_is_status_plus_data_echoes_the_opcode_and_is_urgent);
    RUN(a_handler_error_status_carries_no_data);
    RUN(set_and_get_round_trip_through_a_handler);
    RUN(the_largest_possible_response_fits_exactly);
    RUN(an_after_reply_action_runs_only_after_the_response_was_sent);
    RUN(bulk_verbs_reach_the_handler_with_the_right_verb);
    RUN(interval_subscribe_validates_the_range);
    RUN(an_interval_subscription_pushes_when_due_with_a_wrapping_issue_counter);
    RUN(resubscribing_changes_the_interval_without_restarting_the_issue_counter);
    RUN(unsubscribe_stops_the_pushes_and_a_second_one_says_not_subscribed);
    RUN(subscriptions_are_per_transport_and_a_reconnect_clears_them);
    RUN(nothing_is_sent_to_a_disconnected_transport);
    RUN(an_event_subscription_is_started_by_the_resource_and_can_be_refused);
    RUN(event_pushes_are_limited_per_tick_by_burst_and_by_transmit_headroom);
    RUN(unsubscribing_or_disconnecting_stops_an_event_resource);
    RUN(an_event_resource_is_polled_only_while_subscribed);
    RUN(short_and_inconsistent_frames_count_as_malformed_and_get_no_answer);
    RUN(the_reassembler_builds_packets_from_single_bytes_and_chunks);
    RUN(the_reassembler_drops_an_oversized_declaration_and_times_out_a_stalled_packet);
    RUN(the_settings_changed_hook_is_called_when_asked);
    return test_summary();
}
