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
 * stream (§0x6). Calibrations (§0x2) added 2026-09-24 for WP10's
 * displacement demodulation — the first resources in that category. */

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

/* Called once after a host SET of a Settings resource has been persisted,
 * so an upper layer can re-apply anything derived from DeviceSettings
 * (App wires app_scheduler_reload_periods here). Keeps svc_api from having
 * to #include an App-layer header (CLAUDE.md §8.1 — deps flow downward). */
typedef void (*ApiSettingsChangedFn)(void);
void svc_api_register_settings_changed(ApiSettingsChangedFn fn);
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
 * 0x01 Displacement — 1-byte payload: 0 = stop the ADS131M04 sample
 *      stream + WP10 per-cycle demodulation, 1 = start it. Off at boot
 *      (same bring-up caution WP8's original signal-analysis toggle had —
 *      running the pipeline unconditionally at boot starved the
 *      cooperative scheduler); see Services/svc_displacement.h.
 * 0x02 Force charge — no payload. Enables the charger regardless of SOC
 *      while USB is present (a one-shot overnight top-off); self-clears on
 *      full or USB removal. No-op with no USB. See svc_battery.h.
 * 0x03 Power test — 4-byte payload `u32 mask` LE. Diagnostic: each bit
 *      keeps one subsystem/rail/clock on, cleared bits cut it immediately
 *      (see Services/svc_powertest.h for the bit map). Applied live; the
 *      resulting mask is echoed back and also readable via Raw data 0x01.
 *      Default at boot is all bits set (normal behaviour). */
#define API2_RES_CMD_TEST_BEEP        0x00U
#define API2_RES_CMD_DISPLACEMENT     0x01U   /* was API2_RES_CMD_SIGNAL_ANALYSIS (WP8) — same
                                                  wire value, repointed at WP10 2026-09-24 */
#define API2_RES_CMD_FORCE_CHARGE     0x02U
#define API2_RES_CMD_POWER_TEST       0x03U
/* 0x04 Pin test — 1-byte payload. bits[5:0] drive the 6 MCU->level-
 * converter signals (0 SCK / 1 MOSI / 2 CS / 3 DISP_ON / 4 VCOM /
 * 5 BUZZER) as static push-pull outputs; bit6 = allow DISP_ON high
 * (panel MUST be unplugged); bit7 = reboot to normal. Arming is
 * irreversible without the reboot. See HAL_App/hal_pintest.h. */
#define API2_RES_CMD_PIN_TEST         0x04U
/* 0x05 Reboot to DFU — 0-byte payload. Sets the nBOOT0 option byte to 0
 * and launches an option-byte reload; the device boots into the ROM
 * bootloader (USB DFU, VID 0x0483 / PID 0xDF11 on PA11/PA12) and STAYS
 * there on every subsequent boot — a power-cycle does NOT recover the
 * app. Recover by reflashing with nBOOT0 restored, e.g.
 *   STM32_Programmer_CLI -c port=USB1 -w fw.hex -ob nSWBOOT0=1 nBOOT0=1 -v -rst
 * (or PythonTestCode/dfu_flash.ps1). See HAL_App/hal_dfu.h. */
#define API2_RES_CMD_REBOOT_DFU       0x05U
/* 0x06 Zero calibration — 1-byte payload, the classic 180-degree
 * reversal test (Config/config.h's "Displacement zero calibration"
 * comment has the derivation):
 *   0x00 cancel — abort an in-progress run, no-op if already idle.
 *   0x01 step 1 — place the instrument, then EXECUTE this. Averages
 *        DISPLACEMENT_ZERO_CAL_SAMPLES batches at the current
 *        orientation. Requires the demod already running (Commands
 *        API2_RES_CMD_DISPLACEMENT) — BUSY_RESOURCE otherwise.
 *   0x02 step 2 — after physically rotating the instrument 180 degrees,
 *        EXECUTE this. Same averaging at the new orientation, then
 *        computes and PERSISTS new disp_s1/s2_zero_offset_um values to
 *        this instrument's own EEPROM (Calibrations 0x2, resources
 *        0x03/0x06) — BUSY_RESOURCE if step 1 hasn't finished yet.
 * Each EXECUTE just starts/cancels a step and acks immediately — the
 * averaging itself takes ~1.6 s per step (see config.h). Poll progress
 * via Raw data (0x7) resource 0x03; a GET on Calibrations 0x2 after step
 * 2 completes reads back the new persisted offsets. Entirely local to
 * this physical instrument: reads/writes only this device's own
 * g_device_settings and EEPROM, never anything shared across units. */
