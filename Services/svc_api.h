#ifndef SVC_API_H
#define SVC_API_H

#include <stdint.h>
#include <stdbool.h>

/* Device API v2 -- see docs/api-v2-spec.md for the design rationale and
 * docs/api-reference.md for the host-facing contract. This header is the
 * implementation-side counterpart: opcode/status enums, packet framing
 * constants, and the transport-facing entry points. Transports
 * (Services/svc_usb.c / svc_ble.c) never need anything beyond what's
 * declared here -- all protocol logic lives in svc_api.c.
 *
 * Ported onto master from the wp11-api-v2 branch 2026-09-05. Trimmed to
 * what this (REV B, hardware-validated) build can actually back:
 * System status, Commands, Measurements (onboard temp + battery only),
 * Settings (every DeviceSettings field), and a new Debug-messages log
 * stream (§0x6). Calibrations and WP7-11 sensor resources stay on the
 * wp11-api-v2 branch until those drivers come across and get bench time. */

typedef enum {
    API_TRANSPORT_USB  = 0,
    API_TRANSPORT_BLE  = 1,
    API_TRANSPORT_UART = 2,   /* USART3 debug/VCP header — see Services/svc_uart.c */
    API_TRANSPORT_COUNT,
} ApiTransport;

/* `urgent` distinguishes a direct response to a host request (true — may
 * use a transport's reserved TX space) from a subscription/stream push
 * (false — must leave the reserve free so a response can always get out).
 * See Services/svc_txframe.h. */
typedef void (*ApiSendFn)(const uint8_t *data, uint16_t len, bool urgent);

/* Optional per-transport back-pressure hook: returns true when the
 * transport's TX ring can take at least one more full-size (non-urgent)
 * frame without eating the reserve. Only the bulk-transfer pump consults
 * it — it paces chunk output to the wire instead of blindly filling the
 * ring and dropping chunks. A transport that doesn't register one is
 * treated as always-ready (best-effort; USB does this). */
typedef bool (*ApiReadyFn)(void);

void svc_api_init(void);
void svc_api_update(void);   /* scheduler hook — drains the debug-log push and the bulk-transfer pump */

void svc_api_register_transport(ApiTransport t, ApiSendFn send_fn);
void svc_api_register_transport_ready(ApiTransport t, ApiReadyFn ready_fn);
void svc_api_connected(ApiTransport t);
void svc_api_disconnected(ApiTransport t);

void svc_api_receive(ApiTransport t, const uint8_t *data, uint16_t len);

/* Walks all Measurements subscription slots and pushes the ones that are
 * due. Its own scheduler task (not folded into svc_api_update()) so a
 * 50 ms subscription interval isn't silently coarsened to the slower
 * generic poll cadence -- see the .c comment. */
void svc_api_measurement_subscriptions_update(void);

/* Same, for Topic groups (0x5) subscriptions. Its own scheduler hook for
 * the same reason as the Measurements one. */
void svc_api_topic_subscriptions_update(void);

/* ---------------- packet framing (docs/api-v2-spec.md §2) ----------------
 * [OPCODE 2B LE][LEN 2B LE][PAYLOAD 0..LEN][CRC16 2B LE], no padding.
 * Total on the wire is always 6 + LEN. */
#define API2_PACKET_HDR_BYTES   4U   /* OPCODE(2) + LEN(2) */
#define API2_PACKET_CRC_BYTES   2U
#define API2_PACKET_MAX_SIZE    128U /* reassembly ceiling; multi-frame chaining deferred */

typedef struct {
    uint8_t  buf[API2_PACKET_MAX_SIZE];
    uint16_t pos;
    uint32_t started_ms;
} ApiByteReassembler;

/* Feed received bytes one at a time; a complete CRC-framed packet is
 * dispatched via svc_api_receive() and the reassembler resets. Call
 * svc_api_reassembler_check_timeout() separately to abandon a stalled
 * partial packet. */
