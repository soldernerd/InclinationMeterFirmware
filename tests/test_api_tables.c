/* Host tests for the REAL generated API tables (Services/svc_api_tables.c, from tools/api_spec.py), the generic
 * settings/calibration field handler (Services/svc_api_fields.c) and the dispatcher, with do-nothing handlers for
 * everything else (tests/api_handler_stubs.inc, generated). Checks what the tables promise: every resource reachable
 * with its verbs and request length, every field resource bounds-checked and persisted, state-changing resources
 * safe against a stale v2 client. */
#include "test.h"
#include <string.h>

#include "../Math/math_crc.c"
#include "../Services/svc_api_defs.h"
#include "../system_state.h"
#include "../Services/svc_log.h"
#include "../Drivers_App/drv_common.h"

/* ---------------- host stubs ---------------- */
DeviceSettings g_device_settings;
SystemState    g_system_state;

static uint32_t g_ms;
uint32_t hal_systick_get_ms(void) { return g_ms; }

void api_res_update(void) {}
void api_res_transport_disconnected(ApiTransport t) { (void)t; }
void api_res_request_seen(void) {}
static bool g_unlocked;
bool api_service_unlocked(void) { return g_unlocked; }

static bool g_storage_busy, g_save_fails;
static int  g_saves, g_validates, g_trim_calls;
bool svc_storage_is_busy(void) { return g_storage_busy; }
void svc_storage_validate_settings(DeviceSettings *s) { (void)s; g_validates++; }
DrvStatus svc_storage_save_settings(const DeviceSettings *s) { (void)s; g_saves++; return g_save_fails ? DRV_ERR_COMM : DRV_OK; }
void svc_log(Api2LogSeverity sev, const char *msg) { (void)sev; (void)msg; }
void svc_logf(Api2LogSeverity sev, const char *fmt, ...) { (void)sev; (void)fmt; }
DrvStatus hal_rtc_set_trim_ppm_x10(int16_t v) { (void)v; g_trim_calls++; return DRV_OK; }

#include "../Services/svc_api.c"
#include "../Services/svc_api_fields.c"
#include "../Services/svc_api_tables.c"
#include "api_handler_stubs.inc"

/* ---------------- harness ---------------- */
static uint8_t g_resp[API2_PACKET_MAX_SIZE];
static uint16_t g_resp_n;
static int g_responses;
static void send_usb(const uint8_t *d, uint16_t n, bool urgent)
{
    (void)urgent; memcpy(g_resp, d, n); g_resp_n = n; g_responses++;
}
static int g_changed;
static void changed_cb(void) { g_changed++; }

static void fresh(void)
{
    svc_api_init();
    svc_api_register_transport(API_TRANSPORT_USB, send_usb);
    svc_api_connected(API_TRANSPORT_USB);
    svc_api_register_settings_changed(changed_cb);
    memset(&g_device_settings, 0, sizeof g_device_settings);
    memset(&g_system_state, 0, sizeof g_system_state);
    g_device_settings.battery_low_mv = 3600;
    g_device_settings.battery_critical_mv = 3400;
    g_storage_busy = g_save_fails = false;
    g_unlocked = true;                                   /* most tests are about something else */
    g_saves = g_validates = g_trim_calls = g_changed = 0;
    g_stub_calls = 0;
    g_responses = 0;
}

static Api2Status xfer(uint16_t opcode, const void *payload, uint16_t len)
{
    uint8_t f[API2_PACKET_MAX_SIZE];
    f[0] = (uint8_t)opcode; f[1] = (uint8_t)(opcode >> 8); f[2] = (uint8_t)len; f[3] = (uint8_t)(len >> 8);
    if (len) memcpy(&f[4], payload, len);
    uint16_t crc = math_crc16(f, (uint16_t)(4 + len));
    f[4 + len] = (uint8_t)crc; f[5 + len] = (uint8_t)(crc >> 8);
    int before = g_responses;
    svc_api_receive(API_TRANSPORT_USB, f, (uint16_t)(6 + len));
    return g_responses == before ? (Api2Status)0xFF : (Api2Status)g_resp[4];
}

static int64_t read_field(const ApiFieldDesc *d)
{
    uint32_t u = 0;
    memcpy(&u, (const uint8_t *)&g_device_settings + d->offset, d->size);
    if (d->is_signed) {
        if (d->size == 1) return (int8_t)u;
        if (d->size == 2) return (int16_t)u;
        return (int32_t)u;
    }
    return u;
}

