#include "svc_api.h"
#include "svc_battery.h"
#include "svc_storage.h"
#include "svc_signal_analysis.h"
#include "drv_ads131m04.h"
#include "svc_log.h"
#include "hal_rtc.h"
#include "app_scheduler.h"
#include "drv_buzzer.h"
#include "math_crc.h"
#include "hal_systick.h"
#include "system_state.h"
#include "config.h"
#include "app_version.h"
#include <string.h>
#include <stddef.h>

/* Device API v2 (WP11) -- ported onto master 2026-09-05 from wp11-api-v2,
 * trimmed to what this REV B build backs (see svc_api.h's top comment).
 *
 * On-the-wire: [OPCODE 2B LE][LEN 2B LE][PAYLOAD 0..LEN][CRC16 2B LE], no
 * padding, total 6+LEN. Every response echoes the request opcode; the
 * first payload byte is always an Api2Status, followed by resource data
 * only when status is OK. Subscription pushes go out under the request's
 * opcode too (spec §3.1), payload [status][issue_seq][page][value]. */

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

_Static_assert(sizeof(Api2IdentityPayload)    + 1U <= MAX_PAYLOAD, "IDENTITY response too large");
_Static_assert(sizeof(Api2DeviceStatePayload) + 1U <= MAX_PAYLOAD, "DEVICE_STATE response too large");

/* ---------------- per-transport state ---------------- */

typedef struct {
    bool     active;
    uint32_t interval_ms;
    uint32_t last_push_ms;
    uint8_t  issue_seq;
} MeasurementSubSlot;

typedef struct {
    bool            active;
    Api2LogSeverity min_sev;
    uint32_t        cursor;
    uint8_t         issue_seq;
} DebugSubState;

typedef struct {
    bool               connected;
    ApiSendFn          send_fn;
    ApiReadyFn         ready_fn;   /* optional TX back-pressure hook (bulk pump only) */
    MeasurementSubSlot meas[API2_MEASUREMENT_SLOTS];
    DebugSubState      dbg;
} ApiTransportState;

static ApiTransportState s_t[API_TRANSPORT_COUNT];

/* ---------------- bulk transfer state (docs/api-v2-spec.md §4.5) ----------------
 * One at a time, device-wide. CAPTURING while the RAM buffer fills at the
 * ADC sample rate; SENDING streams it out in chunks paced by the
 * transport's ready_fn. */
static struct {
    bool         active;
    enum { BULK_IDLE = 0, BULK_CAPTURING, BULK_SENDING } phase;
    ApiTransport transport;
    uint16_t     opcode;
    uint16_t     send_pos;   /* next sample index to send, during SENDING */
    uint8_t      page;       /* wrapping chunk counter */
} s_bulk;

/* ---------------- helpers ---------------- */

static void copy_fixed(char *dst, const char *src, size_t cap)
{
    size_t n = 0;
    while (n < cap && src[n] != '\0') { n++; }
    memcpy(dst, src, n);
    if (n < cap) {
        memset(dst + n, 0, cap - n);
    }
}

static void note_malformed(void)
{
    if (g_system_state.api_rx_malformed_count < UINT16_MAX) {
        g_system_state.api_rx_malformed_count++;
    }
}

/* Build a framed packet and hand it to the transport. `urgent` is passed
 * straight through to the transport's send_fn: true for a direct reply to
 * a request (may use the transport's reserved TX space), false for a
 * subscription/stream push (must not). */
