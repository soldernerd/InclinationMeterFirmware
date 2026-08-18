#include "svc_api.h"
#include "svc_measurement.h"
#include "svc_displacement.h"
#include "svc_storage.h"
#include "math_crc.h"
#include "hal_systick.h"
#include "system_state.h"
#include "config.h"
#include "app_version.h"
#include "app_scheduler.h"
#include <string.h>

/* On-the-wire packet:
 *   [CMD][LEN][PAYLOAD ...][CRC16 LSB][CRC16 MSB]
 *   total <= USB_HID_REPORT_SIZE (64). LEN counts payload bytes only.
 *   CRC16 covers CMD + LEN + PAYLOAD.
 * HDR_BYTES/CRC_BYTES alias svc_api.h's public API_PACKET_* so this file's
 * shorter names stay readable without a second, divergible source of
 * truth — every byte-stream transport (svc_ble.c, svc_uart.c) reassembles
 * against the same constants via svc_api_reassembler_feed_byte() below. */
#define HDR_BYTES        API_PACKET_HDR_BYTES
#define CRC_BYTES        API_PACKET_CRC_BYTES
#define MAX_PAYLOAD      (USB_HID_REPORT_SIZE - HDR_BYTES - CRC_BYTES)   /* 60 */

_Static_assert(USB_HID_REPORT_SIZE == API_PACKET_MAX_SIZE,
               "API_PACKET_MAX_SIZE must track USB_HID_REPORT_SIZE — both bound the same on-the-wire packet");

/* ---------------- payload structs ---------------- */

typedef struct {
    uint8_t  battery_soc_pct;
    uint16_t battery_mv;
    uint8_t  battery_state;
    uint8_t  ble_connected;
    uint8_t  usb_connected;
    uint8_t  sensor_scl3300_ok;
    uint8_t  sensor_pcap04_1_ok;
    uint8_t  sensor_pcap04_2_ok;
    uint8_t  calibration_valid;
    uint8_t  fw_major;
    uint8_t  fw_minor;
    uint8_t  fw_patch;
} __attribute__((packed)) ApiStatusPayload;

typedef struct {
    int32_t  tilt_pcap04_umpm;
    int32_t  tilt_scl3300_x_umpm;
    int32_t  tilt_scl3300_y_umpm;
    int16_t  temperature_cdeg;
    uint8_t  battery_soc_pct;
    uint8_t  status_flags;
    uint32_t timestamp_ms;
} __attribute__((packed)) ApiStreamPayload;

typedef struct {
    int32_t  pcap04_1_af;
    int32_t  pcap04_2_af;
    int32_t  pcap04_diff_af;
    int16_t  scl3300_x_cdeg;
    int16_t  scl3300_y_cdeg;
    int16_t  scl3300_z_cdeg;
    int32_t  tilt_pcap04_umpm;
    int32_t  tilt_scl3300_x_umpm;
    int16_t  temperature_cdeg;
    uint8_t  battery_soc_pct;
    uint8_t  status_flags;
    uint32_t timestamp_ms;
} __attribute__((packed)) ApiRawStreamPayload;

typedef struct {
    int32_t  tilt_pcap04_umpm;
    int32_t  tilt_scl3300_x_umpm;
    int32_t  tilt_scl3300_y_umpm;
    int16_t  temperature_cdeg;
    uint8_t  battery_soc_pct;
    uint8_t  status_flags;
    uint32_t timestamp_ms;
    uint16_t sample_count;
} __attribute__((packed)) ApiSinglePayload;

typedef struct {
    uint8_t  fw_major;
    uint8_t  fw_minor;
    uint8_t  fw_patch;
    char     product_str[16];
    char     serial_str[8];
} __attribute__((packed)) ApiIdentityPayload;

/* One Services/svc_displacement.c cycle, on the wire. Deliberately a
 * separate packed struct rather than sending DisplacementCycle directly:
 * that struct is intentionally left unpacked (float-aligned) for safe
 * direct field access in the ring buffer (see its own doc comment) --
 * memcpy-ing an array of it here would carry its trailing alignment
 * padding onto the wire and blow the batch-of-3-per-report budget below. */
typedef struct {
    uint16_t seq;   /* Services/svc_displacement.h's DisplacementCycle.seq --
                      * wraps every 65536 cycles (~25 s at ~2.6 kHz); a host
                      * doing gap detection MUST compare with wraparound-
                      * safe (modular) arithmetic, not naive equality or
                      * seq != prev+1 checks, or it'll see a false gap at
                      * every rollover even with nothing actually lost. */
    float    delta1_mm;
    float    residual1;
    float    delta2_mm;
    float    residual2;
} __attribute__((packed)) ApiDispRecord;