static Api2Status set_field(const ApiResource *r, uint8_t cat, int64_t v)
{
    const ApiFieldDesc *d = (const ApiFieldDesc *)r->ctx;
    uint32_t u = (uint32_t)v;
    return xfer(API2_OPCODE(API2_VERB_SET, cat, r->id), &u, d->size);
}

static const ApiCategory *cat_by_id(uint8_t id)
{
    for (uint8_t i = 0; i < g_api_category_count; ++i) if (g_api_categories[i].id == id) return &g_api_categories[i];
    return 0;
}

/* ---------------- structure ---------------- */

TEST(resource_ids_and_subscription_slots_are_unique)
{
    uint8_t slot_used[API2_SUB_SLOTS];
    memset(slot_used, 0, sizeof slot_used);
    for (uint8_t ci = 0; ci < g_api_category_count; ++ci) {
        const ApiCategory *c = &g_api_categories[ci];
        for (uint8_t i = 0; i < c->count; ++i) {
            for (uint8_t j = (uint8_t)(i + 1); j < c->count; ++j) CHECK(c->res[i].id != c->res[j].id);
            const ApiResource *r = &c->res[i];
            CHECK((r->verbs & ~c->verbs) == 0);          /* a resource never allows a verb its category does not */
            if (r->sub != API2_SUB_NONE) {
                CHECK(r->slot < API2_SUB_SLOTS);
                CHECK_EQ(slot_used[r->slot], 0);
                slot_used[r->slot] = 1;
                CHECK(r->verbs & API2_VERB_BIT(API2_VERB_SUBSCRIBE));
                CHECK(r->verbs & API2_VERB_BIT(API2_VERB_UNSUBSCRIBE));
                if (r->sub == API2_SUB_EVENT) CHECK(r->ev != 0 && r->burst >= 1);
            }
        }
    }
    for (unsigned i = 0; i < API2_SUB_SLOTS; ++i) CHECK_EQ(slot_used[i], 1);   /* no slot wasted or missing */
}

TEST(state_changing_resources_never_use_ids_a_v2_client_knew)
{
    for (uint8_t ci = 0; ci < g_api_category_count; ++ci) {
        const ApiCategory *c = &g_api_categories[ci];
        for (uint8_t i = 0; i < c->count; ++i) {
            const ApiResource *r = &c->res[i];
            bool changes = (r->verbs & (API2_VERB_BIT(API2_VERB_SET) | API2_VERB_BIT(API2_VERB_EXECUTE))) != 0;
            if (changes && c->id != API2_CAT_SYSTEM) CHECK(r->id >= 0x40);
        }
    }
    fresh();
    /* what a v2 client would send: Commands 0x00..0x09 (EXECUTE), Settings 0x00..0x1C / Calibrations 0x00..0x0E (SET) */
    for (uint8_t id = 0; id <= 0x1F; ++id) {
        uint8_t zeros[4] = {0};
        CHECK_EQ(xfer(API2_OPCODE(API2_VERB_EXECUTE, API2_CAT_COMMANDS, id), zeros, 1), API2_STATUS_UNKNOWN_RESOURCE);
        CHECK_EQ(xfer(API2_OPCODE(API2_VERB_SET, API2_CAT_SETTINGS, id), zeros, 2), API2_STATUS_UNKNOWN_RESOURCE);
        CHECK_EQ(xfer(API2_OPCODE(API2_VERB_SET, API2_CAT_CALIBRATIONS, id), zeros, 4), API2_STATUS_UNKNOWN_RESOURCE);
    }
    CHECK_EQ(g_stub_calls, 0);
    CHECK_EQ(g_saves, 0);
}