void svc_api_reassembler_feed_byte(ApiTransport t, ApiByteReassembler *r, uint8_t b);
void svc_api_reassembler_check_timeout(ApiByteReassembler *r, uint32_t timeout_ms);

/* ---------------- opcode structure (docs/api-v2-spec.md §3) ----------------
 * 16 bits: [VERB:4][CATEGORY:4][RESOURCE INDEX:8], VERB in the top nibble. */
typedef enum {
    API2_VERB_GET         = 0x0U,
    API2_VERB_SET         = 0x1U,
    API2_VERB_EXECUTE     = 0x2U,
    API2_VERB_SUBSCRIBE   = 0x3U,
    API2_VERB_UNSUBSCRIBE = 0x4U,
    API2_VERB_START_BULK  = 0x5U,
    API2_VERB_CANCEL_BULK = 0x6U,
} Api2Verb;

typedef enum {
    API2_CAT_SYSTEM_STATUS = 0x0U,
    API2_CAT_COMMANDS      = 0x1U,
    API2_CAT_CALIBRATIONS  = 0x2U,
    API2_CAT_SETTINGS      = 0x3U,
    API2_CAT_MEASUREMENTS  = 0x4U,
    API2_CAT_TOPIC_GROUPS  = 0x5U,
    API2_CAT_DEBUG_MSGS    = 0x6U,
    API2_CAT_RAW_DATA      = 0x7U,
    API2_CAT_BULK          = 0x8U,
} Api2Category;

#define API2_OPCODE(verb, cat, res) \
    ((uint16_t)((((uint16_t)(verb) & 0xFU) << 12) \
              | (((uint16_t)(cat)  & 0xFU) << 8)  \
              |  ((uint16_t)(res)  & 0xFFU)))
#define API2_OPCODE_VERB(op)     ((uint8_t)(((op) >> 12) & 0xFU))
#define API2_OPCODE_CATEGORY(op) ((uint8_t)(((op) >> 8)  & 0xFU))
#define API2_OPCODE_RESOURCE(op) ((uint8_t)((op) & 0xFFU))

/* ---------------- status codes (docs/api-v2-spec.md §5) ---------------- */
typedef enum {
    API2_STATUS_OK                = 0x00U,
    API2_STATUS_UNKNOWN_CATEGORY  = 0x01U,
    API2_STATUS_VERB_NOT_VALID    = 0x02U,
    API2_STATUS_UNKNOWN_RESOURCE  = 0x03U,
    API2_STATUS_BAD_CRC           = 0x04U,
    API2_STATUS_BAD_LENGTH        = 0x05U,
    API2_STATUS_BUSY_RESOURCE     = 0x06U,
    API2_STATUS_BUSY_EXCLUSIVE    = 0x07U,
    API2_STATUS_INVALID_PARAMETER = 0x08U,
    API2_STATUS_NOT_SUBSCRIBED    = 0x09U,
    API2_STATUS_NOTHING_TO_CANCEL = 0x0AU,
} Api2Status;

/* ---------------- System status (0x0) ----------------
 * 0x00 Identity, 0x01 Device state — GET only.
 * 0x02 RTC datetime — GET and SET (the one writable system-status
 *      resource). GET response payload (9 B): year u16 LE, month, day,
 *      weekday (1=Mon..7=Sun), hour, minute, second, is_set (0/1).
 *      SET request payload (7 B): year u16 LE, month, day, hour, minute,
 *      second — weekday is recomputed. */
#define API2_RES_SYS_IDENTITY      0x00U
#define API2_RES_SYS_DEVICE_STATE  0x01U
#define API2_RES_SYS_RTC           0x02U

#define API2_OP_SYS_GET_IDENTITY \
    API2_OPCODE(API2_VERB_GET, API2_CAT_SYSTEM_STATUS, API2_RES_SYS_IDENTITY)
