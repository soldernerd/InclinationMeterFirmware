#include "svc_api.h"
#include "svc_battery.h"
#include "svc_storage.h"
#include "app_scheduler.h"
#include "drv_buzzer.h"
#include "math_crc.h"
#include "hal_systick.h"
#include "system_state.h"
#include "config.h"
#include "app_version.h"
#include <string.h>
#include <stddef.h>

/* Device API v2 (WP11) -- breaking replacement of the v1 protocol
 * previously implemented here. See docs/api-v2-spec.md for the design
 * rationale and docs/api-reference.md for the host-facing contract.
 *
 * Stage 1 (2026-08-19): core packet format + dispatcher (spec §2, §3,
 * §5, §6), proven end-to-end with the two simplest categories -- System
 * status (GET only) and Commands (EXECUTE only). No SUBSCRIBE/bulk
 * machinery yet.
 *
 * Stage 2 (2026-08-19): Measurements (category 0x4) -- every live sensor
 * reading exposed as its own individually GET/SUBSCRIBE-able resource
 * (project rule: single data points always available out of the box;
 * bigger constructs like Topic groups/Bulk transfers deferred until an
 * actual need exists). svc_api_update() stays a no-op; subscription
 * delivery is its own function, svc_api_measurement_subscriptions_update(),
 * polled by its own scheduler task -- see that function's own comment
 * for why a shared interval-driven poll would be the wrong shape here.
 *
 * Stage 3 (2026-08-19): Settings (0x3) and Calibrations (0x2) -- same
 * "every value individually accessible" rule extended to every
 * DeviceSettings field and every WP10 displacement calibration constant
 * (legacy REV-A CalibrationData fields deliberately excluded). Both
 * table-driven (27 and 7 resources respectively -- far past the point of
 * handwritten per-resource dispatch); SET range-checks against real,
 * domain-grounded bounds (not just deferring to
 * svc_storage_validate_settings()'s existing zero-divisor repair -- see
 * s_settings_fields[]/s_calib_fields[]'s own comments for the reasoning
 * per field).
 *
 * On-the-wire packet: [OPCODE 2B LE][LEN 2B LE][PAYLOAD 0..LEN][CRC16 2B
 * LE], no padding, total 6+LEN. Every response echoes the request's
 * opcode; a full status byte (Api2Status) is always the first byte of
 * the response payload, followed by resource-specific data (if any) only
 * when status is API2_STATUS_OK. Subscription pushes are also sent under
 * the request's opcode (verb=SUBSCRIBE) per spec §3.1, payload
 * [status][issue_seq][page][resource value] -- see
 * svc_api_measurement_subscriptions_update(). Plain GET responses do NOT
 * carry issue/page counters (spec §2.3's wording is ambiguous on whether
 * they're universal; none of this firmware's payloads are large enough
 * to ever need multi-packet chaining, so there's nothing for a page
 * counter to distinguish yet -- revisit if that changes). */

#define MAX_PAYLOAD (API2_PACKET_MAX_SIZE - API2_PACKET_HDR_BYTES - API2_PACKET_CRC_BYTES)

/* ---------------- payload structs ---------------- */

typedef struct {
    uint8_t fw_major;
    uint8_t fw_minor;
    uint8_t fw_patch;
    char    product_str[16];
    char    serial_str[8];
} __attribute__((packed)) Api2IdentityPayload;

typedef struct {
    uint8_t  battery_state;     /* battery_state_t (Services/svc_battery.h) */
    uint8_t  battery_soc_pct;
    uint16_t battery_mv;
    uint8_t  usb_connected;
    uint8_t  ble_connected;
    uint8_t  calibration_valid;
} __attribute__((packed)) Api2DeviceStatePayload;

/* +1 for the status byte every response payload is prefixed with. */
_Static_assert(sizeof(Api2IdentityPayload)    + 1U <= MAX_PAYLOAD, "IDENTITY response too large");
_Static_assert(sizeof(Api2DeviceStatePayload) + 1U <= MAX_PAYLOAD, "DEVICE_STATE response too large");

/* ---------------- per-transport state ---------------- */

typedef struct {
    bool      connected;
    ApiSendFn send_fn;
} ApiTransportState;

static ApiTransportState s_t[API_TRANSPORT_COUNT];

/* Measurements (category 0x4) subscription state -- direct-indexed by
 * resource index (see svc_api.h's API2_MEASUREMENT_SLOTS comment for why
 * this is a slot-per-topic array, not a searched list). issue_seq is the
 * per-subscription rolling counter (spec §2.3) -- reset to 0 whenever a
 * subscription (re)starts, not shared across resubscriptions. */
typedef struct {
    bool     active;
    uint32_t interval_ms;
    uint32_t last_push_ms;
    uint8_t  issue_seq;
} MeasurementSubSlot;

static MeasurementSubSlot s_meas_subs[API_TRANSPORT_COUNT][API2_MEASUREMENT_SLOTS];

/* ---------------- helpers ---------------- */

/* Fill `dst` with up to `cap` bytes of `src`, zero-padding any remainder.
 * No NUL guarantee -- fields are fixed-width and the host parses by
 * length, not by C-string termination (same convention v1 used). */
static void copy_fixed(char *dst, const char *src, size_t cap)
{
    size_t n = 0;
    while (n < cap && src[n] != '\0') { n++; }
    memcpy(dst, src, n);
    if (n < cap) {
        memset(dst + n, 0, cap - n);
    }
}

/* CLAUDE.md 7.6 escalation for a request/frame this file can't respond to
 * or dispatch -- see SystemState.api_rx_malformed_count's own comment for
 * why this is a counter, not a DBG_PRINT call. */
static void note_malformed(void)
{
    if (g_system_state.api_rx_malformed_count < UINT16_MAX) {
        g_system_state.api_rx_malformed_count++;
    }
}