TEST(every_resource_answers_each_of_its_verbs_with_the_right_request_length)
{
    fresh();
    for (uint8_t ci = 0; ci < g_api_category_count; ++ci) {
        const ApiCategory *c = &g_api_categories[ci];
        for (uint8_t i = 0; i < c->count; ++i) {
            const ApiResource *r = &c->res[i];
            if (r->handler == api_field_handler) continue;           /* the field test below */
            for (uint8_t v = 0; v <= (uint8_t)API2_VERB_CANCEL_BULK; ++v) {
                uint8_t zeros[16] = {0};
                uint16_t len = 0;
                if (v == API2_VERB_SUBSCRIBE) {
                    len = r->sub_in_len;
                    if (r->sub == API2_SUB_INTERVAL) { zeros[0] = 0xE8; zeros[1] = 0x03; }   /* 1000 ms */
                }
                else if (v == API2_VERB_SET || v == API2_VERB_EXECUTE) len = r->in_min;
                Api2Status st = xfer(API2_OPCODE(v, c->id, r->id), zeros, len);
                if (r->verbs & API2_VERB_BIT(v)) {
                    if (v == API2_VERB_UNSUBSCRIBE) {
                        CHECK(st == API2_STATUS_NOT_SUBSCRIBED || st == API2_STATUS_OK);
                    } else {
                        CHECK_EQ(st, API2_STATUS_OK);
                    }
                } else {
                    CHECK_EQ(st, API2_STATUS_VERB_NOT_VALID);
                }
            }
        }
    }
}

/* ---------------- the field resources ---------------- */

TEST(every_field_resource_reads_back_what_is_stored_and_rejects_the_wrong_length)
{
    fresh();
    int fields = 0;
    for (uint8_t cid = 0; cid < 2; ++cid) {
        const ApiCategory *c = cat_by_id(cid == 0 ? API2_CAT_CALIBRATIONS : API2_CAT_SETTINGS);
        for (uint8_t i = 0; i < c->count; ++i) {
            const ApiResource *r = &c->res[i];
            const ApiFieldDesc *d = (const ApiFieldDesc *)r->ctx;
            CHECK(d != 0 && d->res == r->id);
            CHECK(d->size == 1 || d->size == 2 || d->size == 4);
            CHECK(d->offset + d->size <= sizeof(DeviceSettings));
            CHECK(d->lo <= d->hi);
            CHECK_EQ(r->in_min, d->size);
            CHECK_EQ(r->in_max, d->size);
            fields++;

            int64_t v = (d->lo + d->hi) / 2;
            if (d->check) v = d->lo;                              /* cross-field rules are tested below */
            if (set_field(r, c->id, v) == API2_STATUS_OK) {
                CHECK_EQ(read_field(d), v);
                CHECK_EQ(xfer(API2_OPCODE(API2_VERB_GET, c->id, r->id), 0, 0), API2_STATUS_OK);
                CHECK_EQ(g_resp_n, 6 + 1 + d->size);
                uint32_t got = 0; memcpy(&got, &g_resp[5], d->size);
                uint32_t want = (uint32_t)v;
                if (d->size < 4) want &= (1UL << (8 * d->size)) - 1U;
                CHECK_EQ(got, want);
            }
            uint8_t wrong[8] = {0};
            CHECK_EQ(xfer(API2_OPCODE(API2_VERB_SET, c->id, r->id), wrong, (uint16_t)(d->size + 1)), API2_STATUS_BAD_LENGTH);
            CHECK_EQ(xfer(API2_OPCODE(API2_VERB_SET, c->id, r->id), wrong, (uint16_t)(d->size - 1)), API2_STATUS_BAD_LENGTH);
        }
    }
    CHECK(fields >= 30);
}

TEST(every_field_accepts_both_bounds_and_refuses_one_step_outside)
{
    fresh();
    for (uint8_t cid = 0; cid < 2; ++cid) {
        const ApiCategory *c = cat_by_id(cid == 0 ? API2_CAT_CALIBRATIONS : API2_CAT_SETTINGS);
        for (uint8_t i = 0; i < c->count; ++i) {
            const ApiResource *r = &c->res[i];
            const ApiFieldDesc *d = (const ApiFieldDesc *)r->ctx;
            if (d->check) continue;                                   /* ranges depend on a second field */
            CHECK_EQ(set_field(r, c->id, d->lo), API2_STATUS_OK);
            CHECK_EQ(read_field(d), d->lo);
            CHECK_EQ(set_field(r, c->id, d->hi), API2_STATUS_OK);
            CHECK_EQ(read_field(d), d->hi);
            int64_t max_repr = d->is_signed ? ((1LL << (8 * d->size - 1)) - 1) : ((1LL << (8 * d->size)) - 1);
            int64_t min_repr = d->is_signed ? -(1LL << (8 * d->size - 1)) : 0;
            if (d->hi < max_repr) {
                int saves = g_saves;
                CHECK_EQ(set_field(r, c->id, d->hi + 1), API2_STATUS_INVALID_PARAMETER);
                CHECK_EQ(read_field(d), d->hi);                       /* a refused value is not stored ... */
                CHECK_EQ(g_saves, saves);                             /* ... nor saved */
            }
            if (d->lo > min_repr) {
                CHECK_EQ(set_field(r, c->id, d->lo), API2_STATUS_OK);
                CHECK_EQ(set_field(r, c->id, d->lo - 1), API2_STATUS_INVALID_PARAMETER);
                CHECK_EQ(read_field(d), d->lo);
            }
        }
    }
}