/* MAX_PAYLOAD (60) = 2-byte header + N * sizeof(ApiDispRecord) (18) -->
 * N <= 3.22, so 3 records/report (56 of 60 bytes used). At the USB HID
 * interrupt endpoint's ~1000 reports/sec ceiling (bInterval=1ms -- not
 * directly confirmed from this device's descriptor, verify before
 * relying on this headroom number) that's 3000 cycles/sec of drain
 * capacity against ~2604.17 cycles/sec production, ~15% margin. */
#define API_DISP_STREAM_MAX_RECORDS 3U

typedef struct {
    uint8_t       count;      /* valid entries in records[], 1..API_DISP_STREAM_MAX_RECORDS */
    uint8_t       reserved;   /* Not currently read by either side --
                                * available for a future flags/version byte
                                * (e.g. "output ring overflowed since last
                                * report") without changing the header size
                                * the batch-of-3 byte budget is computed
                                * against. */
    ApiDispRecord records[API_DISP_STREAM_MAX_RECORDS];
} __attribute__((packed)) ApiDispStreamPayload;

/* Compile-time guarantees that every payload fits in one HID report */
_Static_assert(sizeof(ApiStatusPayload)     <= MAX_PAYLOAD, "STATUS payload too large");
_Static_assert(sizeof(ApiStreamPayload)     <= MAX_PAYLOAD, "STREAM payload too large");
_Static_assert(sizeof(ApiRawStreamPayload)  <= MAX_PAYLOAD, "RAW_STREAM payload too large");
_Static_assert(sizeof(ApiSinglePayload)     <= MAX_PAYLOAD, "SINGLE payload too large");
_Static_assert(sizeof(ApiIdentityPayload)   <= MAX_PAYLOAD, "IDENTITY payload too large");
_Static_assert(sizeof(DeviceSettings)       <= MAX_PAYLOAD, "DeviceSettings payload too large");
_Static_assert(sizeof(CalibrationData)      <= MAX_PAYLOAD, "CalibrationData payload too large");
_Static_assert(sizeof(ApiDispStreamPayload) <= MAX_PAYLOAD, "DISP_STREAM payload too large");

/* ---------------- per-transport state ---------------- */

typedef struct {
    bool        connected;
    ApiMode     mode;
    ApiSendFn   send_fn;
    uint32_t    last_notify_ms;
    uint32_t    last_progress_ms;
} ApiTransportState;

static ApiTransportState s_t[API_TRANSPORT_COUNT];

/* ---------------- helpers ---------------- */

static void send_packet(ApiTransport t, uint8_t cmd,
                        const uint8_t *payload, uint16_t payload_len)
{
    if (t >= API_TRANSPORT_COUNT)        return;
    if (!s_t[t].connected || !s_t[t].send_fn) return;
    if (payload_len > MAX_PAYLOAD)       return;

    uint8_t buf[USB_HID_REPORT_SIZE];
    memset(buf, 0, sizeof buf);
    buf[0] = cmd;
    buf[1] = (uint8_t)payload_len;
    if (payload && payload_len) {
        memcpy(&buf[2], payload, payload_len);
    }
    uint16_t crc = math_crc16(buf, (uint16_t)(HDR_BYTES + payload_len));
    buf[HDR_BYTES + payload_len + 0U] = (uint8_t)(crc & 0xFFU);
    buf[HDR_BYTES + payload_len + 1U] = (uint8_t)((crc >> 8) & 0xFFU);

    s_t[t].send_fn(buf, USB_HID_REPORT_SIZE);
}

static void send_ack(ApiTransport t)  { send_packet(t, API_RSP_ACK,  0, 0); }
static void send_nack(ApiTransport t) { send_packet(t, API_RSP_NACK, 0, 0); }

/* Shared by SET_ZERO/SET_CALIBRATION/SET_SETTINGS — CLAUDE.md 7.6: an
 * EEPROM write failure must not be reported to the host as success, and
 * must be escalated the same way App/app_ui.c's commit_edit() does for
 * the local-UI save path. Callers must have already ruled out
 * DRV_ERR_NOT_READY (svc_storage_is_busy()) before calling this — that's
 * transient contention with another in-flight save, not a real failure,
 * and doesn't belong in settings_save_failed. */