static void send_framed(ApiTransport t, uint16_t opcode, Api2Status status,
                        const uint8_t *data, uint16_t data_len, bool urgent)
{
    if (t >= API_TRANSPORT_COUNT)             return;
    if (!s_t[t].connected || !s_t[t].send_fn) return;

    uint16_t payload_len = (uint16_t)(1U + data_len);   /* status byte + data */
    if (payload_len > MAX_PAYLOAD) {
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

/* Direct reply to a received request — always urgent. */
static void send_response(ApiTransport t, uint16_t opcode, Api2Status status,
                          const uint8_t *data, uint16_t data_len)
{
    send_framed(t, opcode, status, data, data_len, true);
}

/* CRC over the full received frame. Called after category/verb/resource
 * are confirmed valid (spec §3.4 steps 1-3 before step 4). Dispatch
 * always stops here on mismatch, before any resource handler runs. */
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

/* ---------------- System status (0x0) ----------------
 * 0x00 Identity / 0x01 Device state: GET only.
 * 0x02 RTC datetime: GET and SET. */

static void dispatch_rtc(ApiTransport t, uint16_t opcode, uint8_t verb,
                         const uint8_t *frame, uint16_t paylen)
{
    if (verb == API2_VERB_GET) {
        if (paylen != 0U) {
            send_response(t, opcode, API2_STATUS_BAD_LENGTH, 0, 0);
            return;
        }
        rtc_datetime_t dt;
        hal_rtc_get(&dt);
        uint8_t p[9] = {
            (uint8_t)(dt.year & 0xFFU), (uint8_t)(dt.year >> 8),
            dt.month, dt.day, dt.weekday, dt.hour, dt.minute, dt.second,
            (uint8_t)(hal_rtc_is_set() ? 1U : 0U),
        };
        send_response(t, opcode, API2_STATUS_OK, p, sizeof p);
        return;
    }

    /* SET */
    if (paylen != 7U) {
        send_response(t, opcode, API2_STATUS_BAD_LENGTH, 0, 0);
        return;
    }
    const uint8_t *b = &frame[API2_PACKET_HDR_BYTES];
    rtc_datetime_t dt = {
        .year   = (uint16_t)(b[0] | ((uint16_t)b[1] << 8)),
        .month  = b[2], .day = b[3], .weekday = 0,
        .hour   = b[4], .minute = b[5], .second = b[6],
    };
    if (!hal_rtc_datetime_valid(&dt)) {
        send_response(t, opcode, API2_STATUS_INVALID_PARAMETER, 0, 0);
        return;
    }
    if (hal_rtc_set(&dt) != DRV_OK) {
        send_response(t, opcode, API2_STATUS_BUSY_RESOURCE, 0, 0);
        return;
    }
    svc_logf(API2_LOG_INFO, "rtc set %04u-%02u-%02u %02u:%02u:%02u",
             dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    send_response(t, opcode, API2_STATUS_OK, 0, 0);
}

static void dispatch_system_status(ApiTransport t, uint16_t opcode, uint8_t verb,
                                   uint8_t res, const uint8_t *frame, uint16_t paylen)
{
    if (res == API2_RES_SYS_RTC) {
        if (verb != API2_VERB_GET && verb != API2_VERB_SET) {
            send_response(t, opcode, API2_STATUS_VERB_NOT_VALID, 0, 0);
            return;
        }
        if (!check_crc(t, opcode, frame, paylen)) return;
        dispatch_rtc(t, opcode, verb, frame, paylen);
        return;
    }

    if (verb != API2_VERB_GET) {
        send_response(t, opcode, API2_STATUS_VERB_NOT_VALID, 0, 0);
        return;
    }
    if (res != API2_RES_SYS_IDENTITY && res != API2_RES_SYS_DEVICE_STATE) {
        send_response(t, opcode, API2_STATUS_UNKNOWN_RESOURCE, 0, 0);
        return;
    }
    if (!check_crc(t, opcode, frame, paylen)) return;
    if (paylen != 0U) {
        send_response(t, opcode, API2_STATUS_BAD_LENGTH, 0, 0);
        return;
    }

    if (res == API2_RES_SYS_IDENTITY) {
        Api2IdentityPayload p;
        memset(&p, 0, sizeof p);
        p.fw_major = (uint8_t)FW_VERSION_MAJOR;
        p.fw_minor = (uint8_t)FW_VERSION_MINOR;
        p.fw_patch = (uint8_t)FW_VERSION_PATCH;
        copy_fixed(p.product_str, USB_PRODUCT_STR, sizeof p.product_str);
        copy_fixed(p.serial_str,  USB_SERIAL_STR,  sizeof p.serial_str);
        send_response(t, opcode, API2_STATUS_OK, (const uint8_t *)&p, sizeof p);
    } else {
        Api2DeviceStatePayload p;
        p.battery_state     = (uint8_t)svc_battery_get_state();
        p.battery_soc_pct   = svc_battery_get_soc_pct();
        p.battery_mv        = svc_battery_get_vbat_mv();
        p.usb_connected     = g_system_state.usb_connected     ? 1U : 0U;
        p.ble_connected     = g_system_state.ble_connected     ? 1U : 0U;
        p.calibration_valid = g_system_state.calibration_valid ? 1U : 0U;
        send_response(t, opcode, API2_STATUS_OK, (const uint8_t *)&p, sizeof p);
    }
}

/* ---------------- Commands (0x1, EXECUTE only) ---------------- */

static void dispatch_commands(ApiTransport t, uint16_t opcode, uint8_t verb,
                              uint8_t res, const uint8_t *frame, uint16_t paylen)
{
    if (verb != API2_VERB_EXECUTE) {
        send_response(t, opcode, API2_STATUS_VERB_NOT_VALID, 0, 0);
        return;
    }
    if (res != API2_RES_CMD_TEST_BEEP && res != API2_RES_CMD_SIGNAL_ANALYSIS) {
        send_response(t, opcode, API2_STATUS_UNKNOWN_RESOURCE, 0, 0);
        return;
    }
    if (!check_crc(t, opcode, frame, paylen)) return;

    if (res == API2_RES_CMD_SIGNAL_ANALYSIS) {
        if (paylen != 1U) {
            send_response(t, opcode, API2_STATUS_BAD_LENGTH, 0, 0);
            return;
        }
        uint8_t on = frame[API2_PACKET_HDR_BYTES];
        if (on > 1U) {
            send_response(t, opcode, API2_STATUS_INVALID_PARAMETER, 0, 0);
            return;
        }
        if (on) {
            (void)svc_signal_analysis_start();
        } else {
            svc_signal_analysis_stop();
        }
        svc_logf(API2_LOG_INFO, "cmd: signal analysis %s", on ? "start" : "stop");
        send_response(t, opcode, API2_STATUS_OK, 0, 0);
        return;
    }

    /* API2_RES_CMD_TEST_BEEP */
    if (paylen != 0U) {
        send_response(t, opcode, API2_STATUS_BAD_LENGTH, 0, 0);
        return;
    }
    drv_buzzer_beep(BUZZER_TONE_CLICK, 100U);
    svc_log(API2_LOG_INFO, "cmd: test beep");
    send_response(t, opcode, API2_STATUS_OK, 0, 0);
}

/* ---------------- Bulk transfers (0x8: START_BULK, CANCEL_BULK) ---------------- */

static void bulk_abort(void)
{
    svc_signal_analysis_capture_end();
    s_bulk.active = false;
    s_bulk.phase  = BULK_IDLE;
}

static void dispatch_bulk(ApiTransport t, uint16_t opcode, uint8_t verb,
                          uint8_t res, const uint8_t *frame, uint16_t paylen)
{
    if (verb != API2_VERB_START_BULK && verb != API2_VERB_CANCEL_BULK) {
        send_response(t, opcode, API2_STATUS_VERB_NOT_VALID, 0, 0);
        return;
    }
    if (res != API2_RES_BULK_RAW_ADC) {
        send_response(t, opcode, API2_STATUS_UNKNOWN_RESOURCE, 0, 0);
        return;
    }
    if (!check_crc(t, opcode, frame, paylen)) return;
    if (paylen != 0U) {
        send_response(t, opcode, API2_STATUS_BAD_LENGTH, 0, 0);
        return;
    }

    if (verb == API2_VERB_CANCEL_BULK) {
        if (!s_bulk.active) {
            send_response(t, opcode, API2_STATUS_NOTHING_TO_CANCEL, 0, 0);
            return;
        }
        bulk_abort();
        svc_log(API2_LOG_INFO, "bulk: raw adc cancelled");
        send_response(t, opcode, API2_STATUS_OK, 0, 0);
        return;
    }

    /* START_BULK */
    if (s_bulk.active) {
        send_response(t, opcode, API2_STATUS_BUSY_EXCLUSIVE, 0, 0);
        return;
    }
    if (!g_system_state.ads_ok) {
        send_response(t, opcode, API2_STATUS_BUSY_RESOURCE, 0, 0);
        return;
    }
    if (svc_signal_analysis_is_running()) {
        send_response(t, opcode, API2_STATUS_BUSY_EXCLUSIVE, 0, 0);
        return;
    }
    if (svc_signal_analysis_capture_begin() != DRV_OK) {
        send_response(t, opcode, API2_STATUS_BUSY_RESOURCE, 0, 0);
        return;
    }
    s_bulk.active    = true;
    s_bulk.phase     = BULK_CAPTURING;
    s_bulk.transport = t;
    s_bulk.opcode    = opcode;
    s_bulk.send_pos  = 0;
    s_bulk.page      = 0;
    svc_log(API2_LOG_INFO, "bulk: raw adc capture started");
    send_response(t, opcode, API2_STATUS_OK, 0, 0);
}

/* Chunk pump — runs from svc_api_update() each tick while a bulk transfer
 * is active. CAPTURING: wait for the RAM buffer to fill. SENDING: emit up
 * to ADC_BULK_CHUNKS_PER_TICK chunks, but only while the owning
 * transport's TX ring has headroom (ready_fn) so we pace to the wire and
 * yield to other traffic between bursts (spec §4.1). */
static void bulk_pump(void)
{
    if (!s_bulk.active) return;

    ApiTransport t = s_bulk.transport;
    if (!s_t[t].connected) {           /* peer vanished mid-transfer */
        bulk_abort();
        return;
    }

    if (s_bulk.phase == BULK_CAPTURING) {
        if (!svc_signal_analysis_capture_done()) return;
        uint16_t drops = svc_signal_analysis_capture_drops();
        svc_signal_analysis_capture_end();   /* stop the stream ASAP */
        s_bulk.phase    = BULK_SENDING;
        s_bulk.send_pos = 0;
        s_bulk.page     = 0;
        svc_logf(API2_LOG_INFO, "bulk: capture full, %u trigger drops", (unsigned)drops);
    }

    const uint8_t *buf   = svc_signal_analysis_capture_buffer();   /* total * BPS bytes */
    const uint16_t total = svc_signal_analysis_capture_sample_count();
    const ApiReadyFn ready = s_t[t].ready_fn;
    enum { BPS = ADC_BULK_BYTES_PER_SAMPLE };

    for (uint8_t c = 0; c < ADC_BULK_CHUNKS_PER_TICK && s_bulk.send_pos < total; ++c) {
        if (ready != 0 && !ready()) break;   /* let the link drain */

        uint16_t k = (uint16_t)(total - s_bulk.send_pos);
        if (k > ADC_BULK_CHUNK_SAMPLES) k = ADC_BULK_CHUNK_SAMPLES;

        uint8_t payload[1U + ADC_BULK_CHUNK_SAMPLES * BPS];
        payload[0] = s_bulk.page++;
        memcpy(&payload[1], &buf[(size_t)s_bulk.send_pos * BPS], (size_t)k * BPS);

        send_framed(t, s_bulk.opcode, API2_STATUS_OK, payload,
                    (uint16_t)(1U + (size_t)k * BPS), false);
        s_bulk.send_pos = (uint16_t)(s_bulk.send_pos + k);
    }

    if (s_bulk.send_pos >= total) {
        svc_logf(API2_LOG_INFO, "bulk: raw adc sent (%u samples)", (unsigned)total);
        s_bulk.active = false;
        s_bulk.phase  = BULK_IDLE;
    }
}

/* ---------------- Raw data (0x7: GET) ---------------- */

static void dispatch_raw_data(ApiTransport t, uint16_t opcode, uint8_t verb,
                              uint8_t res, const uint8_t *frame, uint16_t paylen)
{
    if (verb != API2_VERB_GET) {
        send_response(t, opcode, API2_STATUS_VERB_NOT_VALID, 0, 0);
        return;
    }
    if (res != API2_RES_RAW_ADC_DIAG) {
        send_response(t, opcode, API2_STATUS_UNKNOWN_RESOURCE, 0, 0);
        return;
    }
    if (!check_crc(t, opcode, frame, paylen)) return;
    if (paylen != 0U) {
        send_response(t, opcode, API2_STATUS_BAD_LENGTH, 0, 0);
        return;
    }

    const Ads131m04Regs *r = drv_ads131m04_get_regs();
    uint16_t samples = 0, drops = 0;
    uint32_t elapsed = 0;
    svc_signal_analysis_last_capture(&samples, &drops, &elapsed);

    struct __attribute__((packed)) {
        uint16_t id, status, mode, clock, gain1, cfg;
        uint16_t clock_expected;
        uint8_t  regs_read_ok;
        uint8_t  ads_ok;
        uint16_t last_samples;
        uint16_t last_drops;
        uint32_t last_elapsed_ms;
    } p;
    p.id              = r->id;
    p.status          = r->status;
    p.mode            = r->mode;
    p.clock           = r->clock;
    p.gain1           = r->gain1;
    p.cfg             = r->cfg;
    p.clock_expected  = r->clock_expected;
    p.regs_read_ok    = r->read_ok ? 1U : 0U;
    p.ads_ok          = g_system_state.ads_ok ? 1U : 0U;
    p.last_samples    = samples;
    p.last_drops      = drops;
    p.last_elapsed_ms = elapsed;

    send_response(t, opcode, API2_STATUS_OK, (const uint8_t *)&p, sizeof p);
}

/* ---------------- Measurements (0x4: GET, SUBSCRIBE, UNSUBSCRIBE) ---------------- */

#define MEAS_VALUE_MAX_LEN 4U
typedef uint16_t (*MeasurementReadFn)(uint8_t *buf);

static uint16_t read_onboard_temp(uint8_t *buf)
{
    int16_t v = g_system_state.temperature_cdeg;
    memcpy(buf, &v, sizeof v);
    return sizeof v;
}
static uint16_t read_battery_mv(uint8_t *buf)
{
    uint16_t v = svc_battery_get_vbat_mv();
    memcpy(buf, &v, sizeof v);
    return sizeof v;
}
static uint16_t read_battery_soc(uint8_t *buf)
{
    buf[0] = svc_battery_get_soc_pct();
    return 1U;
}

typedef struct {
    uint8_t           resource;
    MeasurementReadFn read;
} MeasurementResourceDesc;

static const MeasurementResourceDesc s_meas_resources[] = {
    { API2_RES_MEAS_ONBOARD_TEMP, read_onboard_temp },
    { API2_RES_MEAS_BATTERY_MV,   read_battery_mv },
    { API2_RES_MEAS_BATTERY_SOC,  read_battery_soc },
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
        MeasurementSubSlot *slot = &s_t[t].meas[res];
        if (!slot->active) {
            slot->issue_seq = 0;
        }
        slot->active       = true;
        slot->interval_ms  = interval_ms;
        slot->last_push_ms = hal_systick_get_ms();
        send_response(t, opcode, API2_STATUS_OK, 0, 0);
        return;
    }

    /* UNSUBSCRIBE */
    if (paylen != 0U) {
        send_response(t, opcode, API2_STATUS_BAD_LENGTH, 0, 0);
        return;
    }
    MeasurementSubSlot *slot = &s_t[t].meas[res];
    if (!slot->active) {
        send_response(t, opcode, API2_STATUS_NOT_SUBSCRIBED, 0, 0);
        return;
    }
    slot->active = false;
    send_response(t, opcode, API2_STATUS_OK, 0, 0);
}

/* ---------------- Settings (0x3: GET, SET) ---------------- */

typedef enum { SF_U16, SF_U32, SF_I32 } SettingsFieldType;

typedef struct {
    uint8_t           resource;
    SettingsFieldType type;
    uint8_t           size;    /* 2 or 4, == sizeof(field) */
    size_t            offset;  /* offsetof(DeviceSettings, field) */
    int64_t           min;
    int64_t           max;
} SettingsFieldDesc;

#define SF(res, type, size, field, lo, hi) \
    { (res), (type), (size), offsetof(DeviceSettings, field), (lo), (hi) }

/* Bounds: *_ms 1..60000 (1 ms scheduler tick .. effectively-disabled);
 * battery_*_mv 2500..4200 (single-cell Li-ion real range); tmp236 voffs /
 * boundary 0..3300 (ADC VDDA); num/den ratio pairs 1..10000 (nonzero
 * divisors); lm35_scale 1..1000; encoder_counts 1..100;
 * settling_threshold 1..100000; tmp236 tinfl 0..20000 (0..200.00 degC);
 * auto_poweroff_s 0..65535 (0 = disabled). */
static const SettingsFieldDesc s_settings_fields[] = {
    SF(API2_RES_SET_TASK_SENSORS_MS,         SF_U16, 2, task_sensors_ms,           1,    60000),
    SF(API2_RES_SET_TASK_PROCESSING_MS,      SF_U16, 2, task_processing_ms,        1,    60000),
    SF(API2_RES_SET_TASK_DISPLAY_MS,         SF_U16, 2, task_display_ms,           1,    60000),
    SF(API2_RES_SET_TASK_BLE_MS,             SF_U16, 2, task_ble_ms,               1,    60000),
    SF(API2_RES_SET_TASK_USB_MS,             SF_U16, 2, task_usb_ms,               1,    60000),
    SF(API2_RES_SET_TASK_BATTERY_MS,         SF_U16, 2, task_battery_ms,           1,    60000),
    SF(API2_RES_SET_TASK_TEMPERATURE_MS,     SF_U16, 2, task_temperature_ms,       1,    60000),
    SF(API2_RES_SET_STREAM_INTERVAL_MS,      SF_U16, 2, stream_interval_ms,        1,    60000),
    SF(API2_RES_SET_SETTLING_THRESHOLD,      SF_I32, 4, settling_threshold_umpm,   1,    100000),
    SF(API2_RES_SET_SETTLING_TIMEOUT_MS,     SF_U32, 4, settling_timeout_ms,       1,    60000),
    SF(API2_RES_SET_FILTER_CUTOFF_HZ_NUM,    SF_U16, 2, filter_cutoff_hz_num,      1,    10000),
    SF(API2_RES_SET_FILTER_CUTOFF_HZ_DEN,    SF_U16, 2, filter_cutoff_hz_den,      1,    10000),
    SF(API2_RES_SET_BATTERY_CRITICAL_MV,     SF_U16, 2, battery_critical_mv,       2500, 4200),
    SF(API2_RES_SET_BATTERY_LOW_MV,          SF_U16, 2, battery_low_mv,            2500, 4200),
    SF(API2_RES_SET_BATTERY_CHARGE_START_MV, SF_U16, 2, battery_charge_start_mv,   2500, 4200),
    SF(API2_RES_SET_VBAT_SCALE_NUM,          SF_U16, 2, vbat_scale_num,            1,    10000),
    SF(API2_RES_SET_VBAT_SCALE_DEN,          SF_U16, 2, vbat_scale_den,            1,    10000),
    SF(API2_RES_SET_TMP236_SEG1_VOFFS_MV,    SF_U16, 2, tmp236_seg1_voffs_mv,      0,    3300),
    SF(API2_RES_SET_TMP236_SEG1_NUM,         SF_U16, 2, tmp236_seg1_num,           1,    10000),
    SF(API2_RES_SET_TMP236_SEG1_DEN,         SF_U16, 2, tmp236_seg1_den,           1,    10000),
    SF(API2_RES_SET_TMP236_SEG_BOUNDARY_MV,  SF_U16, 2, tmp236_seg_boundary_mv,    0,    3300),
    SF(API2_RES_SET_TMP236_SEG2_VOFFS_MV,    SF_U16, 2, tmp236_seg2_voffs_mv,      0,    3300),
    SF(API2_RES_SET_TMP236_SEG2_NUM,         SF_U16, 2, tmp236_seg2_num,           1,    10000),
    SF(API2_RES_SET_TMP236_SEG2_DEN,         SF_U16, 2, tmp236_seg2_den,           1,    10000),
    SF(API2_RES_SET_TMP236_SEG2_TINFL_CDEG,  SF_U16, 2, tmp236_seg2_tinfl_cdeg,    0,    20000),
    SF(API2_RES_SET_LM35_SCALE_MV_PER_C,     SF_U16, 2, lm35_scale_mv_per_c,       1,    1000),
    SF(API2_RES_SET_ENCODER_COUNTS_PER_DET,  SF_U16, 2, encoder_counts_per_detent, 1,    100),
    SF(API2_RES_SET_AUTO_POWEROFF_S,         SF_U16, 2, auto_poweroff_s,           0,    65535),
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

static int64_t parse_settings_value(const SettingsFieldDesc *d, const uint8_t *p)
{
    uint32_t u = 0;
    for (uint8_t i = 0; i < d->size; ++i) {
        u |= (uint32_t)p[i] << (8U * i);
    }
    if (d->type == SF_I32) {
        return (int64_t)(int32_t)u;
    }
    return (int64_t)u;
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
        uint8_t buf[4];
        memcpy(buf, (const uint8_t *)&g_device_settings + desc->offset, desc->size);
        send_response(t, opcode, API2_STATUS_OK, buf, desc->size);
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
    /* Cross-field: battery_critical_mv < battery_low_mv (svc_battery.c's
     * classify order). Per-field bounds can't express a 2-resource
     * relationship; checked here for this one pair. */
    if (res == API2_RES_SET_BATTERY_CRITICAL_MV && val >= g_device_settings.battery_low_mv) {
        send_response(t, opcode, API2_STATUS_INVALID_PARAMETER, 0, 0);
        return;
    }
    if (res == API2_RES_SET_BATTERY_LOW_MV && val <= g_device_settings.battery_critical_mv) {
        send_response(t, opcode, API2_STATUS_INVALID_PARAMETER, 0, 0);
        return;
    }

    uint32_t u = (uint32_t)val;
    memcpy((uint8_t *)&g_device_settings + desc->offset, &u, desc->size);
    svc_storage_validate_settings(&g_device_settings);
    DrvStatus rc = svc_storage_save_settings(&g_device_settings);
    if (rc == DRV_OK) {
        app_scheduler_reload_periods();
        svc_logf(API2_LOG_INFO, "set: res 0x%02X saved", res);
        send_response(t, opcode, API2_STATUS_OK, 0, 0);
    } else {
        g_system_state.settings_save_failed = true;
        svc_logf(API2_LOG_ERROR, "set: res 0x%02X save failed", res);
        send_response(t, opcode, API2_STATUS_BUSY_RESOURCE, 0, 0);
    }
}

/* ---------------- Debug messages (0x6: SUBSCRIBE, UNSUBSCRIBE) ---------------- */

static void dispatch_debug(ApiTransport t, uint16_t opcode, uint8_t verb,
                           uint8_t res, const uint8_t *frame, uint16_t paylen)
{
    if (verb != API2_VERB_SUBSCRIBE && verb != API2_VERB_UNSUBSCRIBE) {
        send_response(t, opcode, API2_STATUS_VERB_NOT_VALID, 0, 0);
        return;
    }
    if (res != API2_RES_DEBUG_LOG_STREAM) {
        send_response(t, opcode, API2_STATUS_UNKNOWN_RESOURCE, 0, 0);
        return;
    }
    if (!check_crc(t, opcode, frame, paylen)) return;

    if (verb == API2_VERB_SUBSCRIBE) {
        if (paylen != 1U) {
            send_response(t, opcode, API2_STATUS_BAD_LENGTH, 0, 0);
            return;
        }
        uint8_t min_sev = frame[API2_PACKET_HDR_BYTES];
        if (min_sev > (uint8_t)API2_LOG_ERROR) {
            send_response(t, opcode, API2_STATUS_INVALID_PARAMETER, 0, 0);
            return;
        }
        DebugSubState *d = &s_t[t].dbg;
        if (!d->active) {
            d->issue_seq = 0;
            d->cursor    = 0;   /* 0 -> first drain flushes whatever backlog is held */
        }
        d->active  = true;
        d->min_sev = (Api2LogSeverity)min_sev;
        send_response(t, opcode, API2_STATUS_OK, 0, 0);
        return;
    }

    /* UNSUBSCRIBE */
    if (paylen != 0U) {
        send_response(t, opcode, API2_STATUS_BAD_LENGTH, 0, 0);
        return;
    }
    DebugSubState *d = &s_t[t].dbg;
    if (!d->active) {
        send_response(t, opcode, API2_STATUS_NOT_SUBSCRIBED, 0, 0);
        return;
    }
    d->active = false;
    send_response(t, opcode, API2_STATUS_OK, 0, 0);
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
        case API2_CAT_SETTINGS:
            dispatch_settings(t, opcode, verb, res, frame, paylen);
            return;
        case API2_CAT_MEASUREMENTS:
            dispatch_measurements(t, opcode, verb, res, frame, paylen);
            return;
        case API2_CAT_DEBUG_MSGS:
            dispatch_debug(t, opcode, verb, res, frame, paylen);
            return;
        case API2_CAT_RAW_DATA:
            dispatch_raw_data(t, opcode, verb, res, frame, paylen);
            return;
        case API2_CAT_BULK:
            dispatch_bulk(t, opcode, verb, res, frame, paylen);
            return;
        default:
            /* Calibrations (0x2) and 0x5-0xF: not built in this ported
             * subset. Spec §7 -- "not implemented yet" and "not a real
             * category" are the same answer on the wire. */
            send_response(t, opcode, API2_STATUS_UNKNOWN_CATEGORY, 0, 0);
            return;
    }
}

/* ---------------- public API ---------------- */

static void clear_subs(ApiTransport t)
{
    if (t >= API_TRANSPORT_COUNT) return;
    memset(s_t[t].meas, 0, sizeof s_t[t].meas);
    memset(&s_t[t].dbg, 0, sizeof s_t[t].dbg);
}

void svc_api_init(void)
{
    memset(s_t, 0, sizeof s_t);
    memset(&s_bulk, 0, sizeof s_bulk);
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
    if (s_bulk.active && s_bulk.transport == t) {
        bulk_abort();
    }
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

/* Debug-log push: a few lines per call per subscribed transport so a slow
 * BLE link isn't flooded in one tick. */
#define DEBUG_PUSH_PER_TICK 4U

void svc_api_update(void)
{
    bulk_pump();

    for (ApiTransport t = 0; t < API_TRANSPORT_COUNT; ++t) {
        if (!s_t[t].connected || !s_t[t].dbg.active) continue;
        DebugSubState *d = &s_t[t].dbg;

        for (uint8_t k = 0; k < DEBUG_PUSH_PER_TICK; ++k) {
            char    msg[SVC_LOG_MSG_MAX];
            uint8_t mlen = 0;
            Api2LogSeverity sev = API2_LOG_INFO;
            if (!svc_log_drain(&d->cursor, d->min_sev, &sev, msg, &mlen)) {
                break;
            }
            uint16_t opcode = API2_OPCODE(API2_VERB_SUBSCRIBE, API2_CAT_DEBUG_MSGS,
                                          API2_RES_DEBUG_LOG_STREAM);
            uint8_t push[3U + SVC_LOG_MSG_MAX];
            push[0] = d->issue_seq++;
            push[1] = 0U;                 /* page */
            push[2] = (uint8_t)sev;
            memcpy(&push[3], msg, mlen);
            /* stream push — not urgent, must leave the TX reserve free */
            send_framed(t, opcode, API2_STATUS_OK, push, (uint16_t)(3U + mlen), false);
        }
    }
}

void svc_api_measurement_subscriptions_update(void)
{
    uint32_t now = hal_systick_get_ms();
    for (ApiTransport t = 0; t < API_TRANSPORT_COUNT; ++t) {
        if (!s_t[t].connected) continue;
        for (uint8_t res = 0; res < API2_MEASUREMENT_SLOTS; ++res) {
            MeasurementSubSlot *slot = &s_t[t].meas[res];
            if (!slot->active) continue;
            if ((uint32_t)(now - slot->last_push_ms) < slot->interval_ms) continue;

            const MeasurementResourceDesc *desc = find_meas_resource(res);
            if (desc == 0) continue;

            uint16_t opcode = API2_OPCODE(API2_VERB_SUBSCRIBE, API2_CAT_MEASUREMENTS, res);
            uint8_t  val[MEAS_VALUE_MAX_LEN];
            uint16_t vlen = desc->read(val);

            uint8_t push[2U + MEAS_VALUE_MAX_LEN];
            push[0] = slot->issue_seq++;
            push[1] = 0U;   /* page */
            memcpy(&push[2], val, vlen);
            /* stream push — not urgent, must leave the TX reserve free */
            send_framed(t, opcode, API2_STATUS_OK, push, (uint16_t)(2U + vlen), false);

            slot->last_push_ms = now;
        }
    }
}