TEST(a_set_is_validated_saved_and_only_settings_notify_the_app)
{
    fresh();
    const ApiCategory *set = cat_by_id(API2_CAT_SETTINGS), *cal = cat_by_id(API2_CAT_CALIBRATIONS);
    const ApiResource *sr = &set->res[0];
    CHECK_EQ(set_field(sr, set->id, ((const ApiFieldDesc *)sr->ctx)->lo), API2_STATUS_OK);
    CHECK_EQ(g_saves, 1);
    CHECK_EQ(g_validates, 1);
    CHECK_EQ(g_changed, 1);
    const ApiResource *cr = &cal->res[0];
    CHECK_EQ(set_field(cr, cal->id, ((const ApiFieldDesc *)cr->ctx)->lo), API2_STATUS_OK);
    CHECK_EQ(g_saves, 2);
    CHECK_EQ(g_changed, 1);                                          /* calibrations do not notify */
}

TEST(a_busy_store_or_a_failed_save_is_reported_and_latched)
{
    fresh();
    const ApiCategory *cal = cat_by_id(API2_CAT_CALIBRATIONS);
    const ApiResource *r = &cal->res[0];
    const ApiFieldDesc *d = (const ApiFieldDesc *)r->ctx;
    CHECK_EQ(set_field(r, cal->id, d->lo), API2_STATUS_OK);
    g_storage_busy = true;
    CHECK_EQ(set_field(r, cal->id, d->hi), API2_STATUS_BUSY_RESOURCE);
    CHECK_EQ(read_field(d), d->lo);                                  /* nothing changed */
    g_storage_busy = false;
    g_save_fails = true;
    CHECK_EQ(set_field(r, cal->id, d->hi), API2_STATUS_BUSY_RESOURCE);
    CHECK(g_system_state.settings_save_failed);
}

TEST(the_battery_thresholds_keep_critical_below_low)
{
    fresh();
    const ApiCategory *set = cat_by_id(API2_CAT_SETTINGS);
    const ApiResource *crit = 0, *low = 0;
    for (uint8_t i = 0; i < set->count; ++i) {
        if (set->res[i].id == API2_RES_SETTINGS_BATTERY_CRITICAL) crit = &set->res[i];
        if (set->res[i].id == API2_RES_SETTINGS_BATTERY_LOW) low = &set->res[i];
    }
    CHECK(crit && low);
    g_device_settings.battery_critical_mv = 3400;
    g_device_settings.battery_low_mv = 3600;
    CHECK_EQ(set_field(crit, set->id, 3599), API2_STATUS_OK);
    CHECK_EQ(set_field(crit, set->id, 3600), API2_STATUS_INVALID_PARAMETER);       /* not below low */
    CHECK_EQ(set_field(low, set->id, 3599), API2_STATUS_INVALID_PARAMETER);        /* not above critical */
    CHECK_EQ(set_field(low, set->id, 3700), API2_STATUS_OK);
    CHECK_EQ(set_field(crit, set->id, 3600), API2_STATUS_OK);
    CHECK_EQ(set_field(crit, set->id, 3601), API2_STATUS_INVALID_PARAMETER);       /* above the field's own limit */
}