static void send_response(ApiTransport t, uint16_t opcode, Api2Status status,
                           const uint8_t *data, uint16_t data_len)
{
    if (t >= API_TRANSPORT_COUNT)             return;
    if (!s_t[t].connected || !s_t[t].send_fn) return;

    uint16_t payload_len = (uint16_t)(1U + data_len);   /* status byte + resource data */
    if (payload_len > MAX_PAYLOAD) {
        /* Shouldn't happen at stage-1's payload sizes, but this guards
         * every future category's response-building too -- a resource
         * handler bug or a legitimately-oversized response both land
         * here. */
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

    s_t[t].send_fn(buf, (uint16_t)(before_crc + API2_PACKET_CRC_BYTES));
}

/* Checks CRC over the full received frame. Called only after category/
 * verb/resource have already been confirmed valid (dispatch validation
 * order, docs/api-v2-spec.md §3.4 steps 1-3 before step 4) -- so CRC
 * cycles are only spent on requests that are worth dispatching in the
 * first place, and the status-code priority matches §5/§3.4 exactly
 * (a corrupted-but-structurally-plausible opcode is diagnosed by its
 * apparent category/verb/resource before CRC gets a say). This does NOT
 * mean an invalid request ever actually executes on bad data -- dispatch
 * still stops here on mismatch, before any resource handler runs. */
static bool check_crc(ApiTransport t, uint16_t opcode, const uint8_t *frame, uint16_t paylen)
{
    uint16_t before_crc = (uint16_t)(API2_PACKET_HDR_BYTES + paylen);
    uint16_t calc_crc = math_crc16(frame, before_crc);
    uint16_t got_crc  = (uint16_t)(frame[before_crc + 0U] | (frame[before_crc + 1U] << 8));
    if (calc_crc != got_crc) {
        send_response(t, opcode, API2_STATUS_BAD_CRC, 0, 0);
        return false;
    }
    return true;
}

/* ---------------- System status (GET only) ---------------- */

static void fill_identity(Api2IdentityPayload *p)
{
    memset(p, 0, sizeof *p);
    p->fw_major = (uint8_t)FW_VERSION_MAJOR;
    p->fw_minor = (uint8_t)FW_VERSION_MINOR;
    p->fw_patch = (uint8_t)FW_VERSION_PATCH;
    copy_fixed(p->product_str, USB_PRODUCT_STR, sizeof p->product_str);
    copy_fixed(p->serial_str,  USB_SERIAL_STR,  sizeof p->serial_str);
}

static void fill_device_state(Api2DeviceStatePayload *p)
{
    p->battery_state     = (uint8_t)svc_battery_get_state();
    p->battery_soc_pct   = svc_battery_get_soc_pct();
    p->battery_mv        = svc_battery_get_vbat_mv();
    p->usb_connected     = g_system_state.usb_connected     ? 1U : 0U;
    p->ble_connected     = g_system_state.ble_connected     ? 1U : 0U;
    p->calibration_valid = g_system_state.calibration_valid ? 1U : 0U;
}

static void dispatch_system_status(ApiTransport t, uint16_t opcode, uint8_t verb,
                                    uint8_t res, const uint8_t *frame, uint16_t paylen)
{
    if (verb != API2_VERB_GET) {
        send_response(t, opcode, API2_STATUS_VERB_NOT_VALID, 0, 0);
        return;
    }
    if (res != 0x00U && res != 0x01U) {
        send_response(t, opcode, API2_STATUS_UNKNOWN_RESOURCE, 0, 0);
        return;
    }
    if (!check_crc(t, opcode, frame, paylen)) return;
    if (paylen != 0U) {
        send_response(t, opcode, API2_STATUS_BAD_LENGTH, 0, 0);
        return;
    }

    if (res == 0x00U) {
        Api2IdentityPayload p;
        fill_identity(&p);
        send_response(t, opcode, API2_STATUS_OK, (const uint8_t *)&p, sizeof p);
    } else {
        Api2DeviceStatePayload p;
        fill_device_state(&p);
        send_response(t, opcode, API2_STATUS_OK, (const uint8_t *)&p, sizeof p);
    }
}

/* ---------------- Commands (EXECUTE only) ---------------- */

static void dispatch_commands(ApiTransport t, uint16_t opcode, uint8_t verb,
                               uint8_t res, const uint8_t *frame, uint16_t paylen)
{
    if (verb != API2_VERB_EXECUTE) {
        send_response(t, opcode, API2_STATUS_VERB_NOT_VALID, 0, 0);
        return;
    }
    if (res != 0x00U) {
        send_response(t, opcode, API2_STATUS_UNKNOWN_RESOURCE, 0, 0);
        return;
    }
    if (!check_crc(t, opcode, frame, paylen)) return;
    if (paylen != 0U) {
        send_response(t, opcode, API2_STATUS_BAD_LENGTH, 0, 0);
        return;
    }

    /* TEST_BEEP -- deliberately chosen as the first EXECUTE resource for
     * being safe/self-contained/side-effect-free beyond a short beep, to
     * prove the verb/dispatch mechanics without coupling stage 1 to
     * anything stateful (measurement triggering belongs to Measurements
     * in stage 2 per the spec's own Commands-category framing; charging/
     * sleep control via HAL_App/hal_power.c is real future Commands
     * content but not needed to prove this stage). */
    drv_buzzer_beep(BUZZER_TONE_CLICK, 100U);
    send_response(t, opcode, API2_STATUS_OK, 0, 0);
}

/* ---------------- Measurements (GET, SUBSCRIBE, UNSUBSCRIBE) ---------------- */

/* Every read_*() fills `buf` with exactly the resource's value payload
 * (the bytes that follow the status byte in a GET response, or the
 * issue/page bytes in a subscription push) and returns its length --
 * resource-specific shape, not a generic framework concept (some carry a
 * validity byte, some don't, matching whatever the underlying sensor
 * actually provides -- see each function's own comment). Max payload
 * across all of these is 5 bytes (a float + a validity byte), so a fixed
 * 5-byte scratch buffer is enough for every resource in this table. */
#define MEAS_VALUE_MAX_LEN 5U
typedef uint16_t (*MeasurementReadFn)(uint8_t *buf);

static uint16_t read_onboard_temp(uint8_t *buf)
{
    int16_t v = g_system_state.temperature_cdeg;
    memcpy(buf, &v, sizeof v);
    return sizeof v;
}

static uint16_t read_external_temp(uint8_t *buf)
{
    int16_t v = g_system_state.lm35_temp_cdeg;
    memcpy(buf, &v, sizeof v);
    return sizeof v;
}

static uint16_t read_bme280_temp(uint8_t *buf)
{
    int16_t v = g_system_state.bme280_temp_cdeg;
    memcpy(buf, &v, sizeof v);
    buf[sizeof v] = g_system_state.bme280_ok ? 1U : 0U;
    return (uint16_t)(sizeof v + 1U);
}

static uint16_t read_bme280_pressure(uint8_t *buf)
{
    uint32_t v = g_system_state.bme280_pressure_pa;
    memcpy(buf, &v, sizeof v);
    buf[sizeof v] = g_system_state.bme280_ok ? 1U : 0U;
    return (uint16_t)(sizeof v + 1U);
}

static uint16_t read_bme280_humidity(uint8_t *buf)
{
    uint16_t v = g_system_state.bme280_humidity_centipct;
    memcpy(buf, &v, sizeof v);
    buf[sizeof v] = g_system_state.bme280_ok ? 1U : 0U;
    return (uint16_t)(sizeof v + 1U);
}

static uint16_t read_disp1_delta(uint8_t *buf)
{
    float v = g_system_state.disp1_delta_mm;
    memcpy(buf, &v, sizeof v);
    buf[sizeof v] = g_system_state.disp_ok ? 1U : 0U;
    return (uint16_t)(sizeof v + 1U);
}

static uint16_t read_disp1_residual(uint8_t *buf)
{
    float v = g_system_state.disp1_residual;
    memcpy(buf, &v, sizeof v);
    buf[sizeof v] = g_system_state.disp_ok ? 1U : 0U;
    return (uint16_t)(sizeof v + 1U);
}

static uint16_t read_disp2_delta(uint8_t *buf)
{
    float v = g_system_state.disp2_delta_mm;
    memcpy(buf, &v, sizeof v);
    buf[sizeof v] = g_system_state.disp_ok ? 1U : 0U;
    return (uint16_t)(sizeof v + 1U);
}

static uint16_t read_disp2_residual(uint8_t *buf)
{
    float v = g_system_state.disp2_residual;
    memcpy(buf, &v, sizeof v);
    buf[sizeof v] = g_system_state.disp_ok ? 1U : 0U;
    return (uint16_t)(sizeof v + 1U);
}

typedef struct {
    uint8_t           resource;
    MeasurementReadFn read;
} MeasurementResourceDesc;

static const MeasurementResourceDesc s_meas_resources[] = {
    { API2_RES_MEAS_ONBOARD_TEMP,    read_onboard_temp },
    { API2_RES_MEAS_EXTERNAL_TEMP,   read_external_temp },
    { API2_RES_MEAS_BME280_TEMP,     read_bme280_temp },
    { API2_RES_MEAS_BME280_PRESSURE, read_bme280_pressure },
    { API2_RES_MEAS_BME280_HUMIDITY, read_bme280_humidity },
    { API2_RES_MEAS_DISP1_DELTA,     read_disp1_delta },
    { API2_RES_MEAS_DISP1_RESIDUAL,  read_disp1_residual },
    { API2_RES_MEAS_DISP2_DELTA,     read_disp2_delta },
    { API2_RES_MEAS_DISP2_RESIDUAL,  read_disp2_residual },
};
#define MEAS_RESOURCE_COUNT (sizeof(s_meas_resources) / sizeof(s_meas_resources[0]))

static const MeasurementResourceDesc *find_meas_resource(uint8_t res)
{
    for (size_t i = 0; i < MEAS_RESOURCE_COUNT; ++i) {
        if (s_meas_resources[i].resource == res) {
            return &s_meas_resources[i];
        }
    }
    return 0;
}

static void dispatch_measurements(ApiTransport t, uint16_t opcode, uint8_t verb,
                                   uint8_t res, const uint8_t *frame, uint16_t paylen)
{
    if (verb != API2_VERB_GET && verb != API2_VERB_SUBSCRIBE && verb != API2_VERB_UNSUBSCRIBE) {
        send_response(t, opcode, API2_STATUS_VERB_NOT_VALID, 0, 0);
        return;
    }
    const MeasurementResourceDesc *desc = find_meas_resource(res);
    if (desc == 0 || res >= API2_MEASUREMENT_SLOTS) {
        /* The res >= API2_MEASUREMENT_SLOTS half can't actually happen
         * with today's resource table (all indices are well under 32),
         * but guards the subscription-slot array below against ever
         * being indexed out of bounds if a resource is added above
         * API2_MEASUREMENT_SLOTS without raising that constant too. */
        send_response(t, opcode, API2_STATUS_UNKNOWN_RESOURCE, 0, 0);
        return;
    }
    if (!check_crc(t, opcode, frame, paylen)) return;

    if (verb == API2_VERB_GET) {
        if (paylen != 0U) {
            send_response(t, opcode, API2_STATUS_BAD_LENGTH, 0, 0);
            return;
        }
        uint8_t  val[MEAS_VALUE_MAX_LEN];
        uint16_t vlen = desc->read(val);
        send_response(t, opcode, API2_STATUS_OK, val, vlen);
        return;
    }

    if (verb == API2_VERB_SUBSCRIBE) {
        if (paylen != 4U) {
            send_response(t, opcode, API2_STATUS_BAD_LENGTH, 0, 0);
            return;
        }
        uint32_t interval_ms = (uint32_t)frame[API2_PACKET_HDR_BYTES + 0U]
                              | ((uint32_t)frame[API2_PACKET_HDR_BYTES + 1U] << 8)
                              | ((uint32_t)frame[API2_PACKET_HDR_BYTES + 2U] << 16)
                              | ((uint32_t)frame[API2_PACKET_HDR_BYTES + 3U] << 24);
        if (interval_ms < API2_MEASUREMENT_MIN_INTERVAL_MS
            || interval_ms > API2_MEASUREMENT_MAX_INTERVAL_MS) {
            send_response(t, opcode, API2_STATUS_INVALID_PARAMETER, 0, 0);
            return;
        }
        /* Re-subscribing (already active) updates interval_ms in place
         * and does NOT reset issue_seq -- spec §4.3: this is the same
         * subscription continuing with new parameters, not a new one. */
        MeasurementSubSlot *slot = &s_meas_subs[t][res];
        if (!slot->active) {
            slot->issue_seq = 0;
        }
        slot->active       = true;
        slot->interval_ms  = interval_ms;
        slot->last_push_ms = hal_systick_get_ms();
        send_response(t, opcode, API2_STATUS_OK, 0, 0);
        return;
    }

    /* API2_VERB_UNSUBSCRIBE */
    if (paylen != 0U) {
        send_response(t, opcode, API2_STATUS_BAD_LENGTH, 0, 0);
        return;
    }
    MeasurementSubSlot *slot = &s_meas_subs[t][res];
    if (!slot->active) {
        send_response(t, opcode, API2_STATUS_NOT_SUBSCRIBED, 0, 0);
        return;
    }
    slot->active = false;
    send_response(t, opcode, API2_STATUS_OK, 0, 0);
}

/* ---------------- Settings (GET, SET) / Calibrations (GET, SET) ----------------
 * Both categories are table-driven for the same reason Measurements
 * (above) is: 27 Settings + 7 Calibrations resources is far past the
 * point where handwritten per-resource dispatch cases make sense.
 * Settings' table covers every DeviceSettings field except the internal
 * _settings_end_marker; Calibrations' covers WP10's displacement
 * calibration constants only (DisplacementSensorCal x2 + Displacement-
 * SharedCal) -- legacy REV-A CalibrationData fields are deliberately not
 * exposed (confirmed with the user 2026-08-19). Bounds are real,
 * domain-grounded limits (confirmed with the user 2026-08-19: enforce
 * reasonable bounds, don't just lean on svc_storage_validate_settings()'s
 * existing zero-divisor repair), not arbitrary placeholders -- see each
 * table entry's own reasoning where it isn't self-evident. */

/* Shared by Settings/Calibrations SET -- CLAUDE.md 7.6: an EEPROM write
 * failure must not be reported to the host as success. Mirrors v1's old
 * handle_save_result(). svc_storage_save_settings()/save_blob() are both
 * non-blocking (queue the write, drained later by svc_storage_update()),
 * so DRV_OK here means "successfully queued", not "confirmed written" --
 * a later async failure during that drain still sets
 * g_system_state.settings_save_failed, just without a further API
 * response, since this request/response cycle has already completed by
 * then. Same asynchronous-failure shape as every other DeviceSettings/
 * calibration writer in this codebase (App/app_ui.c's commit_edit()). */
static void send_save_result(ApiTransport t, uint16_t opcode, DrvStatus rc)
{
    if (rc == DRV_OK) {
        send_response(t, opcode, API2_STATUS_OK, 0, 0);
    } else {
        g_system_state.settings_save_failed = true;
        send_response(t, opcode, API2_STATUS_BUSY_RESOURCE, 0, 0);
    }
}

typedef enum {
    SETTINGS_FIELD_U8,
    SETTINGS_FIELD_U16,
    SETTINGS_FIELD_U32,
    SETTINGS_FIELD_I32,
} SettingsFieldType;

typedef struct {
    uint8_t           resource;
    SettingsFieldType type;
    uint8_t           size;    /* wire width in bytes: 1, 2, or 4 -- matches sizeof(field) */
    size_t            offset;  /* offsetof(DeviceSettings, field) */
    int64_t           min;     /* inclusive */
    int64_t           max;     /* inclusive */
} SettingsFieldDesc;

#define SETTINGS_FIELD(res, type, size, field, lo, hi) \
    { (res), (type), (size), offsetof(DeviceSettings, field), (lo), (hi) }

/* Bounds reasoning, grouped:
 * - All *_ms scheduler/timing fields: 1 (can't be faster than the 1 ms
 *   scheduler tick, SYSTICK_PERIOD_MS) to 60000 (1 minute -- beyond this
 *   the feature is effectively disabled, a misconfiguration).
 * - battery_critical_mv/battery_low_mv: 2500-4200 mV, a single-cell Li-
 *   ion/Li-Po's real discharge-curve range (below ~2500 mV risks
 *   over-discharge damage; above ~4200 mV exceeds a full charge).
 *   critical < low is a second, cross-field invariant this per-field
 *   table can't express on its own -- enforced separately in
 *   dispatch_settings()'s SET path, see that check's own comment.
 * - tmp236_*_voffs_mv/seg_boundary_mv: 0-3300, must fit the ADC's VDDA
 *   range (REV B ties VREF+ to 3V3_STANDBY, see hal_adc.h).
 * - tmp236_*_tinfl_cdeg: 0-20000 (0-200.00 degC), generous around the
 *   datasheet's 100 degC segment inflection.
 * - num/den ratio pairs (filter_cutoff_hz, vbat_scale, tmp236_seg1,
 *   tmp236_seg2): 1-10000 each -- must be nonzero (both are divisors),
 *   generous ceiling for a scale-factor ratio.
 * - lm35_scale_mv_per_c: 1-1000, must be nonzero (divisor,
 *   Drivers_App/drv_lm35.c), generous around the LM35's nominal 10.
 * - encoder_counts_per_detent: 1-100, must be nonzero (divisor,
 *   App/app_ui.c), generous ceiling -- no real quadrature encoder needs
 *   more than a few dozen transitions per detent.
 * - settling_threshold_umpm: 1-100000 -- a settling tolerance window
 *   should be a small positive slope value; upper bound is 10x a 1%
 *   grade in umpm terms, comfortably past any real leveling use case.
 * - ble_configured: 0 or 1 only (it's a bool). */
static const SettingsFieldDesc s_settings_fields[] = {
    SETTINGS_FIELD(API2_RES_SET_TASK_SENSORS_MS,        SETTINGS_FIELD_U16, 2, task_sensors_ms,           1,     60000),
    SETTINGS_FIELD(API2_RES_SET_TASK_PROCESSING_MS,     SETTINGS_FIELD_U16, 2, task_processing_ms,        1,     60000),
    SETTINGS_FIELD(API2_RES_SET_TASK_DISPLAY_MS,        SETTINGS_FIELD_U16, 2, task_display_ms,           1,     60000),
    SETTINGS_FIELD(API2_RES_SET_TASK_BLE_MS,            SETTINGS_FIELD_U16, 2, task_ble_ms,               1,     60000),
    SETTINGS_FIELD(API2_RES_SET_TASK_USB_MS,            SETTINGS_FIELD_U16, 2, task_usb_ms,               1,     60000),
    SETTINGS_FIELD(API2_RES_SET_TASK_BATTERY_MS,        SETTINGS_FIELD_U16, 2, task_battery_ms,           1,     60000),
    SETTINGS_FIELD(API2_RES_SET_TASK_TEMPERATURE_MS,    SETTINGS_FIELD_U16, 2, task_temperature_ms,       1,     60000),
    SETTINGS_FIELD(API2_RES_SET_STREAM_INTERVAL_MS,     SETTINGS_FIELD_U16, 2, stream_interval_ms,        1,     60000),
    SETTINGS_FIELD(API2_RES_SET_SETTLING_THRESHOLD,     SETTINGS_FIELD_I32, 4, settling_threshold_umpm,   1,     100000),
    SETTINGS_FIELD(API2_RES_SET_SETTLING_TIMEOUT_MS,    SETTINGS_FIELD_U32, 4, settling_timeout_ms,       1,     60000),
    SETTINGS_FIELD(API2_RES_SET_FILTER_CUTOFF_HZ_NUM,   SETTINGS_FIELD_U16, 2, filter_cutoff_hz_num,      1,     10000),
    SETTINGS_FIELD(API2_RES_SET_FILTER_CUTOFF_HZ_DEN,   SETTINGS_FIELD_U16, 2, filter_cutoff_hz_den,      1,     10000),
    SETTINGS_FIELD(API2_RES_SET_BATTERY_CRITICAL_MV,    SETTINGS_FIELD_U16, 2, battery_critical_mv,       2500,  4200),
    SETTINGS_FIELD(API2_RES_SET_BATTERY_LOW_MV,         SETTINGS_FIELD_U16, 2, battery_low_mv,            2500,  4200),
    SETTINGS_FIELD(API2_RES_SET_VBAT_SCALE_NUM,         SETTINGS_FIELD_U16, 2, vbat_scale_num,            1,     10000),
    SETTINGS_FIELD(API2_RES_SET_VBAT_SCALE_DEN,         SETTINGS_FIELD_U16, 2, vbat_scale_den,            1,     10000),
    SETTINGS_FIELD(API2_RES_SET_TMP236_SEG1_VOFFS_MV,   SETTINGS_FIELD_U16, 2, tmp236_seg1_voffs_mv,      0,     3300),
    SETTINGS_FIELD(API2_RES_SET_TMP236_SEG1_NUM,        SETTINGS_FIELD_U16, 2, tmp236_seg1_num,           1,     10000),
    SETTINGS_FIELD(API2_RES_SET_TMP236_SEG1_DEN,        SETTINGS_FIELD_U16, 2, tmp236_seg1_den,           1,     10000),
    SETTINGS_FIELD(API2_RES_SET_TMP236_SEG_BOUNDARY_MV, SETTINGS_FIELD_U16, 2, tmp236_seg_boundary_mv,    0,     3300),
    SETTINGS_FIELD(API2_RES_SET_TMP236_SEG2_VOFFS_MV,   SETTINGS_FIELD_U16, 2, tmp236_seg2_voffs_mv,      0,     3300),
    SETTINGS_FIELD(API2_RES_SET_TMP236_SEG2_NUM,        SETTINGS_FIELD_U16, 2, tmp236_seg2_num,           1,     10000),
    SETTINGS_FIELD(API2_RES_SET_TMP236_SEG2_DEN,        SETTINGS_FIELD_U16, 2, tmp236_seg2_den,           1,     10000),
    SETTINGS_FIELD(API2_RES_SET_TMP236_SEG2_TINFL_CDEG, SETTINGS_FIELD_U16, 2, tmp236_seg2_tinfl_cdeg,    0,     20000),
    SETTINGS_FIELD(API2_RES_SET_LM35_SCALE_MV_PER_C,    SETTINGS_FIELD_U16, 2, lm35_scale_mv_per_c,       1,     1000),
    SETTINGS_FIELD(API2_RES_SET_ENCODER_COUNTS_PER_DET, SETTINGS_FIELD_U16, 2, encoder_counts_per_detent, 1,     100),
    SETTINGS_FIELD(API2_RES_SET_BLE_CONFIGURED,         SETTINGS_FIELD_U8,  1, ble_configured,            0,     1),
};
#define SETTINGS_FIELD_COUNT (sizeof(s_settings_fields) / sizeof(s_settings_fields[0]))

static const SettingsFieldDesc *find_settings_field(uint8_t res)
{
    for (size_t i = 0; i < SETTINGS_FIELD_COUNT; ++i) {
        if (s_settings_fields[i].resource == res) {
            return &s_settings_fields[i];
        }
    }
    return 0;
}

static uint16_t read_settings_field(const SettingsFieldDesc *d, uint8_t *buf)
{
    const uint8_t *p = (const uint8_t *)&g_device_settings + d->offset;
    memcpy(buf, p, d->size);
    return d->size;
}

/* Widens the wire bytes to int64_t for a uniform range check against
 * d->min/max regardless of the field's actual width/signedness.
 * settling_threshold_umpm (the one SETTINGS_FIELD_I32 field) needs an
 * explicit sign-extend -- every other type here is unsigned, where the
 * raw widened value is already correct. */
static int64_t parse_settings_value(const SettingsFieldDesc *d, const uint8_t *p)
{
    uint32_t u = 0;
    for (uint8_t i = 0; i < d->size; ++i) {
        u |= (uint32_t)p[i] << (8U * i);
    }
    if (d->type == SETTINGS_FIELD_I32) {
        return (int64_t)(int32_t)u;
    }
    return (int64_t)u;
}

static void write_settings_field(const SettingsFieldDesc *d, int64_t val)
{
    uint8_t *p = (uint8_t *)&g_device_settings + d->offset;
    /* val is already range-checked to fit the field; truncating to
     * uint32_t and copying only d->size bytes reproduces the exact
     * native bit pattern regardless of signedness (two's complement) --
     * this machine and the wire format are both little-endian, so no
     * byte-order conversion is needed here either. */
    uint32_t u = (uint32_t)val;
    memcpy(p, &u, d->size);
}

static void dispatch_settings(ApiTransport t, uint16_t opcode, uint8_t verb,
                               uint8_t res, const uint8_t *frame, uint16_t paylen)
{
    if (verb != API2_VERB_GET && verb != API2_VERB_SET) {
        send_response(t, opcode, API2_STATUS_VERB_NOT_VALID, 0, 0);
        return;
    }
    const SettingsFieldDesc *desc = find_settings_field(res);
    if (desc == 0) {
        send_response(t, opcode, API2_STATUS_UNKNOWN_RESOURCE, 0, 0);
        return;
    }
    if (!check_crc(t, opcode, frame, paylen)) return;

    if (verb == API2_VERB_GET) {
        if (paylen != 0U) {
            send_response(t, opcode, API2_STATUS_BAD_LENGTH, 0, 0);
            return;
        }
        uint8_t  buf[4];
        uint16_t len = read_settings_field(desc, buf);
        send_response(t, opcode, API2_STATUS_OK, buf, len);
        return;
    }

    /* SET */
    if (paylen != desc->size) {
        send_response(t, opcode, API2_STATUS_BAD_LENGTH, 0, 0);
        return;
    }
    if (svc_storage_is_busy()) {
        send_response(t, opcode, API2_STATUS_BUSY_RESOURCE, 0, 0);
        return;
    }
    int64_t val = parse_settings_value(desc, &frame[API2_PACKET_HDR_BYTES]);
    if (val < desc->min || val > desc->max) {
        send_response(t, opcode, API2_STATUS_INVALID_PARAMETER, 0, 0);
        return;
    }
    /* Cross-field invariant: battery_critical_mv < battery_low_mv --
     * Services/svc_battery.c's classify_battery_state() relies on this
     * ordering (checks critical first, then low); an inverted pair would
     * misclassify a healthy battery as critical. The per-field [min,max]
     * table above can't express a relationship between two different
     * resources, only each one's own absolute range -- confirmed via
     * code review (2026-08-19) this is a real, host-reachable gap (two
     * independent SETs, each individually in-range, can invert the
     * pair), not just a theoretical one. Checked here as a small special
     * case for this one pair rather than generalizing the whole table. */
    if (res == API2_RES_SET_BATTERY_CRITICAL_MV && val >= g_device_settings.battery_low_mv) {
        send_response(t, opcode, API2_STATUS_INVALID_PARAMETER, 0, 0);
        return;
    }
    if (res == API2_RES_SET_BATTERY_LOW_MV && val <= g_device_settings.battery_critical_mv) {
        send_response(t, opcode, API2_STATUS_INVALID_PARAMETER, 0, 0);
        return;
    }
    write_settings_field(desc, val);
    /* Repairs any zero-divisor field to its default -- the same shared
     * safety net svc_storage_init() applies at boot (Services/
     * svc_storage.c). Every field's own range above already excludes 0
     * from the divisors it covers, so this shouldn't ever actually fire
     * here -- kept anyway per svc_storage.h's own doc comment: "any
     * other writer of DeviceSettings... must run it too". */
    svc_storage_validate_settings(&g_device_settings);
    DrvStatus rc = svc_storage_save_settings(&g_device_settings);
    if (rc == DRV_OK) {
        /* A task-period field may have just changed -- this is the
         * runtime-reload entry point (app_scheduler_init() is boot-only,
         * see its own comment), matching v1's SET_SETTINGS precedent. */
        app_scheduler_reload_periods();
    }
    send_save_result(t, opcode, rc);
}

typedef struct {
    uint8_t  resource;
    void    *base;           /* &g_disp_s1_cal / &g_disp_s2_cal / &g_disp_shared_cal */
    size_t   offset;         /* offsetof(...) within *base's real struct type */
    float    fmin;           /* inclusive */
    float    fmax;           /* inclusive */
    uint16_t eeprom_addr;
    uint16_t eeprom_version;
    uint16_t struct_size;    /* whole struct's size, for svc_storage_save_blob() */
} CalibFieldDesc;

#define CALIB_FIELD(res, structptr, structtype, field, lo, hi, addr, ver) \
    { (res), (structptr), offsetof(structtype, field), (lo), (hi), (addr), (ver), sizeof(structtype) }

/* Bounds reasoning:
 * - gain (S1/S2): 0.1-1000.0 -- must be nonzero (k = atten*gain is a
 *   divisor in Services/svc_displacement.c's compute_sensor_delta()),
 *   generous around the nominal 10.0 (DEFAULT_DISP_GAIN).
 * - d0_mm (S1/S2): 0.001-10.0 -- a physical air gap must be positive;
 *   generous around the nominal 0.1 mm (DEFAULT_DISP_D0_MM) for a
 *   differential-capacitor sensor head.
 * - zero_offset_mm (S1/S2): -10.0 to 10.0 -- no divide-by-zero risk (only
 *   ever added/subtracted), bounded to roughly d0's own generous range so
 *   a wildly wrong offset doesn't silently pass -- an offset larger than
 *   the sensor's own gap range isn't physically meaningful.
 * - atten (shared): 0.1-100.0 -- must be nonzero (also part of the same
 *   k = atten*gain divisor), generous around the nominal 3.0
 *   (DEFAULT_DISP_ATTEN). */
static const CalibFieldDesc s_calib_fields[] = {
    CALIB_FIELD(API2_RES_CAL_S1_GAIN,           &g_disp_s1_cal,     DisplacementSensorCal, gain,           0.1f,   1000.0f,
                EEPROM_DISP_S1_SETTINGS_ADDR, EEPROM_DISP_S1_SETTINGS_VERSION),
    CALIB_FIELD(API2_RES_CAL_S1_D0_MM,          &g_disp_s1_cal,     DisplacementSensorCal, d0_mm,          0.001f, 10.0f,
                EEPROM_DISP_S1_SETTINGS_ADDR, EEPROM_DISP_S1_SETTINGS_VERSION),
    CALIB_FIELD(API2_RES_CAL_S1_ZERO_OFFSET_MM, &g_disp_s1_cal,     DisplacementSensorCal, zero_offset_mm, -10.0f, 10.0f,
                EEPROM_DISP_S1_SETTINGS_ADDR, EEPROM_DISP_S1_SETTINGS_VERSION),
    CALIB_FIELD(API2_RES_CAL_S2_GAIN,           &g_disp_s2_cal,     DisplacementSensorCal, gain,           0.1f,   1000.0f,
                EEPROM_DISP_S2_SETTINGS_ADDR, EEPROM_DISP_S2_SETTINGS_VERSION),
    CALIB_FIELD(API2_RES_CAL_S2_D0_MM,          &g_disp_s2_cal,     DisplacementSensorCal, d0_mm,          0.001f, 10.0f,
                EEPROM_DISP_S2_SETTINGS_ADDR, EEPROM_DISP_S2_SETTINGS_VERSION),
    CALIB_FIELD(API2_RES_CAL_S2_ZERO_OFFSET_MM, &g_disp_s2_cal,     DisplacementSensorCal, zero_offset_mm, -10.0f, 10.0f,
                EEPROM_DISP_S2_SETTINGS_ADDR, EEPROM_DISP_S2_SETTINGS_VERSION),
    CALIB_FIELD(API2_RES_CAL_SHARED_ATTEN,      &g_disp_shared_cal, DisplacementSharedCal, atten,          0.1f,   100.0f,
                EEPROM_DISP_SHARED_SETTINGS_ADDR, EEPROM_DISP_SHARED_SETTINGS_VERSION),
};
#define CALIB_FIELD_COUNT (sizeof(s_calib_fields) / sizeof(s_calib_fields[0]))

static const CalibFieldDesc *find_calib_field(uint8_t res)
{
    for (size_t i = 0; i < CALIB_FIELD_COUNT; ++i) {
        if (s_calib_fields[i].resource == res) {
            return &s_calib_fields[i];
        }
    }
    return 0;
}

static void dispatch_calibrations(ApiTransport t, uint16_t opcode, uint8_t verb,
                                   uint8_t res, const uint8_t *frame, uint16_t paylen)
{
    if (verb != API2_VERB_GET && verb != API2_VERB_SET) {
        send_response(t, opcode, API2_STATUS_VERB_NOT_VALID, 0, 0);
        return;
    }
    const CalibFieldDesc *desc = find_calib_field(res);
    if (desc == 0) {
        send_response(t, opcode, API2_STATUS_UNKNOWN_RESOURCE, 0, 0);
        return;
    }
    if (!check_crc(t, opcode, frame, paylen)) return;

    if (verb == API2_VERB_GET) {
        if (paylen != 0U) {
            send_response(t, opcode, API2_STATUS_BAD_LENGTH, 0, 0);
            return;
        }
        float v;
        memcpy(&v, (const uint8_t *)desc->base + desc->offset, sizeof v);
        send_response(t, opcode, API2_STATUS_OK, (const uint8_t *)&v, sizeof v);
        return;
    }

    /* SET */
    if (paylen != 4U) {
        send_response(t, opcode, API2_STATUS_BAD_LENGTH, 0, 0);
        return;
    }
    if (svc_storage_is_busy()) {
        send_response(t, opcode, API2_STATUS_BUSY_RESOURCE, 0, 0);
        return;
    }
    float val;
    memcpy(&val, &frame[API2_PACKET_HDR_BYTES], sizeof val);
    if (val < desc->fmin || val > desc->fmax) {
        send_response(t, opcode, API2_STATUS_INVALID_PARAMETER, 0, 0);
        return;
    }
    memcpy((uint8_t *)desc->base + desc->offset, &val, sizeof val);
    DrvStatus rc = svc_storage_save_blob(desc->eeprom_addr, desc->eeprom_version,
                                          desc->base, desc->struct_size);
    send_save_result(t, opcode, rc);
}

/* ---------------- dispatch ---------------- */

static void dispatch(ApiTransport t, uint16_t opcode, const uint8_t *frame, uint16_t paylen)
{
    uint8_t verb = API2_OPCODE_VERB(opcode);
    uint8_t cat  = API2_OPCODE_CATEGORY(opcode);
    uint8_t res  = API2_OPCODE_RESOURCE(opcode);

    switch (cat) {
        case API2_CAT_SYSTEM_STATUS:
            dispatch_system_status(t, opcode, verb, res, frame, paylen);
            return;
        case API2_CAT_COMMANDS:
            dispatch_commands(t, opcode, verb, res, frame, paylen);
            return;
        case API2_CAT_CALIBRATIONS:
            dispatch_calibrations(t, opcode, verb, res, frame, paylen);
            return;
        case API2_CAT_SETTINGS:
            dispatch_settings(t, opcode, verb, res, frame, paylen);
            return;
        case API2_CAT_MEASUREMENTS:
            dispatch_measurements(t, opcode, verb, res, frame, paylen);
            return;
        default:
            /* Every other category (0x5-0x8 defined but not yet built in
             * this firmware, or 0x9-0xF genuinely undefined) is
             * UNKNOWN_CATEGORY -- docs/api-v2-spec.md §7's philosophy is
             * that a host knows from versioned documentation what a given
             * firmware build supports, so "not implemented yet" and "not
             * a real category" are the same answer from the wire's point
             * of view. Doesn't reach CRC/length checks -- category
             * existence is validation step 1 (§3.4), before everything
             * else, same as verb/resource above. */
            send_response(t, opcode, API2_STATUS_UNKNOWN_CATEGORY, 0, 0);
            return;
    }
}

/* ---------------- public API ---------------- */

/* Ends every active Measurements subscription on transport `t` -- called
 * on both connect and disconnect (defensive on connect: a fresh session
 * should never inherit a prior one's subscriptions regardless of
 * call-ordering assumptions; required on disconnect: a dropped transport
 * has nowhere for svc_api_measurement_subscriptions_update() to push
 * to). */
static void clear_measurement_subs(ApiTransport t)
{
    if (t >= API_TRANSPORT_COUNT) return;
    memset(s_meas_subs[t], 0, sizeof s_meas_subs[t]);
}

void svc_api_init(void)
{
    memset(s_t, 0, sizeof s_t);
    memset(s_meas_subs, 0, sizeof s_meas_subs);
}

void svc_api_register_transport(ApiTransport t, ApiSendFn send_fn)
{
    if (t >= API_TRANSPORT_COUNT) return;
    s_t[t].send_fn = send_fn;
}

void svc_api_connected(ApiTransport t)
{
    if (t >= API_TRANSPORT_COUNT) return;
    s_t[t].connected = true;
    clear_measurement_subs(t);
}

void svc_api_disconnected(ApiTransport t)
{
    if (t >= API_TRANSPORT_COUNT) return;
    s_t[t].connected = false;
    clear_measurement_subs(t);
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
        /* Declared LEN doesn't match what actually arrived -- can't trust
         * CRC math against a buffer we might read past, and can't tell
         * corrupt-LEN from truncated-delivery apart. Drop rather than
         * echo a possibly-bogus opcode with a guessed status -- mirrors
         * v1's equivalent short-read guard. */
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

    /* As soon as we have the 4-byte header (opcode + len) we know how big
     * the packet is. */
    if (r->pos >= API2_PACKET_HDR_BYTES) {
        uint16_t paylen = (uint16_t)(r->buf[2] | ((uint16_t)r->buf[3] << 8));
        /* uint32_t, not uint16_t -- a corrupted paylen near 0xFFFF (a
         * real risk: this is exactly why byte-stream transports need
         * this reassembler, since wired UART has no link-layer CRC at
         * all) would otherwise overflow-wrap the sum before the ">
         * API2_PACKET_MAX_SIZE" check below ever saw it, e.g. paylen=
         * 0xFFFA gives a true total of 65536 which truncates to 0 and
         * silently defeats this guard. Cast before adding, matching
         * svc_api_receive()'s equivalent (already-correct) guard. */
        uint32_t total = (uint32_t)API2_PACKET_HDR_BYTES + paylen + API2_PACKET_CRC_BYTES;
        if (total > API2_PACKET_MAX_SIZE) {
            /* Garbage, or a packet bigger than this build's reassembly
             * buffer supports (see API2_PACKET_MAX_SIZE's own comment on
             * deferred multi-frame chaining) -- drop either way. */
            note_malformed();
            r->pos = 0;
        } else if (r->pos >= total) {
            /* total is verified <= API2_PACKET_MAX_SIZE (128) above, so
             * this narrowing back to uint16_t is safe. */
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

void svc_api_update(void)
{
    /* Still nothing to poll here -- GET/EXECUTE/SET are synchronous
     * request/response with no periodic push, so nothing in Measurements'
     * one-shot GET path or any other stage-1/stage-2 category needs this.
     * Measurements' subscription delivery is deliberately its OWN
     * function (svc_api_measurement_subscriptions_update() below),
     * polled by its own scheduler task rather than folded in here -- see
     * that function's comment for why. Kept as a scheduler-callable hook
     * (matches every other svc_*_update()) for whatever eventually does
     * need a slower, generic poll (e.g. a future EXECUTE resource with
     * async completion). */
}

void svc_api_measurement_subscriptions_update(void)
{
    /* A dedicated, every-tick-polled function rather than folded into
     * svc_api_update() above: svc_api_update() is itself polled at
     * task_sensors_ms (100 ms default, see App/app_scheduler.c) for
     * stage-1-era reasons (matching v1's old poll cadence) that have
     * nothing to do with Measurements -- tying subscription timing
     * accuracy to that unrelated period would mean a 50 ms subscription
     * interval (API2_MEASUREMENT_MIN_INTERVAL_MS) silently degrades to
     * ~100 ms in practice. This function owns its own scheduling instead:
     * called every tick by its own task, it walks all slots and only
     * acts on the ones actually due, so subscription timing depends
     * solely on each slot's own interval_ms. */
    uint32_t now = hal_systick_get_ms();
    for (ApiTransport t = 0; t < API_TRANSPORT_COUNT; ++t) {
        if (!s_t[t].connected) continue;
        for (uint8_t res = 0; res < API2_MEASUREMENT_SLOTS; ++res) {
            MeasurementSubSlot *slot = &s_meas_subs[t][res];
            if (!slot->active) continue;
            if ((uint32_t)(now - slot->last_push_ms) < slot->interval_ms) continue;

            const MeasurementResourceDesc *desc = find_meas_resource(res);
            if (desc == 0) {
                /* Can't happen with today's table (every active slot was
                 * only ever set by dispatch_measurements() after
                 * confirming the resource exists), but a resource
                 * removed from s_meas_resources[] in a future change
                 * without also clearing any slot that referenced it
                 * would land here -- skip rather than dereference a null
                 * read function. */
                continue;
            }

            uint16_t opcode = API2_OPCODE(API2_VERB_SUBSCRIBE, API2_CAT_MEASUREMENTS, res);
            uint8_t  val[MEAS_VALUE_MAX_LEN];
            uint16_t vlen = desc->read(val);

            /* Push payload: [issue_seq][page=0] + resource value.
             * send_response() prepends the status byte (always OK -- a
             * push is only ever sent for a value that read successfully;
             * there's no failure mode to report mid-stream). page is
             * always 0: none of today's Measurements payloads are large
             * enough to ever need multi-packet chaining, so there's only
             * ever one page per delivery (see this file's top comment). */
            uint8_t push[2U + MEAS_VALUE_MAX_LEN];
            push[0] = slot->issue_seq++;
            push[1] = 0U;   /* page */
            memcpy(&push[2], val, vlen);
            send_response(t, opcode, API2_STATUS_OK, push, (uint16_t)(2U + vlen));

            slot->last_push_ms = now;
        }
    }
}