#define API2_OP_SYS_GET_DEVICE_STATE \
    API2_OPCODE(API2_VERB_GET, API2_CAT_SYSTEM_STATUS, API2_RES_SYS_DEVICE_STATE)
#define API2_OP_SYS_GET_RTC \
    API2_OPCODE(API2_VERB_GET, API2_CAT_SYSTEM_STATUS, API2_RES_SYS_RTC)
#define API2_OP_SYS_SET_RTC \
    API2_OPCODE(API2_VERB_SET, API2_CAT_SYSTEM_STATUS, API2_RES_SYS_RTC)

/* ---------------- Commands (0x1, EXECUTE only) ----------------
 * 0x00 Test beep — no payload.
 * 0x01 Signal analysis — 1-byte payload: 0 = stop the ADS131M04 sample
 *      stream + DFT, 1 = start it. Off at boot (v0.8.2); see
 *      Services/svc_signal_analysis.h for why.
 * 0x02 Force charge — no payload. Enables the charger regardless of SOC
 *      while USB is present (a one-shot overnight top-off); self-clears on
 *      full or USB removal. No-op with no USB. See svc_battery.h. */
#define API2_RES_CMD_TEST_BEEP        0x00U
#define API2_RES_CMD_SIGNAL_ANALYSIS  0x01U
#define API2_RES_CMD_FORCE_CHARGE     0x02U

#define API2_OP_CMD_TEST_BEEP \
    API2_OPCODE(API2_VERB_EXECUTE, API2_CAT_COMMANDS, API2_RES_CMD_TEST_BEEP)
#define API2_OP_CMD_SIGNAL_ANALYSIS \
    API2_OPCODE(API2_VERB_EXECUTE, API2_CAT_COMMANDS, API2_RES_CMD_SIGNAL_ANALYSIS)
#define API2_OP_CMD_FORCE_CHARGE \
    API2_OPCODE(API2_VERB_EXECUTE, API2_CAT_COMMANDS, API2_RES_CMD_FORCE_CHARGE)

/* ---------------- Measurements (0x4: GET, SUBSCRIBE, UNSUBSCRIBE) ----------------
 * Only what REV B actually reads today. All are subscribable. */
#define API2_RES_MEAS_ONBOARD_TEMP   0x00U   /* int16 centi-degC, TMP236 */
#define API2_RES_MEAS_BATTERY_MV     0x01U   /* uint16 mV */
#define API2_RES_MEAS_BATTERY_SOC    0x02U   /* uint8 percent */
/* BME280 (WP9), I2C1. All read from g_system_state.bme280_*; report the
 * last value even when the sensor is absent/stale — pair with
 * `System status` DEVICE_STATE or a dedicated flag to know freshness
 * (bme280_ok). */
#define API2_RES_MEAS_BME280_TEMP    0x03U   /* int16  centi-degC (0.01 degC/LSB) */
#define API2_RES_MEAS_BME280_PRESS   0x04U   /* uint32 Pa */
#define API2_RES_MEAS_BME280_HUMID   0x05U   /* uint16 centi-%RH (0.01 %RH/LSB) */
#define API2_RES_MEAS_BME280_OK      0x06U   /* uint8 0/1 — is the last reading fresh */
/* LM35 external temperature (WP11), TEMP_SENSE_EXT / PB11. */
#define API2_RES_MEAS_EXT_TEMP       0x07U   /* int16 centi-degC */
#define API2_RES_MEAS_EXT_TEMP_OK    0x08U   /* uint8 0/1 — in-range reading present */

#define API2_MEASUREMENT_MIN_INTERVAL_MS 50U
#define API2_MEASUREMENT_MAX_INTERVAL_MS 3600000U   /* 1 hour */
#define API2_MEASUREMENT_SLOTS           16U        /* direct-indexed by resource id */