static void handle_save_result(ApiTransport t, DrvStatus rc)
{
    if (rc == DRV_OK) {
        send_ack(t);
    } else {
        g_system_state.settings_save_failed = true;
        send_nack(t);
    }
}

static void fill_status(ApiStatusPayload *p)
{
    extern uint16_t svc_battery_get_vbat_mv(void);
    p->battery_soc_pct    = g_system_state.battery_soc_pct;
    p->battery_mv         = svc_battery_get_vbat_mv();
    p->battery_state      = (uint8_t)(g_system_state.battery_critical ? 2U
                              : (g_system_state.battery_charging ? 3U : 0U));
    p->ble_connected      = g_system_state.ble_connected      ? 1U : 0U;
    p->usb_connected      = g_system_state.usb_connected      ? 1U : 0U;
    p->sensor_scl3300_ok  = g_system_state.sensor_scl3300_ok  ? 1U : 0U;
    p->sensor_pcap04_1_ok = g_system_state.sensor_pcap04_1_ok ? 1U : 0U;
    p->sensor_pcap04_2_ok = g_system_state.sensor_pcap04_2_ok ? 1U : 0U;
    p->calibration_valid  = g_system_state.calibration_valid  ? 1U : 0U;
    p->fw_major           = (uint8_t)FW_VERSION_MAJOR;
    p->fw_minor            = (uint8_t)FW_VERSION_MINOR;
    p->fw_patch            = (uint8_t)FW_VERSION_PATCH;
}

static uint8_t sensor_status_flags(void)
{
    uint8_t f = 0;
    if (g_system_state.sensor_scl3300_ok)  f |= 0x01U;
    if (g_system_state.sensor_pcap04_1_ok) f |= 0x02U;
    if (g_system_state.sensor_pcap04_2_ok) f |= 0x04U;
    return f;
}

static void fill_stream(ApiStreamPayload *p)
{
    p->tilt_pcap04_umpm    = g_system_state.tilt_pcap04_umpm;
    p->tilt_scl3300_x_umpm = g_system_state.tilt_scl3300_x_umpm;
    p->tilt_scl3300_y_umpm = g_system_state.tilt_scl3300_y_umpm;
    p->temperature_cdeg    = g_system_state.temperature_cdeg;
    p->battery_soc_pct     = g_system_state.battery_soc_pct;
    p->status_flags        = sensor_status_flags();
    p->timestamp_ms        = hal_systick_get_ms();
}

static void fill_raw_stream(ApiRawStreamPayload *p)
{
    p->pcap04_1_af         = g_system_state.pcap04_1_af;
    p->pcap04_2_af         = g_system_state.pcap04_2_af;
    p->pcap04_diff_af      = g_system_state.pcap04_1_af - g_system_state.pcap04_2_af;
    p->scl3300_x_cdeg      = g_system_state.scl3300_x_cdeg;
    p->scl3300_y_cdeg      = g_system_state.scl3300_y_cdeg;
    p->scl3300_z_cdeg      = g_system_state.scl3300_z_cdeg;
    p->tilt_pcap04_umpm    = g_system_state.tilt_pcap04_umpm;
    p->tilt_scl3300_x_umpm = g_system_state.tilt_scl3300_x_umpm;
    p->temperature_cdeg    = g_system_state.temperature_cdeg;
    p->battery_soc_pct     = g_system_state.battery_soc_pct;
    p->status_flags        = sensor_status_flags();
    p->timestamp_ms        = hal_systick_get_ms();
}

static void copy_fixed(char *dst, const char *src, size_t cap)
{
    /* Fill `dst` with up to `cap` bytes of `src`, zero-padding any
     * remainder. No NUL guarantee — fields are fixed-width and the host
     * parses by length, not by C-string termination. memcpy of a string
     * literal that exactly fills the field is the case strncpy warns
     * about under -Wstringop-truncation. */
    size_t n = 0;
    while (n < cap && src[n] != '\0') { n++; }
    memcpy(dst, src, n);
    if (n < cap) {
        memset(dst + n, 0, cap - n);
    }
}

static void fill_identity(ApiIdentityPayload *p)
{
    memset(p, 0, sizeof *p);
    p->fw_major = (uint8_t)FW_VERSION_MAJOR;
    p->fw_minor = (uint8_t)FW_VERSION_MINOR;
    p->fw_patch = (uint8_t)FW_VERSION_PATCH;
    copy_fixed(p->product_str, USB_PRODUCT_STR, sizeof p->product_str);
    copy_fixed(p->serial_str,  USB_SERIAL_STR,  sizeof p->serial_str);
}