TEST(a_new_rtc_trim_is_applied_at_once_but_only_when_it_was_stored)
{
    fresh();
    const ApiCategory *cal = cat_by_id(API2_CAT_CALIBRATIONS);
    const ApiResource *trim = 0;
    for (uint8_t i = 0; i < cal->count; ++i) if (cal->res[i].id == API2_RES_CALIBRATIONS_RTC_TRIM) trim = &cal->res[i];
    CHECK(trim != 0);
    CHECK_EQ(set_field(trim, cal->id, -123), API2_STATUS_OK);
    CHECK_EQ(g_device_settings.rtc_trim_ppm_x10, -123);
    CHECK_EQ(g_trim_calls, 1);
    CHECK_EQ(set_field(trim, cal->id, 99999), API2_STATUS_INVALID_PARAMETER);
    CHECK_EQ(g_trim_calls, 1);
    g_save_fails = true;
    CHECK_EQ(set_field(trim, cal->id, 50), API2_STATUS_BUSY_RESOURCE);
    CHECK_EQ(g_trim_calls, 1);
}

TEST(selected_resources_have_the_documented_verbs_and_lengths)
{
    fresh();
    uint8_t z[8] = {0};
    CHECK_EQ(xfer(API2_OP_SYSTEM_IDENTITY_GET, 0, 0), API2_STATUS_OK);
    CHECK_EQ(xfer(API2_OPCODE(API2_VERB_SET, API2_CAT_SYSTEM, API2_RES_SYSTEM_IDENTITY), z, 1), API2_STATUS_VERB_NOT_VALID);
    CHECK_EQ(xfer(API2_OP_SYSTEM_RTC_SET, z, 6), API2_STATUS_BAD_LENGTH);
    CHECK_EQ(xfer(API2_OP_SYSTEM_RTC_SET, z, 7), API2_STATUS_OK);
    CHECK_EQ(xfer(API2_OPCODE(API2_VERB_GET, API2_CAT_TOPICS, API2_RES_TOPICS_PHASOR_STREAM), 0, 0), API2_STATUS_VERB_NOT_VALID);
    CHECK_EQ(xfer(API2_OP_COMMANDS_ZERO_CAL_EXECUTE, z, 0), API2_STATUS_BAD_LENGTH);
    CHECK_EQ(xfer(API2_OP_COMMANDS_ZERO_CAL_EXECUTE, z, 1), API2_STATUS_OK);
    CHECK_EQ(xfer(API2_OP_COMMANDS_ZERO_CAL_EXECUTE, z, 2), API2_STATUS_OK);
    CHECK_EQ(xfer(API2_OP_COMMANDS_ZERO_CAL_EXECUTE, z, 3), API2_STATUS_BAD_LENGTH);
    CHECK_EQ(xfer(API2_OP_COMMANDS_POWER_TEST_EXECUTE, z, 4), API2_STATUS_OK);
    CHECK_EQ(xfer(API2_OP_COMMANDS_POWER_TEST_EXECUTE, z, 3), API2_STATUS_BAD_LENGTH);
    CHECK_EQ(xfer(API2_OP_COMMANDS_POWER_OFF_EXECUTE, z, 1), API2_STATUS_BAD_LENGTH);
    CHECK_EQ(xfer(API2_OP_COMMANDS_FACTORY_DEFAULTS_EXECUTE, z, 1), API2_STATUS_OK);
    CHECK_EQ(xfer(API2_OP_DEBUG_LOG_SUBSCRIBE, z, 4), API2_STATUS_BAD_LENGTH);          /* the log takes 1 byte */
    CHECK_EQ(xfer(API2_OP_DEBUG_LOG_SUBSCRIBE, z, 1), API2_STATUS_OK);
    CHECK_EQ(xfer(API2_OP_BULK_RAW_ADC_START_BULK, z, 0), API2_STATUS_OK);
    CHECK_EQ(xfer(API2_OPCODE(API2_VERB_GET, API2_CAT_BULK, API2_RES_BULK_RAW_ADC), 0, 0), API2_STATUS_VERB_NOT_VALID);
    CHECK_EQ(API2_OP_COMMANDS_POWER_OFF_EXECUTE, 0x2140);                               /* ids are part of the contract */
    CHECK_EQ(API2_OP_TOPICS_LIVE_SUBSCRIBE, 0x3502);
    CHECK_EQ(API2_OP_BULK_RAW_ADC_START_BULK, 0x5800);
}

/* The resources a stranger's phone must not be able to change without someone at the instrument. This list is the
 * contract; it must match the `service=True` flags in tools/api_spec.py. */