/* ---------------- Topic groups (0x5: GET, SUBSCRIBE, UNSUBSCRIBE) ----------------
 * Fixed compile-time bundles of related values — subscribe once instead
 * of to many individual Measurements resources. SUBSCRIBE payload is a
 * 4-byte LE interval_ms (same range/rules as Measurements). GET responses
 * and subscription pushes carry the same packed little-endian layout;
 * pushes are prefixed with [issue_seq][page=0] like every other stream.
 *
 * 0x00 Environmental — temperature / atmosphere (14 B):
 *   int16  bme280_temp_cdeg      (0.01 degC)
 *   uint32 bme280_pressure_pa    (Pa)
 *   uint16 bme280_humidity_cpct  (0.01 %RH)
 *   uint8  bme280_ok
 *   int16  onboard_temp_cdeg     (TMP236, g_system_state.temperature_cdeg)
 *   int16  external_temp_cdeg    (LM35, TEMP_SENSE_EXT)
 *   uint8  external_temp_ok      (0 if out of range / no sensor)
 *
 * 0x01 Device status — the "inner workings" (18 B):
 *   uint16 battery_mv
 *   uint8  battery_soc_pct
 *   uint8  battery_state         (battery_state_t)
 *   uint8  usb_connected
 *   uint8  ble_connected
 *   uint8  charging              (TP4056 CHRG line)
 *   uint8  force_charging        (manual override armed)
 *   uint8  rail_3v3_on
 *   uint8  rail_5v_on
 *   uint16 rtc_year
 *   uint8  rtc_month, rtc_day, rtc_hour, rtc_minute, rtc_second
 *   uint8  rtc_set               (1 once the clock has ever been set)
 */
#define API2_RES_TOPIC_ENV      0x00U
#define API2_RES_TOPIC_STATUS   0x01U
#define API2_TOPIC_SLOTS        4U          /* direct-indexed by resource id */

/* ---------------- Settings (0x3: GET, SET) ----------------
 * Indices in DeviceSettings field order (the pad `battery_page_reserved`
 * is not a resource). */
#define API2_RES_SET_TASK_SENSORS_MS         0x00U
#define API2_RES_SET_TASK_PROCESSING_MS      0x01U
#define API2_RES_SET_TASK_DISPLAY_MS         0x02U
#define API2_RES_SET_TASK_BLE_MS             0x03U
#define API2_RES_SET_TASK_USB_MS             0x04U
#define API2_RES_SET_TASK_BATTERY_MS         0x05U
#define API2_RES_SET_TASK_TEMPERATURE_MS     0x06U
#define API2_RES_SET_STREAM_INTERVAL_MS      0x07U
#define API2_RES_SET_SETTLING_THRESHOLD      0x08U
#define API2_RES_SET_SETTLING_TIMEOUT_MS     0x09U
#define API2_RES_SET_FILTER_CUTOFF_HZ_NUM    0x0AU
#define API2_RES_SET_FILTER_CUTOFF_HZ_DEN    0x0BU
#define API2_RES_SET_BATTERY_CRITICAL_MV     0x0CU
#define API2_RES_SET_BATTERY_LOW_MV          0x0DU
#define API2_RES_SET_BATTERY_CHARGE_START_MV 0x0EU
#define API2_RES_SET_VBAT_SCALE_NUM          0x0FU
#define API2_RES_SET_VBAT_SCALE_DEN          0x10U
#define API2_RES_SET_TMP236_SEG1_VOFFS_MV    0x11U
#define API2_RES_SET_TMP236_SEG1_NUM         0x12U
#define API2_RES_SET_TMP236_SEG1_DEN         0x13U
#define API2_RES_SET_TMP236_SEG_BOUNDARY_MV  0x14U
#define API2_RES_SET_TMP236_SEG2_VOFFS_MV    0x15U
#define API2_RES_SET_TMP236_SEG2_NUM         0x16U
#define API2_RES_SET_TMP236_SEG2_DEN         0x17U
#define API2_RES_SET_TMP236_SEG2_TINFL_CDEG  0x18U
#define API2_RES_SET_LM35_SCALE_MV_PER_C     0x19U
#define API2_RES_SET_ENCODER_COUNTS_PER_DET  0x1AU
/* 0x1B (WP6): auto_poweroff_s — idle seconds before auto power-off, 0 =
 * disabled. Appended, not slotted into struct order, so the indices above
 * keep their wire values (its DeviceSettings field sits mid-struct, in
 * the battery page). u16, range 0..65535. */