static void send_status(ApiTransport t)
{
    ApiStatusPayload p;
    fill_status(&p);
    send_packet(t, API_RSP_GET_STATUS, (const uint8_t *)&p, sizeof p);
}

static void send_identity(ApiTransport t)
{
    ApiIdentityPayload p;
    fill_identity(&p);
    send_packet(t, API_RSP_GET_IDENTITY, (const uint8_t *)&p, sizeof p);
}

static void send_settings(ApiTransport t)
{
    send_packet(t, API_RSP_GET_SETTINGS,
                (const uint8_t *)&g_device_settings, sizeof g_device_settings);
}

static void send_calibration(ApiTransport t)
{
    send_packet(t, API_RSP_GET_CALIBRATION,
                (const uint8_t *)&g_calibration, sizeof g_calibration);
}

static void send_stream_data(ApiTransport t)
{
    ApiStreamPayload p;
    fill_stream(&p);
    send_packet(t, API_NOTIFY_STREAM_DATA, (const uint8_t *)&p, sizeof p);
}

static void send_raw_stream_data(ApiTransport t)
{
    ApiRawStreamPayload p;
    fill_raw_stream(&p);
    send_packet(t, API_NOTIFY_RAW_STREAM_DATA, (const uint8_t *)&p, sizeof p);
}

static void send_disp_stream_data(ApiTransport t)
{
    ApiDispStreamPayload p;
    p.count    = 0;
    p.reserved = 0;

    DisplacementCycle c;
    while (p.count < API_DISP_STREAM_MAX_RECORDS && svc_displacement_pop(&c)) {
        ApiDispRecord *r = &p.records[p.count];
        r->seq       = c.seq;
        r->delta1_mm = c.delta1_mm;
        r->residual1 = c.residual1;
        r->delta2_mm = c.delta2_mm;
        r->residual2 = c.residual2;
        p.count++;
    }
    if (p.count == 0) {
        return;   /* nothing pending this tick */
    }

    uint16_t payload_len = (uint16_t)(2U + (uint16_t)p.count * sizeof(ApiDispRecord));
    send_packet(t, API_NOTIFY_DISP_STREAM_DATA, (const uint8_t *)&p, payload_len);
}

static void send_single_progress(ApiTransport t, uint8_t pct)
{
    send_packet(t, API_NOTIFY_SINGLE_PROGRESS, &pct, 1);
}

static void send_single_ready(ApiTransport t)
{
    const MeasurementPacket *m = svc_measurement_get_packet();
    ApiSinglePayload p = {
        .tilt_pcap04_umpm    = m->tilt_pcap04_umpm,
        .tilt_scl3300_x_umpm = m->tilt_scl3300_x_umpm,
        .tilt_scl3300_y_umpm = m->tilt_scl3300_y_umpm,
        .temperature_cdeg    = m->temperature_cdeg,
        .battery_soc_pct     = m->battery_soc_pct,
        .status_flags        = m->status_flags,
        .timestamp_ms        = m->timestamp_ms,
        .sample_count        = m->sample_count,
    };
    send_packet(t, API_NOTIFY_SINGLE_READY, (const uint8_t *)&p, sizeof p);
}

/* ---------------- dispatch ---------------- */

