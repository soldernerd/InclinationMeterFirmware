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

void svc_api_init(void);
void svc_api_update(void);   /* scheduler hook — currently only drains the debug-log push */

void svc_api_register_transport(ApiTransport t, ApiSendFn send_fn);
void svc_api_connected(ApiTransport t);
void svc_api_disconnected(ApiTransport t);

void svc_api_receive(ApiTransport t, const uint8_t *data, uint16_t len);

/* Walks all Measurements subscription slots and pushes the ones that are
 * due. Its own scheduler task (not folded into svc_api_update()) so a
 * 50 ms subscription interval isn't silently coarsened to the slower
 * generic poll cadence -- see the .c comment. */
void svc_api_measurement_subscriptions_update(void);

/* v1 vestige: Services/svc_measurement.c's single-shot state machine used
 * to notify the host here. v2 has no single-shot-measurement push in the
 * ported subset; kept as a no-op so svc_measurement.c still links.
 * Redesign against a Measurements SUBSCRIBE when single-shot comes back. */
void svc_api_notify_single_ready(void);

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

/* ---------------- System status (0x0, GET only) ---------------- */
#define API2_OP_SYS_GET_IDENTITY \
    API2_OPCODE(API2_VERB_GET, API2_CAT_SYSTEM_STATUS, 0x00U)
#define API2_OP_SYS_GET_DEVICE_STATE \
    API2_OPCODE(API2_VERB_GET, API2_CAT_SYSTEM_STATUS, 0x01U)

/* ---------------- Commands (0x1, EXECUTE only) ---------------- */
#define API2_OP_CMD_TEST_BEEP \
    API2_OPCODE(API2_VERB_EXECUTE, API2_CAT_COMMANDS, 0x00U)

/* ---------------- Measurements (0x4: GET, SUBSCRIBE, UNSUBSCRIBE) ----------------
 * Only what REV B actually reads today. */
#define API2_RES_MEAS_ONBOARD_TEMP   0x00U   /* int16 centi-degC, TMP236 */
#define API2_RES_MEAS_BATTERY_MV     0x01U   /* uint16 mV */
#define API2_RES_MEAS_BATTERY_SOC    0x02U   /* uint8 percent */

#define API2_MEASUREMENT_MIN_INTERVAL_MS 50U
#define API2_MEASUREMENT_MAX_INTERVAL_MS 3600000U   /* 1 hour */
#define API2_MEASUREMENT_SLOTS           16U        /* direct-indexed by resource id */

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

#endif /* SVC_API_H */