#define API2_RES_SET_AUTO_POWEROFF_S         0x1BU

/* ---------------- Debug messages (0x6: SUBSCRIBE, UNSUBSCRIBE only) ----------------
 * A live log stream. SUBSCRIBE payload is one byte: the minimum severity
 * to receive (>= this level). No GET -- a stream has no "current value".
 * Push payload: [status=OK][issue_seq][page=0][severity][message bytes]. */
#define API2_RES_DEBUG_LOG_STREAM   0x00U

typedef enum {
    API2_LOG_INFO  = 0x00U,
    API2_LOG_WARN  = 0x01U,
    API2_LOG_ERROR = 0x02U,
} Api2LogSeverity;

/* ---------------- Raw data (0x7: GET) ----------------
 * Development/debug intermediate values. 0x00 = ADS131M04 diagnostics,
 * GET only, no request payload. Response payload (24 B, LE):
 *   u16 reg_id, reg_status, reg_mode, reg_clock, reg_gain1, reg_cfg
 *   u16 clock_expected      (what the driver wrote to CLOCK)
 *   u8  regs_read_ok        (all RREG transfers succeeded)
 *   u8  ads_ok              (g_system_state.ads_ok)
 *   u16 last_capture_samples
 *   u16 last_capture_drops
 *   u32 last_capture_elapsed_ms
 * CLOCK.OSR is bits [4:2]: 0=128,1=256,2=512,3=1024,4=2048,5=4096,6=8192,7=16256;
 * fDATA = fCLKIN / (2 * OSR), fCLKIN ~= 5.3333 MHz. */
#define API2_RES_RAW_ADC_DIAG   0x00U

#define API2_OP_RAW_ADC_DIAG \
    API2_OPCODE(API2_VERB_GET, API2_CAT_RAW_DATA, API2_RES_RAW_ADC_DIAG)

/* ---------------- Bulk transfers (0x8: START_BULK, CANCEL_BULK) ----------------
 * 0x00 Raw ADC capture. START_BULK: no request payload (the transfer size
 *      is fixed and known from this doc, spec §4.5). Ack is a bare status
 *      byte. Then Config/config.h ADC_BULK_SAMPLE_COUNT samples are
 *      streamed as chunk packets under the same opcode, each:
 *        [status=OK][page:1][sample:12]xN,  N <= ADC_BULK_CHUNK_SAMPLES
 *      one sample = ch0,ch1,ch2,ch3 each as a 3-byte little-endian signed
 *      24-bit ADC code (1 LSB = 2.4 V / 2^23, gain 1). `page` is the wrapping
 *      chunk counter (§2.3) for gap detection. The host knows it's done
 *      when it has ADC_BULK_SAMPLE_COUNT samples; on a CRC error or gap it
 *      CANCEL_BULKs and restarts (§4.5, no per-chunk resend).
 *      Exclusivity: device-wide, one bulk at a time; also NACKs
 *      BUSY_EXCLUSIVE while the real-time signal-analysis stream is running
 *      and BUSY_RESOURCE if the ADS131M04 failed to init. */
#define API2_RES_BULK_RAW_ADC   0x00U

#define API2_OP_BULK_RAW_ADC_START \
    API2_OPCODE(API2_VERB_START_BULK, API2_CAT_BULK, API2_RES_BULK_RAW_ADC)
#define API2_OP_BULK_RAW_ADC_CANCEL \
    API2_OPCODE(API2_VERB_CANCEL_BULK, API2_CAT_BULK, API2_RES_BULK_RAW_ADC)

#endif /* SVC_API_H */