static void dispatch(ApiTransport t, uint8_t cmd,
                     const uint8_t *payload, uint8_t payload_len)
{
    switch (cmd) {
        case API_CMD_GET_STATUS:
            send_status(t);
            break;
        case API_CMD_REQUEST_SINGLE:
            svc_measurement_trigger();
            send_ack(t);
            break;
        case API_CMD_CANCEL_SINGLE:
            svc_measurement_cancel();
            send_ack(t);
            break;
        case API_CMD_START_STREAM:
            s_t[t].mode           = API_MODE_STREAM;
            s_t[t].last_notify_ms = hal_systick_get_ms();
            send_ack(t);
            break;
        case API_CMD_STOP_STREAM:
            if (s_t[t].mode == API_MODE_STREAM) s_t[t].mode = API_MODE_IDLE;
            send_ack(t);
            break;
        case API_CMD_START_RAW_STREAM:
            s_t[t].mode           = API_MODE_RAW_STREAM;
            s_t[t].last_notify_ms = hal_systick_get_ms();
            send_ack(t);
            break;
        case API_CMD_STOP_RAW_STREAM:
            if (s_t[t].mode == API_MODE_RAW_STREAM) s_t[t].mode = API_MODE_IDLE;
            send_ack(t);
            break;
        case API_CMD_START_DISP_STREAM:
            if (t != API_TRANSPORT_USB) {
                /* ~2604 cycles/sec * sizeof(ApiDispRecord) (18 bytes) =
                 * ~46.9 KB/s of payload data alone -- far beyond BLE/
                 * UART's 115200 baud (~11.5 KB/s). NACK rather than
                 * silently deliver a stream that's actually dropping the
                 * vast majority of its cycles. Revisit this per-command
                 * special case if/when a second high-rate stream is
                 * added (see svc_api.h's ApiMode comment) -- worth a
                 * shared transport-capability check at that point rather
                 * than a third copy of this same NACK block. */
                send_nack(t);
                break;
            }
            s_t[t].mode = API_MODE_DISP_STREAM;
            send_ack(t);
            break;
        case API_CMD_STOP_DISP_STREAM:
            if (s_t[t].mode == API_MODE_DISP_STREAM) s_t[t].mode = API_MODE_IDLE;
            send_ack(t);
            break;
        case API_CMD_SET_ZERO:
            /* Sensors not yet fused into g_system_state — stub: accept
             * the command. A future WP implements actual zero-offset
             * capture. */
            if (svc_storage_is_busy()) {
                /* Another save already in flight (e.g. a local-UI edit
                 * mid-commit) — transient contention, not a failure.
                 * NACK without touching settings_save_failed. */
                send_nack(t);
            } else {
                handle_save_result(t, svc_storage_save_calibration(&g_calibration));
            }
            break;
        case API_CMD_GET_CALIBRATION:
            send_calibration(t);
            break;
        case API_CMD_SET_CALIBRATION:
            if (payload_len != sizeof(CalibrationData)) {
                send_nack(t);
                break;
            }
            if (svc_storage_is_busy()) {
                send_nack(t);
                break;
            }
            memcpy(&g_calibration, payload, sizeof g_calibration);
            /* Recompute immediately — this is otherwise only ever
             * set once at boot in svc_storage_init(), so GET_STATUS
             * would keep reporting the pre-boot value forever after
             * a runtime calibration change. */
            g_system_state.calibration_valid =
                g_calibration.scale_valid && g_calibration.zero_valid;
            handle_save_result(t, svc_storage_save_calibration(&g_calibration));
            break;
        case API_CMD_GET_SETTINGS:
            send_settings(t);
            break;
        case API_CMD_SET_SETTINGS:
            if (payload_len != sizeof(DeviceSettings)) {
                send_nack(t);
                break;
            }
            if (svc_storage_is_busy()) {
                send_nack(t);
                break;
            }
            memcpy(&g_device_settings, payload, sizeof g_device_settings);
            /* Untrusted host payload — re-run the same zero-guard
             * svc_storage_init() applies on every EEPROM load, or a
             * malformed divisor field (e.g. encoder_counts_per_detent)
             * silently deadens whatever consumer divides by it. */
            svc_storage_validate_settings(&g_device_settings);
            {
                DrvStatus rc = svc_storage_save_settings(&g_device_settings);
                if (rc == DRV_OK) {
                    /* Reload task periods only — app_scheduler_init() is
                     * boot-only (guards against re-entrant last_run_ms
                     * resets, see its own comment) and would silently
                     * no-op here since svc_api_update() only ever runs
                     * post-boot. */
                    app_scheduler_reload_periods();
                }
                handle_save_result(t, rc);
            }
            break;
        case API_CMD_GET_IDENTITY:
            send_identity(t);
            break;
        default:
            send_nack(t);
            break;
    }
}

/* ---------------- public API ---------------- */

void svc_api_init(void)
{
    memset(s_t, 0, sizeof s_t);
}

void svc_api_register_transport(ApiTransport t, ApiSendFn send_fn)
{
    if (t >= API_TRANSPORT_COUNT) return;
    s_t[t].send_fn = send_fn;
}

void svc_api_connected(ApiTransport t)
{
    if (t >= API_TRANSPORT_COUNT) return;
    s_t[t].connected      = true;
    s_t[t].mode           = API_MODE_IDLE;
    s_t[t].last_notify_ms = hal_systick_get_ms();
}