#define API2_RES_CMD_ZERO_CAL         0x06U
/* 0x07 Triggered precision measurement (2026-09-26) -- the "user
 * triggers it, device takes the time it needs, reports one reliable
 * value" mode, as opposed to the continuous live/streaming readout
 * (Topic groups 0x3). 1-byte payload:
 *   0x00 start  -- begin averaging quality-good batches per sensor
 *        (Config/config.h's DISPLACEMENT_QUALITY_BAD_MULTIPLE) up to
 *        DISPLACEMENT_PRECISION_TARGET_SAMPLES each, stopping once BOTH
 *        sensors reach the target or DISPLACEMENT_PRECISION_TIMEOUT_MS
 *        elapses (~3.2 s typical/clean, 4 s worst case -- see that
 *        constant's comment for the full ~2s-vs-64-samples tradeoff).
 *        Restarts a fresh run if one was already in progress. Requires
 *        the demod already running (Commands 0x01) and no zero-cal (0x06)
 *        in progress -- BUSY_RESOURCE otherwise.
 *   0x01 cancel -- abort an in-progress run, no-op if already idle/done.
 * EXECUTE acks immediately; poll progress/result via Raw data (0x7)
 * resource 0x04. Unlike zero-cal, the result never touches EEPROM/
 * settings -- it's a pure read-back, re-readable any number of times
 * once done, no separate "consume" step. */
#define API2_RES_CMD_PRECISION_MEASURE 0x07U

#define API2_OP_CMD_TEST_BEEP \
    API2_OPCODE(API2_VERB_EXECUTE, API2_CAT_COMMANDS, API2_RES_CMD_TEST_BEEP)
#define API2_OP_CMD_DISPLACEMENT \
    API2_OPCODE(API2_VERB_EXECUTE, API2_CAT_COMMANDS, API2_RES_CMD_DISPLACEMENT)
#define API2_OP_CMD_FORCE_CHARGE \
    API2_OPCODE(API2_VERB_EXECUTE, API2_CAT_COMMANDS, API2_RES_CMD_FORCE_CHARGE)
#define API2_OP_CMD_POWER_TEST \
    API2_OPCODE(API2_VERB_EXECUTE, API2_CAT_COMMANDS, API2_RES_CMD_POWER_TEST)
#define API2_OP_CMD_ZERO_CAL \
    API2_OPCODE(API2_VERB_EXECUTE, API2_CAT_COMMANDS, API2_RES_CMD_ZERO_CAL)
#define API2_OP_CMD_REBOOT_DFU \
    API2_OPCODE(API2_VERB_EXECUTE, API2_CAT_COMMANDS, API2_RES_CMD_REBOOT_DFU)
#define API2_OP_CMD_PRECISION_MEASURE \
    API2_OPCODE(API2_VERB_EXECUTE, API2_CAT_COMMANDS, API2_RES_CMD_PRECISION_MEASURE)

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
/* Displacement (WP10), Services/svc_displacement.c. delta_mm is the
 * POST-moving-average value (config.h's DISPLACEMENT_MA_SAMPLES, added
 * 2026-09-26 -- a boxcar smoothing stage over the last few completed
 * batches, applied on top of the coherent per-batch sum config.h's
 * DISPLACEMENT_BATCH_CYCLES already does; ~20.3 raw batches/s at the
 * default batch size, but the smoothed value here updates no faster than
 * that regardless). Want the raw, pre-MA per-batch value instead (e.g.
 * for granular noise analysis)? Subscribe to Topic groups (0x5)
 * API2_RES_TOPIC_RAW_DISPLACEMENT below instead of polling this.
 * float32 LE, IEEE-754 — the first floats on this wire; MEAS_VALUE_MAX_LEN
 * is 4 bytes, an exact fit. Both only meaningful while disp_ok is true —
 * GET/SUBSCRIBE that first if freshness matters, same pattern as
 * bme280_ok above. residual is Im(x) — never had the MA applied (it
 * already averages near 0) — should sit near 0 if the Calibrations (0x2)
 * page below holds up; a consistently nonzero residual usually means a
 * calibration constant is off, not a faulty sensor. */