static bool expected_gated(uint8_t cat, uint8_t res)
{
    if (cat == API2_CAT_CALIBRATIONS) return true;                       /* every calibration write */
    if (cat != API2_CAT_COMMANDS) return false;
    return res == API2_RES_COMMANDS_REBOOT_DFU || res == API2_RES_COMMANDS_FACTORY_DEFAULTS
        || res == API2_RES_COMMANDS_ZERO_CAL   || res == API2_RES_COMMANDS_POWER_TEST
        || res == API2_RES_COMMANDS_PIN_TEST   || res == API2_RES_COMMANDS_RAIL
        || res == API2_RES_COMMANDS_FAULT_TEST;
}

TEST(exactly_the_documented_resources_need_service_mode)
{
    fresh();
    int gated = 0;
    for (uint8_t ci = 0; ci < g_api_category_count; ++ci) {
        const ApiCategory *c = &g_api_categories[ci];
        for (uint8_t i = 0; i < c->count; ++i) {
            const ApiResource *r = &c->res[i];
            bool flagged = (r->flags & API2_RES_F_SERVICE) != 0;
            CHECK_EQ(flagged, expected_gated(c->id, r->id));
            if (flagged) gated++;
            /* a gated resource really changes state (nothing read-only is flagged) */
            if (flagged) CHECK(r->verbs & (API2_VERB_BIT(API2_VERB_SET) | API2_VERB_BIT(API2_VERB_EXECUTE)));
        }
    }
    CHECK(gated >= 20);
}

TEST(locked_the_gated_resources_refuse_and_everything_else_works)
{
    fresh();
    g_unlocked = false;
    uint8_t z[8] = {0};
    int refused = 0;
    for (uint8_t ci = 0; ci < g_api_category_count; ++ci) {
        const ApiCategory *c = &g_api_categories[ci];
        for (uint8_t i = 0; i < c->count; ++i) {
            const ApiResource *r = &c->res[i];
            for (uint8_t v = 0; v <= (uint8_t)API2_VERB_CANCEL_BULK; ++v) {
                if (!(r->verbs & API2_VERB_BIT(v)) || (v != API2_VERB_SET && v != API2_VERB_EXECUTE)) continue;
                uint16_t len = (r->handler == api_field_handler) ? ((const ApiFieldDesc *)r->ctx)->size : r->in_min;
                unsigned calls = g_stub_calls;
                int saves = g_saves;
                Api2Status st = xfer(API2_OPCODE(v, c->id, r->id), z, len);
                if (expected_gated(c->id, r->id)) {
                    CHECK_EQ(st, API2_STATUS_SERVICE_MODE_REQUIRED);
                    CHECK_EQ(g_stub_calls, calls);
                    CHECK_EQ(g_saves, saves);                                 /* nothing was written */
                    refused++;
                } else {
                    CHECK(st != API2_STATUS_SERVICE_MODE_REQUIRED);
                }
            }
        }
    }
    CHECK(refused >= 20);
    /* reading calibrations stays possible while locked */
    const ApiCategory *cal = cat_by_id(API2_CAT_CALIBRATIONS);
    for (uint8_t i = 0; i < cal->count; ++i) {
        CHECK_EQ(xfer(API2_OPCODE(API2_VERB_GET, cal->id, cal->res[i].id), 0, 0), API2_STATUS_OK);
    }
    g_unlocked = true;
    const ApiResource *r = &cal->res[0];
    CHECK_EQ(set_field(r, cal->id, ((const ApiFieldDesc *)r->ctx)->lo), API2_STATUS_OK);
}

int main(void)
{
    RUN(resource_ids_and_subscription_slots_are_unique);
    RUN(state_changing_resources_never_use_ids_a_v2_client_knew);
    RUN(every_resource_answers_each_of_its_verbs_with_the_right_request_length);
    RUN(every_field_resource_reads_back_what_is_stored_and_rejects_the_wrong_length);
    RUN(every_field_accepts_both_bounds_and_refuses_one_step_outside);
    RUN(a_set_is_validated_saved_and_only_settings_notify_the_app);
    RUN(a_busy_store_or_a_failed_save_is_reported_and_latched);
    RUN(the_battery_thresholds_keep_critical_below_low);
    RUN(a_new_rtc_trim_is_applied_at_once_but_only_when_it_was_stored);
    RUN(selected_resources_have_the_documented_verbs_and_lengths);
    RUN(exactly_the_documented_resources_need_service_mode);
    RUN(locked_the_gated_resources_refuse_and_everything_else_works);
    return test_summary();
}