void svc_api_disconnected(ApiTransport t)
{
    if (t >= API_TRANSPORT_COUNT) return;
    s_t[t].connected = false;
    s_t[t].mode      = API_MODE_IDLE;
}

void svc_api_receive(ApiTransport t, const uint8_t *data, uint16_t len)
{
    if (t >= API_TRANSPORT_COUNT)         return;
    if (data == 0 || len < HDR_BYTES + CRC_BYTES) return;

    uint8_t cmd     = data[0];
    uint8_t paylen  = data[1];
    if ((uint16_t)(HDR_BYTES + paylen + CRC_BYTES) > len) {
        send_nack(t);
        return;
    }
    uint16_t calc_crc = math_crc16(data, (uint16_t)(HDR_BYTES + paylen));
    uint16_t got_crc  = (uint16_t)(data[HDR_BYTES + paylen + 0U]
                                   | (data[HDR_BYTES + paylen + 1U] << 8));
    if (calc_crc != got_crc) {
        send_nack(t);
        return;
    }
    dispatch(t, cmd, paylen ? &data[HDR_BYTES] : 0, paylen);
}

void svc_api_reassembler_feed_byte(ApiTransport t, ApiByteReassembler *r, uint8_t b)
{
    if (r->pos == 0) {
        r->started_ms = hal_systick_get_ms();
    }
    if (r->pos < API_PACKET_MAX_SIZE) {
        r->buf[r->pos++] = b;
    }

    /* As soon as we have header + length, we know how big the packet is. */
    if (r->pos >= HDR_BYTES) {
        uint16_t paylen = r->buf[1];
        uint16_t total  = (uint16_t)(HDR_BYTES + paylen + CRC_BYTES);
        if (total > API_PACKET_MAX_SIZE) {
            /* Garbage — drop. */
            r->pos = 0;
        } else if (r->pos >= total) {
            svc_api_receive(t, r->buf, total);
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

void svc_api_notify_single_ready(void)
{
    /* Send to whichever transport(s) are connected. The host that issued
     * the trigger is responsible for ignoring duplicates — simpler than
     * tracking origin. */
    for (ApiTransport t = 0; t < API_TRANSPORT_COUNT; ++t) {
        if (s_t[t].connected) {
            send_single_ready(t);
        }
    }
    svc_measurement_acknowledge();
}

ApiMode svc_api_get_mode(ApiTransport t)
{
    if (t >= API_TRANSPORT_COUNT) return API_MODE_IDLE;
    return s_t[t].mode;
}

void svc_api_update(void)
{
    uint32_t now = hal_systick_get_ms();
    for (ApiTransport t = 0; t < API_TRANSPORT_COUNT; ++t) {
        if (!s_t[t].connected) continue;

        switch (s_t[t].mode) {
            case API_MODE_STREAM:
                if ((uint32_t)(now - s_t[t].last_notify_ms)
                    >= g_device_settings.stream_interval_ms) {
                    send_stream_data(t);
                    s_t[t].last_notify_ms = now;
                }
                break;
            case API_MODE_RAW_STREAM:
                if ((uint32_t)(now - s_t[t].last_notify_ms)
                    >= g_device_settings.task_sensors_ms) {
                    send_raw_stream_data(t);
                    s_t[t].last_notify_ms = now;
                }
                break;
            default:
                break;
        }

        /* SINGLE in flight — emit progress every 500 ms */
        MeasurementState ms = svc_measurement_get_state();
        if (ms == MEAS_STATE_SETTLING || ms == MEAS_STATE_CAPTURING) {
            if ((uint32_t)(now - s_t[t].last_progress_ms) >= 500U) {
                send_single_progress(t, svc_measurement_get_progress_pct());
                s_t[t].last_progress_ms = now;
            }
        }
    }
}

void svc_api_disp_stream_update(void)
{
    /* Deliberately no elapsed-time gate (unlike svc_api_update()'s other
     * modes) -- the output ring's occupancy is the natural pacing signal
     * here, and this needs draining every tick to keep up with ~2.6 kHz
     * production (see svc_api.h's doc comment and app_scheduler.c's
     * task_api_disp_stream). */
    for (ApiTransport t = 0; t < API_TRANSPORT_COUNT; ++t) {
        if (!s_t[t].connected)                continue;
        if (s_t[t].mode != API_MODE_DISP_STREAM) continue;
        send_disp_stream_data(t);
    }
}