#define API2_RES_MEAS_DISP1_DELTA_MM 0x09U   /* float32 mm, Sensor 1 (CH3) */
#define API2_RES_MEAS_DISP1_RESIDUAL 0x0AU   /* float32, Im(x1) diagnostic */
#define API2_RES_MEAS_DISP2_DELTA_MM 0x0BU   /* float32 mm, Sensor 2 (CH0) */
#define API2_RES_MEAS_DISP2_RESIDUAL 0x0CU   /* float32, Im(x2) diagnostic */
#define API2_RES_MEAS_DISP_OK        0x0DU   /* uint8 0/1 */

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
 *
 * 0x02 Displacement phasors — WP10 demod diagnostics (32 B, added
 * 2026-09-25, Config/config.h's "Displacement phasor diagnostics"
 * comment has the full reasoning for exposing these at all): the latest
 * completed batch's 8 raw I/Q phasors, one step upstream of Measurements
 * 0x09-0x0C's delta_mm/residual — a consistently-off phasor here (e.g.
 * the A/B pair not reading ~180 deg apart) points at a wiring or
 * calibration problem Measurements alone can't distinguish from "device
 * working, instrument tilted". All float32 LE, IEEE-754, valid only
 * while Measurements 0x0D (disp_ok) is true:
 *   float iB, qB     (Exciter B, CH1)
 *   float iA, qA     (Exciter A, CH2)
 *   float iS1, qS1   (Sensor 1, CH3)
 *   float iS2, qS2   (Sensor 2, CH0)
 *
 * 0x03 Raw displacement -- PRE-moving-average per-batch delta/residual
 * (18 B, added 2026-09-26 at the user's request for granular analysis of
 * the demod's raw output, before config.h's DISPLACEMENT_MA_SAMPLES
 * boxcar smoothing that Measurements 0x09/0x0B apply). Same ~20.3
 * updates/s source rate as the phasors topic above (both come from
 * process_one_batch(), config.h's DISPLACEMENT_BATCH_CYCLES); subscribe
 * at the API2_MEASUREMENT_MIN_INTERVAL_MS floor (50 ms) to track it
 * essentially 1:1 (batch period ~49.2 ms). Valid only while Measurements
 * 0x0D (disp_ok) is true:
 *   float delta1_mm_raw   (Sensor 1, pre-MA)
 *   float residual1       (Im(x1) -- identical to Measurements 0x0A,
 *                           never had MA applied to begin with)
 *   float delta2_mm_raw   (Sensor 2, pre-MA)
 *   float residual2       (identical to Measurements 0x0C)
 *   u8    quality1_ok      (svc_displacement_get_quality1_ok(), added
 *                            2026-09-26 -- bench-validated: this batch's
 *                            residual moved anomalously vs. its own
 *                            recent baseline, correlated with delta1
 *                            having one of the discrete "jumps" the
 *                            2026-09-26 noise investigation found. 0 =
 *                            treat this batch's delta1_mm_raw with
 *                            suspicion, not a hard guarantee of error)
 *   u8    quality2_ok      (same, for Sensor 2)
 */
#define API2_RES_TOPIC_ENV              0x00U
#define API2_RES_TOPIC_STATUS           0x01U
#define API2_RES_TOPIC_PHASORS          0x02U
#define API2_RES_TOPIC_RAW_DISPLACEMENT 0x03U
#define API2_TOPIC_SLOTS        4U          /* direct-indexed by resource id */

/* ---------------- Calibrations (0x2: GET, SET) ----------------
 * Sensor-correction constants — structurally identical to Settings
 * (0x3), same SF()-style integer field machinery (Services/svc_api.c),
 * just a separate category/EEPROM page per the API spec's own split
 * ("Sensor-correction constants: offsets, gains" vs Settings'
 * "Operational/behavioral config"). First resources to use this
 * category (WP10, 2026-09-24) — the wire values below are all new, no
 * gaps to preserve.
 *
 * Displacement (Services/svc_displacement.c) — milli-units (x1000) for
 * the two dimensionless ratios, micrometers for the two lengths, not
 * raw floats (see system_state.h's comment on these DeviceSettings
 * fields for why). All u32 payload (4 bytes), int32 signed for the
 * offsets. D0_UM's default was bumped 1000x 2026-09-26 as a coarse
 * sensitivity fix standing in for a still-missing gain calibration --
 * see Config/config.h's DEFAULT_DISP_S1_D0_UM comment; its wire bounds
 * (Services/svc_api.c's s_calibration_fields[]) widened to match, so it
 * no longer represents a literal sensor air gap in mm. */
#define API2_RES_CALIB_DISP_ATTEN_MILLI        0x00U   /* shared A/B attenuator, x1000 */
#define API2_RES_CALIB_DISP_S1_GAIN_MILLI      0x01U   /* S1 amplifier gain, x1000 */
#define API2_RES_CALIB_DISP_S1_D0_UM           0x02U   /* S1 neutral air gap, um (see 1000x-bump note above) */
#define API2_RES_CALIB_DISP_S1_ZERO_OFFSET_UM  0x03U   /* S1 zero calibration, um, signed */
#define API2_RES_CALIB_DISP_S2_GAIN_MILLI      0x04U   /* S2 amplifier gain, x1000 */
#define API2_RES_CALIB_DISP_S2_D0_UM           0x05U   /* S2 neutral air gap, um */
#define API2_RES_CALIB_DISP_S2_ZERO_OFFSET_UM  0x06U   /* S2 zero calibration, um, signed */

/* ---------------- Settings (0x3: GET, SET) ----------------
 * Resource IDs are stable wire values, not a dense sequence. 0x01 and
 * 0x07..0x0B are retired: they were the REV A task_processing_ms /
 * stream_interval_ms / settling_threshold / settling_timeout /
 * complementary-filter fields, removed with the REV A sensor stack. The
 * gaps stay so every surviving ID keeps its number. */
#define API2_RES_SET_TASK_SENSORS_MS         0x00U
/* 0x01 retired (task_processing_ms) */
#define API2_RES_SET_TASK_DISPLAY_MS         0x02U
#define API2_RES_SET_TASK_BLE_MS             0x03U
#define API2_RES_SET_TASK_USB_MS             0x04U
#define API2_RES_SET_TASK_BATTERY_MS         0x05U
#define API2_RES_SET_TASK_TEMPERATURE_MS     0x06U
/* 0x07..0x0B retired (stream_interval_ms, settling_threshold,
 * settling_timeout_ms, filter_cutoff_hz_num, filter_cutoff_hz_den) */
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
/* 0x1C: vbat_offset_mv — additive Vbat correction (bench cal), signed
 * int32 (4-byte payload), range -500..500 mV. Battery page, EEPROM
 * version 0x0004. */
#define API2_RES_SET_VBAT_OFFSET_MV          0x1CU

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
 * Development/debug intermediate values. 0x00 = ADS131M04 register
 * read-back + bulk-capture stats, GET only, no request payload. Response
 * payload (24 B base + 57 B acquisition-integrity tail, LE):
 *   u16 reg_id, reg_status, reg_mode, reg_clock, reg_gain1, reg_cfg
 *   u16 clock_expected      (what the driver wrote to CLOCK)
 *   u8  regs_read_ok        (all RREG transfers succeeded)
 *   u8  ads_ok              (g_system_state.ads_ok)
 *   u16 last_capture_samples     (svc_displacement_last_capture())
 *   u16 last_capture_drops
 *   u32 last_capture_elapsed_ms
 * CLOCK.OSR is bits [4:2]: 0=128,1=256,2=512,3=1024,4=2048,5=4096,6=8192,7=16256;
 * fDATA = fCLKIN / (2 * OSR), fCLKIN ~= 5.3333 MHz. */
#define API2_RES_RAW_ADC_DIAG   0x00U
/* 0x01 = power-test state. GET, no payload. Response (5 B):
 *   u32 mask   — current Commands/0x03 bitmask (svc_powertest.h)
 *   u8  flags  — bit0 3V3 rail on, bit1 5V rail on (read back from the pins) */
#define API2_RES_RAW_PWRTEST    0x01U
/* 0x02 = WP10 displacement demod diagnostics (split out from 0x00 when
 * bulk-capture's original last_capture fields were restored there
 * 2026-09-25 -- see the "Bulk transfers" comment below). GET, no request
 * payload. Response (13 B, LE):
 *   u16 disp_input_drop_count    (svc_displacement_get_input_drop_count())
 *   u16 disp_output_drop_count   (svc_displacement_get_output_drop_count())
 *   u16 disp_degenerate_count    (svc_displacement_get_degenerate_count())
 *   u8  disp_ok                  (svc_displacement_get_ok())
 *   u16 phasor_log_progress      (svc_displacement_phasor_log_progress() --
 *                                  entries stored so far in the current/most
 *                                  recent Bulk 0x8/0x01 capture, 0..
 *                                  DISPLACEMENT_PHASOR_LOG_DEPTH; lets a host
 *                                  poll progress instead of guessing)
 *   u16 clip_count                (svc_displacement_get_clip_count() -- a raw
 *                                  ADC code rode near a rail; added 2026-09-26
 *                                  alongside the S1/S2 PGA=16 bump)
 *   u16 amplitude_fault_count     (svc_displacement_get_amplitude_fault_count() --
 *                                  a batch's phasor exceeded what a full-scale,
 *                                  undistorted signal could produce; NOT a
 *                                  clipping indicator -- see
 *                                  Services/svc_displacement.h's getter comment) */
#define API2_RES_RAW_DISPLACEMENT_DIAG  0x02U
/* 0x03 = zero-calibration progress (Commands 0x06, see its comment for
 * the full procedure). GET, no request payload. Response (5 B, LE):
 *   u8  phase     (DisplacementZeroCalPhase: 0 idle, 1 step1 running,
 *                   2 step1 done (ready for step2), 3 step2 running,
 *                   4 result ready -- transient, svc_api_update() applies
 *                   and persists it within one tick, so a host polling
 *                   slower than that will normally see phase go straight
 *                   from 3 back to 0)
 *   u16 progress  (samples averaged so far in the CURRENT step, 0 while
 *                   idle or between steps)
 *   u16 target    (DISPLACEMENT_ZERO_CAL_SAMPLES, so a host doesn't need
 *                   to hardcode it) */
#define API2_RES_RAW_ZERO_CAL_STATUS    0x03U
/* 0x04 = triggered precision-measurement progress/result (Commands 0x07,
 * see its comment for the full procedure). GET, no request payload.
 * Response (20 B, LE):
 *   u8    phase       (DisplacementPrecisionPhase: 0 idle, 1 running, 2 done)
 *   u16   target       (DISPLACEMENT_PRECISION_TARGET_SAMPLES, so a host
 *                        doesn't need to hardcode it)
 *   u16   count1       (quality-good batches averaged so far for Sensor 1,
 *                        0..target)
 *   u16   count2       (same, for Sensor 2)
 *   u32   elapsed_ms   (wall-clock time since the EXECUTE that started
 *                        this run, 0 while idle)
 *   u8    timed_out     (1 if DISPLACEMENT_PRECISION_TIMEOUT_MS was hit
 *                        before both sensors reached target -- only
 *                        meaningful once phase == done)
 *   float delta1_mm    (mean of the count1 batches actually collected --
 *                        valid once phase == done; 0 before that)
 *   float delta2_mm    (same, for Sensor 2) */
#define API2_RES_RAW_PRECISION_STATUS   0x04U

#define API2_OP_RAW_ADC_DIAG \
    API2_OPCODE(API2_VERB_GET, API2_CAT_RAW_DATA, API2_RES_RAW_ADC_DIAG)
#define API2_OP_RAW_PWRTEST \
    API2_OPCODE(API2_VERB_GET, API2_CAT_RAW_DATA, API2_RES_RAW_PWRTEST)
#define API2_OP_RAW_DISPLACEMENT_DIAG \
    API2_OPCODE(API2_VERB_GET, API2_CAT_RAW_DATA, API2_RES_RAW_DISPLACEMENT_DIAG)
#define API2_OP_RAW_ZERO_CAL_STATUS \
    API2_OPCODE(API2_VERB_GET, API2_CAT_RAW_DATA, API2_RES_RAW_ZERO_CAL_STATUS)
#define API2_OP_RAW_PRECISION_STATUS \
    API2_OPCODE(API2_VERB_GET, API2_CAT_RAW_DATA, API2_RES_RAW_PRECISION_STATUS)

/* ---------------- Bulk transfers (0x8: START_BULK, CANCEL_BULK) ----------------
 * 0x00 Raw ADC capture. START_BULK: no request payload (the transfer size
 *      is fixed, Config/config.h ADC_BULK_SAMPLE_COUNT). Response is the
 *      usual [status] ack; the capture then runs in the background and the
 *      samples stream out asynchronously under the SAME opcode, chunked as
 *      [status=OK][page:1][sample:12]xN, N <= ADC_BULK_CHUNK_SAMPLES, page
 *      wrapping 0-255. CANCEL_BULK aborts an active capture/transfer.
 *      Exclusive with the real-time displacement demod (Commands/
 *      API2_RES_CMD_DISPLACEMENT) -- both want the ADS131M04's one
 *      sample-callback slot (Services/svc_displacement.c); START_BULK
 *      while displacement is running gets BUSY_EXCLUSIVE, and vice versa.
 *      Retired 2026-09-24 on the mistaken theory that the two could never
 *      coexist at all; restored 2026-09-25 once svc_displacement.c grew a
 *      capture-mode branch in its on_sample() that the demod math and the
 *      raw capture both share (mutually exclusively) instead of needing
 *      separate driver callbacks. */
#define API2_RES_BULK_RAW_ADC   0x00U

/* 0x01 Phasor log capture. Added 2026-09-25 (Config/config.h's
 *      "Displacement phasor diagnostics" comment) -- a longer-duration,
 *      decimated companion to 0x00 above: instead of ~0.3 s of every raw
 *      ADC sample, this captures Config/config.h DISPLACEMENT_PHASOR_LOG_DEPTH
 *      entries of the demod's batch-level phasors at every
 *      DISPLACEMENT_PHASOR_LOG_DECIMATIONth batch (~40.7 Hz effective at
 *      the defaults), spanning ~12.6 s -- for diagnosing drift,
 *      degenerate-denominator excursions, or slow mechanical behaviour
 *      the short raw-ADC window can't reach. Same START_BULK/CANCEL_BULK
 *      shape and the same exclusivity with the real-time demod as 0x00.
 *      Chunk payload is [page:1][entry:34]xN, N <=
 *      DISPLACEMENT_PHASOR_LOG_CHUNK_ENTRIES; one entry is 8x float32 LE
 *      (iB,qB,iA,qA,iS1,qS1,iS2,qS2 -- same layout/units as Topic groups
 *      0x5 resource 0x02) plus a uint16 seq (the last raw cycle folded
 *      into that stored batch, for gap detection). */
#define API2_RES_BULK_PHASORS   0x01U

#define API2_OP_BULK_RAW_ADC_START \
    API2_OPCODE(API2_VERB_START_BULK, API2_CAT_BULK, API2_RES_BULK_RAW_ADC)
#define API2_OP_BULK_RAW_ADC_CANCEL \
    API2_OPCODE(API2_VERB_CANCEL_BULK, API2_CAT_BULK, API2_RES_BULK_RAW_ADC)
#define API2_OP_BULK_PHASORS_START \
    API2_OPCODE(API2_VERB_START_BULK, API2_CAT_BULK, API2_RES_BULK_PHASORS)
#define API2_OP_BULK_PHASORS_CANCEL \
    API2_OPCODE(API2_VERB_CANCEL_BULK, API2_CAT_BULK, API2_RES_BULK_PHASORS)

#endif /* SVC_API_H */
